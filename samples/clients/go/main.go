// Command credits-client is a small client of the Credits node API (Thrift, binary protocol,
// buffered transport).
//
//	credits-client [-node HOST:PORT] status
//	credits-client [-node HOST:PORT] balance <address>
//	credits-client [-node HOST:PORT] blocks [n]
//	credits-client [-node HOST:PORT] tx <address> [n]
//	credits-client [-node HOST:PORT] transfer <key-file> <to-address> <amount>
//	credits-client sign <key-file> <to-address> <amount> <inner-id>
//	credits-client [-node HOST:PORT] deploy <key-file> <Contract.java>
//	credits-client [-node HOST:PORT] call <key-file> <contract> <method> [args...]
//	credits-client [-node HOST:PORT] get <contract> <method> [args...]
//	credits-client [-node HOST:PORT] methods <contract>
//
// transfer, deploy and call SIGN AND SEND REAL TRANSACTIONS: on mainnet they spend real CS. Try
// them on a private or test network first. sign builds a transfer offline and prints it; get runs
// a contract method as a getter, which costs nothing and changes nothing.
//
// Contract arguments are given as text and converted to the type the method declares (the node
// refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
// their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.
package main

//go:generate sh -c "rm -rf gen && mkdir gen && thrift -r --gen go:package_prefix=github.com/CreditsCommunity/node/samples/clients/go/gen/,skip_remote -out gen -I ../../../third-party/thrift-interface-definitions ../../../third-party/thrift-interface-definitions/api.thrift"

import (
	"context"
	"crypto/ed25519"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"math"
	"math/big"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/apache/thrift/lib/go/thrift"
	"github.com/mr-tron/base58"
	"golang.org/x/crypto/blake2s"

	"github.com/CreditsCommunity/node/samples/clients/go/gen/api"
	"github.com/CreditsCommunity/node/samples/clients/go/gen/general"
)

const (
	currencyCS = 1
	fraction   = 1_000_000_000_000_000_000 // general.Amount: integral + fraction / 10^18
)

// encodeMaxFee mirrors csdb::AmountCommission(double): a 16-bit pattern with a 5-bit exponent and
// a 10-bit mantissa.
func encodeMaxFee(value float64) int16 {
	v := math.Abs(value)
	expf := math.Log10(v)
	var expi int
	if expf >= 0 {
		expi = int(expf + 0.5)
	} else {
		expi = int(expf - 0.5)
	}
	v /= math.Pow(10, float64(expi))
	if v >= 1.0 {
		v *= 0.1
		expi++
	}
	return int16((((expi + 18) & 0x1F) << 10) | (int(math.Round(v*1024)) & 0x3FF))
}

// the most the sender accepts to pay; the node charges the actual fee
var maxFee = encodeMaxFee(1.0)

// contracts cost more: a deploy, or a call that runs long
var contractMaxFee = encodeMaxFee(10.0)

func fmtAmount(a *general.Amount) string {
	units := new(big.Int).Mul(big.NewInt(int64(a.Integral)), big.NewInt(fraction))
	units.Add(units, big.NewInt(a.Fraction))
	text := new(big.Rat).SetFrac(units, big.NewInt(fraction)).FloatString(18)
	return strings.TrimRight(strings.TrimRight(text, "0"), ".")
}

// parseAmount turns "12.5" into (12, 500000000000000000), exactly.
func parseAmount(text string) (int32, int64, error) {
	r, ok := new(big.Rat).SetString(text)
	if !ok || r.Sign() <= 0 {
		return 0, 0, fmt.Errorf("%q: not a positive amount", text)
	}
	units := new(big.Int).Quo(new(big.Int).Mul(r.Num(), big.NewInt(fraction)), r.Denom())
	integral, frac := new(big.Int).QuoRem(units, big.NewInt(fraction), new(big.Int))
	if !integral.IsInt64() || integral.Int64() > math.MaxInt32 {
		return 0, 0, fmt.Errorf("%q: too large", text)
	}
	return int32(integral.Int64()), frac.Int64(), nil
}

func fmtTime(ms int64) string {
	return time.UnixMilli(ms).UTC().Format("2006-01-02 15:04:05 UTC")
}

