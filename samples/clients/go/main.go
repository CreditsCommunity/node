// Command credits-client is a small client of the Credits node API (Thrift, binary protocol,
// buffered transport).
//
//	credits-client [-node HOST:PORT] status
//	credits-client [-node HOST:PORT] balance <address>
//	credits-client [-node HOST:PORT] blocks [n]
//	credits-client [-node HOST:PORT] tx <address> [n]
//	credits-client [-node HOST:PORT] transfer <key-file> <to-address> <amount>
//	credits-client sign <key-file> <to-address> <amount> <inner-id>
//
// transfer SIGNS AND SENDS A REAL TRANSACTION: on mainnet it spends real CS. Try it on a private
// or test network first.
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
	"strconv"
	"strings"
	"time"

	"github.com/apache/thrift/lib/go/thrift"
	"github.com/mr-tron/base58"

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

func usage() {
	fmt.Fprintln(os.Stderr, `usage: credits-client [-node HOST:PORT] <command>
  status                               last block, round, head hash and time, wallet count
  balance <address>                    balance of a wallet (base58 public key)
  blocks [n]                           the last n blocks (default 10)
  tx <address> [n]                     the last n transactions of a wallet (default 10)
  transfer <key-file> <to> <amount>    sign and send a transfer: SPENDS REAL CS ON MAINNET
  sign <key-file> <to> <amount> <id>   build and sign a transfer with inner id <id>, print it, send nothing`)
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

	socket := thrift.NewTSocketConf(*node, &thrift.TConfiguration{ConnectTimeout: 10 * time.Second, SocketTimeout: 30 * time.Second})
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
	default:
		usage()
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