func address(text string) ([]byte, error) {
	key, err := base58.Decode(text)
	if err != nil || len(key) != ed25519.PublicKeySize {
		return nil, fmt.Errorf("%s: not a base58 public key (32 bytes)", text)
	}
	return key, nil
}

// readKey reads a base58 ed25519 secret key as in the node's NodePrivate.txt: 64 bytes (seed +
// public key), or a bare 32-byte seed.
func readKey(path string) (ed25519.PrivateKey, error) {
	text, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	secret, err := base58.Decode(strings.TrimSpace(string(text)))
	if err != nil || (len(secret) != 32 && len(secret) != 64) {
		return nil, fmt.Errorf("%s: expected a base58 ed25519 secret key of 64 bytes (or a 32-byte seed)", path)
	}
	key := ed25519.NewKeyFromSeed(secret[:32])
	if len(secret) == 64 && !key.Public().(ed25519.PublicKey).Equal(ed25519.PublicKey(secret[32:])) {
		return nil, fmt.Errorf("%s: the public half does not match the seed", path)
	}
	return key, nil
}

func check(status *general.APIResponse, call string) error {
	if status != nil && status.Code != 0 {
		return fmt.Errorf("%s: %s (code %d)", call, status.Message, status.Code)
	}
	return nil
}

func status(ctx context.Context, c *api.APIClient) error {
	s, err := c.SyncStateGet(ctx)
	if err != nil {
		return err
	}
	pools, err := c.PoolListGet(ctx, 0, 1)
	if err != nil {
		return err
	}
	if err := check(pools.Status, "PoolListGet"); err != nil || len(pools.Pools) == 0 {
		return errors.Join(err, errors.New("no blocks"))
	}
	// one wallet, by transaction count: the cheapest way to the total
	wallets, err := c.WalletsGet(ctx, 0, 1, 2, true)
	if err != nil {
		return err
	}
	head := pools.Pools[0]
	fmt.Printf("last block   : %d\n", s.LastBlock)
	fmt.Printf("current round: %d\n", s.CurrRound)
	fmt.Printf("head hash    : %s\n", hex.EncodeToString(head.Hash))
	fmt.Printf("head time    : %s\n", fmtTime(int64(head.Time)))
	fmt.Printf("wallets      : %d\n", wallets.Count)
	return nil
}

func balance(ctx context.Context, c *api.APIClient, addr string) error {
	key, err := address(addr)
	if err != nil {
		return err
	}
	b, err := c.WalletBalanceGet(ctx, key)
	if err != nil {
		return err
	}
	if err := check(b.Status, "WalletBalanceGet"); err != nil {
		return err
	}
	fmt.Printf("%s: %s CS\n", addr, fmtAmount(b.Balance))
	return nil
}

func blocks(ctx context.Context, c *api.APIClient, n int64) error {
	r, err := c.PoolListGet(ctx, 0, n)
	if err != nil {
		return err
	}
	for _, p := range r.Pools {
		fmt.Printf("%11d  %s  %4d tx  %s\n", p.PoolNumber, hex.EncodeToString(p.Hash), p.TransactionsCount, fmtTime(int64(p.Time)))
	}
	return nil
}

func transactions(ctx context.Context, c *api.APIClient, addr string, n int64) error {
	key, err := address(addr)
	if err != nil {
		return err
	}
	r, err := c.TransactionsGet(ctx, key, 0, n)
	if err != nil {
		return err
	}
	if err := check(r.Status, "TransactionsGet"); err != nil {
		return err
	}
	fmt.Printf("%d transactions, the last %d:\n", r.TotalTrxnsCount, len(r.Transactions))
	for _, t := range r.Transactions {
		x := t.Trxn
		fmt.Printf("%11d.%-4d %s  %s -> %s  %s CS  type %d\n", t.ID.PoolSeq, t.ID.Index, fmtTime(int64(x.TimeCreation)),
			base58.Encode(x.Source), base58.Encode(x.Target), fmtAmount(x.Amount), x.Type)
	}
	return nil
}

// buildTransfer builds and signs a CS transfer. The signature covers
// csdb::Transaction::to_byte_stream_for_sig(), little endian, which is returned too.
func buildTransfer(key ed25519.PrivateKey, to, amount string, innerID int64) (*api.Transaction, []byte, error) {
	source := []byte(key.Public().(ed25519.PublicKey))
	target, err := address(to)
	if err != nil {
		return nil, nil, err
	}
	integral, frac, err := parseAmount(amount)
	if err != nil {
		return nil, nil, err
	}
	signed := make([]byte, 0, 86)
	signed = binary.LittleEndian.AppendUint64(signed, uint64(innerID))[:6] // inner id, 6 bytes
	signed = append(signed, source...)                                     // 32 bytes
	signed = append(signed, target...)                                     // 32 bytes
	signed = binary.LittleEndian.AppendUint32(signed, uint32(integral))    // amount
	signed = binary.LittleEndian.AppendUint64(signed, uint64(frac))
	signed = binary.LittleEndian.AppendUint16(signed, uint16(maxFee)) // max fee
	signed = append(signed, currencyCS)                               // currency
	signed = append(signed, 0)                                        // number of user fields

	t := api.NewTransaction()
	t.ID = api.TransactionInnerId(innerID)
	t.Source = source
	t.Target = target
	t.Amount = &general.Amount{Integral: integral, Fraction: frac}
	t.Balance = &general.Amount{}
	t.Currency = currencyCS
	t.Signature = ed25519.Sign(key, signed)
	t.Fee = &api.AmountCommission{Commission: maxFee}
	t.TimeCreation = api.Time(time.Now().UnixMilli())
	t.UserFields = []byte{}
	t.Type = api.TransactionType_TT_Transfer
	return t, signed, nil
}

// sign prints what transfer would send, without connecting to a node.
func sign(keyFile, to, amount, inner string) error {
	key, err := readKey(keyFile)
	if err != nil {
		return err
	}
	innerID, err := strconv.ParseInt(inner, 10, 64)
	if err != nil {
		return err
	}
	t, signed, err := buildTransfer(key, to, amount, innerID)
	if err != nil {
		return err
	}
	fmt.Printf("source   : %s\n", base58.Encode(t.Source))
	fmt.Printf("signed   : %s\n", hex.EncodeToString(signed))
	fmt.Printf("signature: %s\n", hex.EncodeToString(t.Signature))
	return nil
}

func transfer(ctx context.Context, c *api.APIClient, keyFile, to, amount string) error {
	key, err := readKey(keyFile)
	if err != nil {
		return err
	}
	// the inner id must be the sender's last one + 1 (replay protection); a wallet the node has
	// never seen answers "not found": its first transaction has inner id 1
	last, err := c.WalletTransactionsCountGet(ctx, []byte(key.Public().(ed25519.PublicKey)))
	if err != nil {
		return err
	}
	innerID := int64(1)
	if last.Status.Code == 0 {
		innerID = int64(last.LastTransactionInnerId) + 1
	}
	t, _, err := buildTransfer(key, to, amount, innerID)
	if err != nil {
		return err
	}
	r, err := c.TransactionFlow(ctx, t)
	if err != nil {
		return err
	}
	if err := check(r.Status, "TransactionFlow"); err != nil {
		return err
	}
	fmt.Printf("sent: inner id %d, round %d, fee %s CS\n", innerID, r.RoundNum, fmtAmount(r.Fee))
	return nil
}

// ---- smart contracts ------------------------------------------------------------------------------

// nextInnerID is the sender's last inner id + 1 (replay protection); a wallet the node has never
// seen: 1.
func nextInnerID(ctx context.Context, c *api.APIClient, pk []byte) (int64, error) {
	last, err := c.WalletTransactionsCountGet(ctx, pk)
	if err != nil {
		return 0, err
	}
	if last.Status.Code == 0 {
		return int64(last.LastTransactionInnerId) + 1, nil
	}
	return 1, nil
}

// invocation builds a SmartContractInvocation. Every field the IDL does not mark optional is set,
// so these are the bytes the node serializes again to check the signature.
func invocation(method string, params []*general.Variant, deploy *api.SmartContractDeploy, getter bool) *api.SmartContractInvocation {
	if params == nil {
		params = []*general.Variant{}
	}
	sci := api.NewSmartContractInvocation()
	sci.Method = method
	sci.Params = params
	sci.UsedContracts = []general.Address{}
	sci.ForgetNewState = getter
	sci.SmartContractDeploy = deploy
	sci.Version = 1
	return sci
}

// contractTransaction builds a signed contract transaction (deploy or call). The invocation
// travels as the transaction's one user field: its Thrift binary serialization, which the
// signature covers.
func contractTransaction(key ed25519.PrivateKey, target []byte, sci *api.SmartContractInvocation, txType api.TransactionType, innerID int64) (*api.Transaction, error) {
	source := []byte(key.Public().(ed25519.PublicKey))
	code, err := thrift.NewTSerializer().Write(context.Background(), sci)
	if err != nil {
		return nil, err
	}
	signed := make([]byte, 0, 86+4+len(code))
	signed = binary.LittleEndian.AppendUint64(signed, uint64(innerID))[:6] // inner id, 6 bytes
	signed = append(signed, source...)
	signed = append(signed, target...)
	signed = binary.LittleEndian.AppendUint32(signed, 0) // amount: none
	signed = binary.LittleEndian.AppendUint64(signed, 0)
	signed = binary.LittleEndian.AppendUint16(signed, uint16(contractMaxFee))
	signed = append(signed, currencyCS)
	signed = append(signed, 1) // one user field: u32 size + bytes
	signed = binary.LittleEndian.AppendUint32(signed, uint32(len(code)))
	signed = append(signed, code...)

	t := api.NewTransaction()
	t.ID = api.TransactionInnerId(innerID)
	t.Source = source
	t.Target = target
	t.Amount = &general.Amount{}
	t.Balance = &general.Amount{}
	t.Currency = currencyCS
	t.Signature = ed25519.Sign(key, signed)
	t.SmartContract = sci
	t.Fee = &api.AmountCommission{Commission: contractMaxFee}
	t.TimeCreation = api.Time(time.Now().UnixMilli())
	t.Type = txType
	return t, nil
}

var decimalText = regexp.MustCompile(`^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$`)

func flag01(text string) (bool, error) {
	switch strings.ToLower(text) {
	case "true":
		return true, nil
	case "false":
		return false, nil
	}
	return false, errors.New("not a boolean")
}

// variant is the Variant the executor matches with a declared Java type.
func variant(declared, text string) (*general.Variant, error) {
	v := general.NewVariant()
	var err error
	switch declared {
	case "java.lang.String":
		v.VString = &text
	case "java.math.BigDecimal":
		if !decimalText.MatchString(text) {
			err = errors.New("not a number")
		}
		d := strings.TrimPrefix(text, "+")
		v.VBigDecimal = &d
	case "int", "java.lang.Integer":
		var n int64
		if n, err = strconv.ParseInt(text, 10, 32); err == nil {
			i := int32(n)
			if declared == "int" {
				v.VInt = &i
			} else {
				v.VIntBox = &i
			}
		}
	case "long", "java.lang.Long":
		var n int64
		if n, err = strconv.ParseInt(text, 10, 64); err == nil {
			if declared == "long" {
				v.VLong = &n
			} else {
				v.VLongBox = &n
			}
		}
	case "boolean", "java.lang.Boolean":
		var b bool
		if b, err = flag01(text); err == nil {
			if declared == "boolean" {
				v.VBoolean = &b
			} else {
				v.VBooleanBox = &b
			}
		}
	case "double", "java.lang.Double":
		var f float64
		if f, err = strconv.ParseFloat(text, 64); err == nil {
			if declared == "double" {
				v.VDouble = &f
			} else {
				v.VDoubleBox = &f
			}
		}
	case "byte[]":
		var b []byte
		if b, err = hex.DecodeString(text); err == nil {
			v.VByteArray = b
		}
	default:
		return nil, fmt.Errorf("arguments of type %s are not supported by this client", declared)
	}
	if err != nil {
		return nil, fmt.Errorf("%q is not a %s", text, declared)
	}
	return v, nil
}

func floatText(f float64) string {
	s := strconv.FormatFloat(f, 'g', -1, 64)
	if !strings.ContainsAny(s, "eIN.") {
		s += ".0"
	}
	return s
}

// show renders a returned Variant as text, the same way in every sample.
func show(v *general.Variant) string {
	switch {
	case v == nil:
		return "(nothing)"
	case v.VBoolean != nil:
		return strconv.FormatBool(*v.VBoolean)
	case v.VBooleanBox != nil:
		return strconv.FormatBool(*v.VBooleanBox)
	case v.VString != nil:
		return *v.VString
	case v.VBigDecimal != nil:
		return *v.VBigDecimal
	case v.VByte != nil:
		return strconv.Itoa(int(*v.VByte))
	case v.VByteBox != nil:
		return strconv.Itoa(int(*v.VByteBox))
	case v.VShort != nil:
		return strconv.Itoa(int(*v.VShort))
	case v.VShortBox != nil:
		return strconv.Itoa(int(*v.VShortBox))
	case v.VInt != nil:
		return strconv.Itoa(int(*v.VInt))
	case v.VIntBox != nil:
		return strconv.Itoa(int(*v.VIntBox))
	case v.VLong != nil:
		return strconv.FormatInt(*v.VLong, 10)
	case v.VLongBox != nil:
		return strconv.FormatInt(*v.VLongBox, 10)
	case v.VFloat != nil:
		return floatText(*v.VFloat)
	case v.VFloatBox != nil:
		return floatText(*v.VFloatBox)
	case v.VDouble != nil:
		return floatText(*v.VDouble)
	case v.VDoubleBox != nil:
		return floatText(*v.VDoubleBox)
	case v.VByteArray != nil:
		return hex.EncodeToString(v.VByteArray)
	case v.VVoid != nil:
		return "void"
	case v.VNull != nil:
		return "null"
	}
	return "(nothing)"
}

// contractMethods lists the public methods of a deployed contract, as the node's executor reads
// them from its byte code.
func contractMethods(ctx context.Context, c *api.APIClient, contract []byte) ([]*general.MethodDescription, error) {
	deployed, err := c.SmartContractGet(ctx, contract)
	if err != nil {
		return nil, err
	}
	if err := check(deployed.Status, "SmartContractGet"); err != nil {
		return nil, err
	}
	if deployed.SmartContract == nil || deployed.SmartContract.SmartContractDeploy == nil {
		return nil, errors.New("SmartContractGet: no deploy data")
	}
	r, err := c.ContractAllMethodsGet(ctx, deployed.SmartContract.SmartContractDeploy.ByteCodeObjects)
	if err != nil {
		return nil, err
	}
	if r.Code != 0 {
		return nil, fmt.Errorf("ContractAllMethodsGet: %s", r.Message)
	}
	return r.Methods, nil
}

func typedParams(ctx context.Context, c *api.APIClient, contract []byte, method string, texts []string) ([]*general.Variant, error) {
	all, err := contractMethods(ctx, c, contract)
	if err != nil {
		return nil, err
	}
	var candidates []*general.MethodDescription
	for _, m := range all {
		if m.Name == method {
			candidates = append(candidates, m)
		}
	}
	for _, m := range candidates {
		if len(m.Arguments) != len(texts) {
			continue
		}
		params := make([]*general.Variant, 0, len(texts))
		for i, a := range m.Arguments {
			v, err := variant(a.Type, texts[i])
			if err != nil {
				return nil, err
			}
			params = append(params, v)
		}
		return params, nil
	}
	known := make([]string, 0, len(candidates))
	for _, m := range candidates {
		types := make([]string, 0, len(m.Arguments))
		for _, a := range m.Arguments {
			types = append(types, a.Type)
		}
		known = append(known, fmt.Sprintf("%s(%s)", m.Name, strings.Join(types, ", ")))
	}
	msg := fmt.Sprintf("no method %s with %d arguments", method, len(texts))
	if len(known) > 0 {
		msg += "; there is " + strings.Join(known, ", ")
	}
	return nil, errors.New(msg)
}

func methods(ctx context.Context, c *api.APIClient, contractText string) error {
	contract, err := address(contractText)
	if err != nil {
		return err
	}
	all, err := contractMethods(ctx, c, contract)
	if err != nil {
		return err
	}
	sort.SliceStable(all, func(i, j int) bool { return all[i].Name < all[j].Name })
	for _, m := range all {
		args := make([]string, 0, len(m.Arguments))
		for _, a := range m.Arguments {
			args = append(args, a.Type+" "+a.Name)
		}
		fmt.Printf("%s %s(%s)\n", m.ReturnType, m.Name, strings.Join(args, ", "))
	}
	return nil
}

func deploy(ctx context.Context, c *api.APIClient, keyFile, sourceFile string) error {
	key, err := readKey(keyFile)
	if err != nil {
		return err
	}
	pk := []byte(key.Public().(ed25519.PublicKey))
	text, err := os.ReadFile(sourceFile)
	if err != nil {
		return err
	}
	source := string(text)
	// the node's executor compiles the source and checks it against its rules
	compiled, err := c.SmartContractCompile(ctx, source)
	if err != nil {
		return err
	}
	if err := check(compiled.Status, "SmartContractCompile"); err != nil {
		return err
	}
	classes := compiled.ByteCodeObjects
	fmt.Printf("compiled: %d class(es), token standard %d\n", len(classes), compiled.TokenStandard)
	innerID, err := nextInnerID(ctx, c, pk)
	if err != nil {
		return err
	}
	// the address of the new contract (SmartContracts::get_valid_smart_address):
	// blake2s-256(deployer public key, 6-byte inner id, the byte code of every class in order)
	h, _ := blake2s.New256(nil)
	h.Write(pk)
	h.Write(binary.LittleEndian.AppendUint64(nil, uint64(innerID))[:6])
	for _, class := range classes {
		h.Write(class.ByteCode)
	}
	target := h.Sum(nil)

	sci := invocation("", nil, &api.SmartContractDeploy{
		SourceCode: source, ByteCodeObjects: classes, HashState: "", TokenStandard: compiled.TokenStandard, Lang: 0,
	}, false)
	t, err := contractTransaction(key, target, sci, api.TransactionType_TT_ContractDeploy, innerID)
	if err != nil {
		return err
	}
	r, err := c.TransactionFlow(ctx, t)
	if err != nil {
		return err
	}
	if err := check(r.Status, "TransactionFlow"); err != nil {
		return err
	}
	fmt.Printf("sent: inner id %d, round %d, fee %s CS\n", innerID, r.RoundNum, fmtAmount(r.Fee))
	// the contract exists once the deploy is in a block and its executions are done
	deadline := time.Now().Add(180 * time.Second)
	for {
		got, err := c.SmartContractGet(ctx, target)
		if err != nil {
			return err
		}
		if got.Status.Code == 0 {
			break
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("%s is not deployed after 180 s", base58.Encode(target))
		}
		time.Sleep(2 * time.Second)
	}
	fmt.Printf("contract: %s\n", base58.Encode(target))
	return nil
}

func call(ctx context.Context, c *api.APIClient, keyFile, contractText, method string, texts []string) error {
	key, err := readKey(keyFile)
	if err != nil {
		return err
	}
	contract, err := address(contractText)
	if err != nil {
		return err
	}
	params, err := typedParams(ctx, c, contract, method, texts)
	if err != nil {
		return err
	}
	innerID, err := nextInnerID(ctx, c, []byte(key.Public().(ed25519.PublicKey)))
	if err != nil {
		return err
	}
	t, err := contractTransaction(key, contract, invocation(method, params, nil, false), api.TransactionType_TT_ContractCall, innerID)
	if err != nil {
		return err
	}
	// the node answers once the confidants executed the call: the result is the method's return value
	r, err := c.TransactionFlow(ctx, t)
	if err != nil {
		return err
	}
	if err := check(r.Status, "TransactionFlow"); err != nil {
		return err
	}
	fmt.Printf("sent: inner id %d, round %d, fee %s CS\n", innerID, r.RoundNum, fmtAmount(r.Fee))
	fmt.Printf("returned: %s\n", show(r.SmartContractResult_))
	return nil
}

func get(ctx context.Context, c *api.APIClient, contractText, method string, texts []string) error {
	contract, err := address(contractText)
	if err != nil {
		return err
	}
	params, err := typedParams(ctx, c, contract, method, texts)
	if err != nil {
		return err
	}
	// a getter: TransactionFlow with forgetNewState runs the method on the node's own executor against
	// the current state and throws the new state away; nothing is sent, no fee, no signature is checked
	t := api.NewTransaction()
	t.Source = make([]byte, 32)
	t.Target = contract
	t.Amount = &general.Amount{}
	t.Balance = &general.Amount{}
	t.Currency = currencyCS
	t.Signature = make([]byte, 64)
	t.SmartContract = invocation(method, params, nil, true)
	t.Fee = &api.AmountCommission{Commission: contractMaxFee}
	t.TimeCreation = api.Time(time.Now().UnixMilli())
	t.Type = api.TransactionType_TT_ContractCall
	r, err := c.TransactionFlow(ctx, t)
	if err != nil {
		return err
	}
	if err := check(r.Status, "TransactionFlow"); err != nil {
		return err
	}
	fmt.Println(show(r.SmartContractResult_))
	return nil
}

func usage() {
	fmt.Fprintln(os.Stderr, `usage: credits-client [-node HOST:PORT] <command>
  status                               last block, round, head hash and time, wallet count
  balance <address>                    balance of a wallet (base58 public key)
  blocks [n]                           the last n blocks (default 10)
  tx <address> [n]                     the last n transactions of a wallet (default 10)
  transfer <key-file> <to> <amount>    sign and send a transfer: SPENDS REAL CS ON MAINNET
  sign <key-file> <to> <amount> <id>   build and sign a transfer with inner id <id>, print it, send nothing
  deploy <key-file> <Contract.java>    compile and deploy a contract: SPENDS REAL CS ON MAINNET
  call <key-file> <contract> <method> [args...]
                                       call a method in a transaction: SPENDS REAL CS ON MAINNET
  get <contract> <method> [args...]    run a method as a getter: free, changes nothing
  methods <contract>                   a contract's methods and their argument types`)
	flag.PrintDefaults()
	os.Exit(2)
}

func count(args []string, i int) int64 {
	if len(args) <= i {
		return 10
	}
	n, err := strconv.ParseInt(args[i], 10, 64)
	if err != nil {
		usage()
	}
	return n
}

func main() {
	node := flag.String("node", "127.0.0.1:9090", "API endpoint, HOST:PORT")
	flag.Usage = usage
	flag.Parse()
	args := flag.Args()
	if len(args) == 0 {
		usage()
	}
	if args[0] == "sign" {
		if len(args) != 5 {
			usage()
		}
		if err := sign(args[1], args[2], args[3], args[4]); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}

	socket := thrift.NewTSocketConf(*node, &thrift.TConfiguration{ConnectTimeout: 10 * time.Second, SocketTimeout: 120 * time.Second})
	transport := thrift.NewTBufferedTransport(socket, 8192)
	if err := transport.Open(); err != nil {
		fmt.Fprintln(os.Stderr, "connect:", err)
		os.Exit(1)
	}
	defer transport.Close()
	c := api.NewAPIClientFactory(transport, thrift.NewTBinaryProtocolFactoryConf(nil))
	ctx := context.Background()

	var err error
	switch {
	case args[0] == "status":
		err = status(ctx, c)
	case args[0] == "balance" && len(args) == 2:
		err = balance(ctx, c, args[1])
	case args[0] == "blocks" && len(args) <= 2:
		err = blocks(ctx, c, count(args, 1))
	case args[0] == "tx" && (len(args) == 2 || len(args) == 3):
		err = transactions(ctx, c, args[1], count(args, 2))
	case args[0] == "transfer" && len(args) == 4:
		err = transfer(ctx, c, args[1], args[2], args[3])
	case args[0] == "deploy" && len(args) == 3:
		err = deploy(ctx, c, args[1], args[2])
	case args[0] == "call" && len(args) >= 4:
		err = call(ctx, c, args[1], args[2], args[3], args[4:])
	case args[0] == "get" && len(args) >= 3:
		err = get(ctx, c, args[1], args[2], args[3:])
	case args[0] == "methods" && len(args) == 2:
		err = methods(ctx, c, args[1])
	default:
		usage()
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
