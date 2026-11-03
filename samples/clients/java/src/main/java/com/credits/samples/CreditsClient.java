package com.credits.samples;

import com.credits.client.node.thrift.generated.API;
import com.credits.client.node.thrift.generated.AmountCommission;
import com.credits.client.node.thrift.generated.ContractAllMethodsGetResult;
import com.credits.client.node.thrift.generated.Pool;
import com.credits.client.node.thrift.generated.PoolListGetResult;
import com.credits.client.node.thrift.generated.SealedTransaction;
import com.credits.client.node.thrift.generated.SmartContractCompileResult;
import com.credits.client.node.thrift.generated.SmartContractDeploy;
import com.credits.client.node.thrift.generated.SmartContractGetResult;
import com.credits.client.node.thrift.generated.SmartContractInvocation;
import com.credits.client.node.thrift.generated.SyncStateResult;
import com.credits.client.node.thrift.generated.Transaction;
import com.credits.client.node.thrift.generated.TransactionFlowResult;
import com.credits.client.node.thrift.generated.TransactionType;
import com.credits.client.node.thrift.generated.TransactionsGetResult;
import com.credits.client.node.thrift.generated.WalletBalanceGetResult;
import com.credits.client.node.thrift.generated.WalletTransactionsCountGetResult;
import com.credits.client.node.thrift.generated.WalletsGetResult;
import com.credits.general.thrift.generated.APIResponse;
import com.credits.general.thrift.generated.Amount;
import com.credits.general.thrift.generated.ByteCodeObject;
import com.credits.general.thrift.generated.MethodArgument;
import com.credits.general.thrift.generated.MethodDescription;
import com.credits.general.thrift.generated.Variant;
import org.apache.thrift.TException;
import org.apache.thrift.TSerializer;
import org.apache.thrift.protocol.TBinaryProtocol;
import org.apache.thrift.transport.TSocket;
import org.apache.thrift.transport.TTransport;
import org.bouncycastle.crypto.digests.Blake2sDigest;
import org.bouncycastle.crypto.params.Ed25519PrivateKeyParameters;
import org.bouncycastle.crypto.signers.Ed25519Signer;

import java.io.IOException;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HexFormat;
import java.util.List;
import java.util.stream.Collectors;

/**
 * A small client of the Credits node API (Thrift, binary protocol, buffered transport).
 *
 * <pre>
 *   java -jar credits-client.jar [--node HOST:PORT] status
 *   java -jar credits-client.jar [--node HOST:PORT] balance &lt;address&gt;
 *   java -jar credits-client.jar [--node HOST:PORT] blocks [n]
 *   java -jar credits-client.jar [--node HOST:PORT] tx &lt;address&gt; [n]
 *   java -jar credits-client.jar [--node HOST:PORT] transfer &lt;key-file&gt; &lt;to-address&gt; &lt;amount&gt;
 *   java -jar credits-client.jar sign &lt;key-file&gt; &lt;to-address&gt; &lt;amount&gt; &lt;inner-id&gt;
 *   java -jar credits-client.jar [--node HOST:PORT] deploy &lt;key-file&gt; &lt;Contract.java&gt;
 *   java -jar credits-client.jar [--node HOST:PORT] call &lt;key-file&gt; &lt;contract&gt; &lt;method&gt; [args...]
 *   java -jar credits-client.jar [--node HOST:PORT] get &lt;contract&gt; &lt;method&gt; [args...]
 *   java -jar credits-client.jar [--node HOST:PORT] methods &lt;contract&gt;
 * </pre>
 *
 * transfer, deploy and call SIGN AND SEND REAL TRANSACTIONS: on mainnet they spend real CS. Try
 * them on a private or test network first. sign builds a transfer offline and prints it; get runs
 * a contract method as a getter, which costs nothing and changes nothing.
 *
 * Contract arguments are given as text and converted to the type the method declares (the node
 * refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
 * their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.
 */
public final class CreditsClient {
    private static final byte CURRENCY_CS = 1;
    private static final BigDecimal FRACTION = BigDecimal.TEN.pow(18); // integral + fraction / 10^18
    /** The most the sender accepts to pay; the node charges the actual fee. */
    private static final short MAX_FEE = encodeMaxFee(1.0);
    /** Contracts cost more: a deploy, or a call that runs long. */
    private static final short CONTRACT_MAX_FEE = encodeMaxFee(10.0);
    private static final DateTimeFormatter TIME =
            DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss 'UTC'").withZone(ZoneOffset.UTC);
    private static final HexFormat HEX = HexFormat.of();

    private final API.Client client;

    private CreditsClient(API.Client client) {
        this.client = client;
    }

    /** csdb::AmountCommission(double) as its 16-bit pattern (5-bit exponent, 10-bit mantissa). */
    static short encodeMaxFee(double value) {
        double v = Math.abs(value);
        double expf = Math.log10(v);
        int expi = expf >= 0 ? (int) (expf + 0.5) : (int) (expf - 0.5);
        v /= Math.pow(10, expi);
        if (v >= 1.0) {
            v *= 0.1;
            expi++;
        }
        return (short) ((((expi + 18) & 0x1F) << 10) | ((int) Math.round(v * 1024) & 0x3FF));
    }

    static String formatAmount(Amount a) {
        BigDecimal value = BigDecimal.valueOf(a.getIntegral()).add(BigDecimal.valueOf(a.getFraction()).divide(FRACTION));
        return value.stripTrailingZeros().toPlainString();
    }

    /** "12.5" -> (12, 500000000000000000), exactly. */
    static Amount parseAmount(String text) {
        BigInteger units = new BigDecimal(text).multiply(FRACTION).toBigInteger();
        if (units.signum() <= 0) {
            throw new IllegalArgumentException(text + ": not a positive amount");
        }
        BigInteger[] parts = units.divideAndRemainder(FRACTION.toBigInteger());
        return new Amount(parts[0].intValueExact(), parts[1].longValueExact());
    }

    static String formatTime(long ms) {
        return TIME.format(Instant.ofEpochMilli(ms));
    }

    static byte[] address(String text) {
        byte[] key = Base58.decode(text);
        if (key.length != 32) {
            throw new IllegalArgumentException(text + ": not a base58 public key (32 bytes)");
        }
        return key;
    }

    /** A base58 ed25519 secret key as in the node's NodePrivate.txt: 64 bytes (seed + public key), or a 32-byte seed. */
    static Ed25519PrivateKeyParameters readKey(String path) throws IOException {
        byte[] secret = Base58.decode(Files.readString(Path.of(path)).trim());
        if (secret.length != 32 && secret.length != 64) {
            throw new IllegalArgumentException(path + ": expected a base58 ed25519 secret key of 64 bytes (or a 32-byte seed)");
        }
        Ed25519PrivateKeyParameters key = new Ed25519PrivateKeyParameters(secret, 0);
        if (secret.length == 64 && !Arrays.equals(key.generatePublicKey().getEncoded(), Arrays.copyOfRange(secret, 32, 64))) {
            throw new IllegalArgumentException(path + ": the public half does not match the seed");
        }
        return key;
    }

    private static void check(APIResponse status, String call) {
        if (status != null && status.getCode() != 0) {
            throw new IllegalStateException(call + ": " + status.getMessage() + " (code " + status.getCode() + ")");
        }
    }

    void status() throws TException {
        SyncStateResult s = client.SyncStateGet();
        PoolListGetResult pools = client.PoolListGet(0, 1);
        check(pools.getStatus(), "PoolListGet");
        // one wallet, by transaction count: the cheapest way to the total
        WalletsGetResult wallets = client.WalletsGet(0, 1, (byte) 2, true);
        Pool head = pools.getPools().get(0);
        System.out.println("last block   : " + s.getLastBlock());
        System.out.println("current round: " + s.getCurrRound());
        System.out.println("head hash    : " + HEX.formatHex(head.getHash()));
        System.out.println("head time    : " + formatTime(head.getTime()));
        System.out.println("wallets      : " + wallets.getCount());
    }

    void balance(String addr) throws TException {
        WalletBalanceGetResult b = client.WalletBalanceGet(ByteBuffer.wrap(address(addr)));
        check(b.getStatus(), "WalletBalanceGet");
        System.out.println(addr + ": " + formatAmount(b.getBalance()) + " CS");
    }

    void blocks(long n) throws TException {
        for (Pool p : client.PoolListGet(0, n).getPools()) {
            System.out.printf("%11d  %s  %4d tx  %s%n", p.getPoolNumber(), HEX.formatHex(p.getHash()),
                    p.getTransactionsCount(), formatTime(p.getTime()));
        }
    }

    void transactions(String addr, long n) throws TException {
        TransactionsGetResult r = client.TransactionsGet(ByteBuffer.wrap(address(addr)), 0, n);
        check(r.getStatus(), "TransactionsGet");
        System.out.printf("%d transactions, the last %d:%n", r.getTotal_trxns_count(), r.getTransactionsSize());
        for (SealedTransaction t : r.getTransactions()) {
            Transaction x = t.getTrxn();
            System.out.printf("%11d.%-4d %s  %s -> %s  %s CS  type %d%n", t.getId().getPoolSeq(), t.getId().getIndex(),
                    formatTime(x.getTimeCreation()), Base58.encode(x.getSource()), Base58.encode(x.getTarget()),
                    formatAmount(x.getAmount()), x.getType().getValue());
        }
    }

    /**
     * A signed CS transfer. The signature covers csdb::Transaction::to_byte_stream_for_sig(),
     * little endian, which is also stored in {@code signed[0]}.
     */
    static Transaction buildTransfer(Ed25519PrivateKeyParameters key, String to, String amountText, long innerId,
                                     byte[][] signed) {
        byte[] source = key.generatePublicKey().getEncoded();
        byte[] target = address(to);
        Amount amount = parseAmount(amountText);
        ByteBuffer b = ByteBuffer.allocate(6 + 32 + 32 + 4 + 8 + 2 + 1 + 1).order(ByteOrder.LITTLE_ENDIAN);
        b.put(Arrays.copyOf(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(innerId).array(), 6)); // inner id, 6 bytes
        b.put(source);                    // 32 bytes
        b.put(target);                    // 32 bytes
        b.putInt(amount.getIntegral());   // amount
        b.putLong(amount.getFraction());
        b.putShort(MAX_FEE);              // max fee
        b.put(CURRENCY_CS);               // currency
        b.put((byte) 0);                  // number of user fields
        signed[0] = b.array();

        Ed25519Signer signer = new Ed25519Signer();
        signer.init(true, key);
        signer.update(signed[0], 0, signed[0].length);

        Transaction t = new Transaction();
        t.setId(innerId);
        t.setSource(source);
        t.setTarget(target);
        t.setAmount(amount);
        t.setBalance(new Amount(0, 0));
        t.setCurrency(CURRENCY_CS);
        t.setSignature(signer.generateSignature());
        t.setFee(new AmountCommission(MAX_FEE));
        t.setTimeCreation(System.currentTimeMillis());
        t.setUserFields(new byte[0]);
        t.setType(TransactionType.TT_Transfer);
        t.setPoolNumber(0);
        return t;
    }

    static void sign(String keyFile, String to, String amount, long innerId) throws IOException {
        byte[][] signed = new byte[1][];
        Transaction t = buildTransfer(readKey(keyFile), to, amount, innerId, signed);
        System.out.println("source   : " + Base58.encode(t.getSource()));
        System.out.println("signed   : " + HEX.formatHex(signed[0]));
        System.out.println("signature: " + HEX.formatHex(t.getSignature()));
    }

    void transfer(String keyFile, String to, String amount) throws IOException, TException {
        Ed25519PrivateKeyParameters key = readKey(keyFile);
        // the inner id must be the sender's last one + 1 (replay protection); a wallet the node has
        // never seen answers "not found": its first transaction has inner id 1
        WalletTransactionsCountGetResult last =
                client.WalletTransactionsCountGet(ByteBuffer.wrap(key.generatePublicKey().getEncoded()));
        long innerId = (last.getStatus().getCode() == 0 ? last.getLastTransactionInnerId() : 0) + 1;
        Transaction t = buildTransfer(key, to, amount, innerId, new byte[1][]);
        TransactionFlowResult r = client.TransactionFlow(t);
        check(r.getStatus(), "TransactionFlow");
        System.out.printf("sent: inner id %d, round %d, fee %s CS%n", innerId, r.getRoundNum(), formatAmount(r.getFee()));
    }

    // ---- smart contracts ----------------------------------------------------------------------

    private long nextInnerId(byte[] pk) throws TException {
        // the sender's last inner id + 1 (replay protection); a wallet the node has never seen: 1
        WalletTransactionsCountGetResult last = client.WalletTransactionsCountGet(ByteBuffer.wrap(pk));
        return (last.getStatus().getCode() == 0 ? last.getLastTransactionInnerId() : 0) + 1;
    }

    static SmartContractInvocation invocation(String method, List<Variant> params, SmartContractDeploy deploy, boolean getter) {
        // every field the IDL does not mark optional is set: these are the bytes the node
        // serializes again to check the signature
        SmartContractInvocation sci = new SmartContractInvocation();
        sci.setMethod(method);
        sci.setParams(params);
        sci.setUsedContracts(new ArrayList<>());
        sci.setForgetNewState(getter);
        if (deploy != null) {
            sci.setSmartContractDeploy(deploy);
        }
        sci.setVersion((short) 1);
        return sci;
    }

    /**
     * A signed contract transaction (deploy or call). The invocation travels as the transaction's
     * one user field: its Thrift binary serialization, which the signature covers.
     */
    static Transaction contractTransaction(Ed25519PrivateKeyParameters key, byte[] target, SmartContractInvocation sci,
                                           TransactionType type, long innerId) throws TException {
        byte[] source = key.generatePublicKey().getEncoded();
        byte[] code = new TSerializer(new TBinaryProtocol.Factory()).serialize(sci);
        ByteBuffer b = ByteBuffer.allocate(6 + 32 + 32 + 4 + 8 + 2 + 1 + 1 + 4 + code.length).order(ByteOrder.LITTLE_ENDIAN);
        b.put(Arrays.copyOf(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(innerId).array(), 6));
        b.put(source);
        b.put(target);
        b.putInt(0);                      // amount: none
        b.putLong(0);
        b.putShort(CONTRACT_MAX_FEE);
        b.put(CURRENCY_CS);
        b.put((byte) 1);                  // one user field: u32 size + bytes
        b.putInt(code.length);
        b.put(code);
        Ed25519Signer signer = new Ed25519Signer();
        signer.init(true, key);
        signer.update(b.array(), 0, b.capacity());

        Transaction t = new Transaction();
        t.setId(innerId);
        t.setSource(source);
        t.setTarget(target);
        t.setAmount(new Amount(0, 0));
        t.setBalance(new Amount(0, 0));
        t.setCurrency(CURRENCY_CS);
        t.setSignature(signer.generateSignature());
        t.setSmartContract(sci);
        t.setFee(new AmountCommission(CONTRACT_MAX_FEE));
        t.setTimeCreation(System.currentTimeMillis());
        t.setType(type);
        return t;
    }

    /** The Variant the executor matches with a declared Java type. */
    static Variant variant(String declared, String text) {
        try {
            return switch (declared) {
                case "java.lang.String" -> Variant.v_string(text);
                case "java.math.BigDecimal" -> Variant.v_big_decimal(new BigDecimal(text).toString());
                case "int" -> Variant.v_int(Integer.parseInt(text));
                case "java.lang.Integer" -> Variant.v_int_box(Integer.parseInt(text));
                case "long" -> Variant.v_long(Long.parseLong(text));
                case "java.lang.Long" -> Variant.v_long_box(Long.parseLong(text));
                case "boolean" -> Variant.v_boolean(flag(text));
                case "java.lang.Boolean" -> Variant.v_boolean_box(flag(text));
                case "double" -> Variant.v_double(Double.parseDouble(text));
                case "java.lang.Double" -> Variant.v_double_box(Double.parseDouble(text));
                case "byte[]" -> Variant.v_byte_array(HEX.parseHex(text));
                default -> throw new IllegalArgumentException("arguments of type " + declared + " are not supported by this client");
            };
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("'" + text + "' is not a " + declared);
        }
    }

    private static boolean flag(String text) {
        return switch (text.toLowerCase()) {
            case "true" -> true;
            case "false" -> false;
            default -> throw new NumberFormatException(text);
        };
    }

    /** A returned Variant as text, the same way in every sample. */
    static String show(Variant v) {
        if (v == null || v.getSetField() == null) {
            return "(nothing)";
        }
        return switch (v.getSetField()) {
            case V_BYTE_ARRAY -> HEX.formatHex(v.getV_byte_array());
            case V_VOID -> "void";
            case V_NULL -> "null";
            default -> String.valueOf(v.getFieldValue());
        };
    }

    /** The public methods of a deployed contract, as the node's executor lists them from its byte code. */
    private List<MethodDescription> contractMethods(byte[] contract) throws TException {
        SmartContractGetResult deployed = client.SmartContractGet(ByteBuffer.wrap(contract));
        check(deployed.getStatus(), "SmartContractGet");
        ContractAllMethodsGetResult r =
                client.ContractAllMethodsGet(deployed.getSmartContract().getSmartContractDeploy().getByteCodeObjects());
        if (r.getCode() != 0) {
            throw new IllegalStateException("ContractAllMethodsGet: " + r.getMessage());
        }
        return r.getMethods();
    }

    private List<Variant> typedParams(byte[] contract, String method, String[] texts) throws TException {
        List<MethodDescription> candidates = contractMethods(contract).stream()
                .filter(m -> m.getName().equals(method)).collect(Collectors.toList());
        MethodDescription match = candidates.stream().filter(m -> m.getArgumentsSize() == texts.length).findFirst()
                .orElseThrow(() -> new IllegalArgumentException("no method " + method + " with " + texts.length + " arguments"
                        + (candidates.isEmpty() ? "" : "; there is " + candidates.stream()
                        .map(m -> m.getName() + "(" + m.getArguments().stream().map(MethodArgument::getType)
                                .collect(Collectors.joining(", ")) + ")").collect(Collectors.joining(", ")))));
        List<Variant> params = new ArrayList<>();
        for (int i = 0; i < texts.length; i++) {
            params.add(variant(match.getArguments().get(i).getType(), texts[i]));
        }
        return params;
    }

    void methods(String contract) throws TException {
        List<MethodDescription> all = new ArrayList<>(contractMethods(address(contract)));
        all.sort(Comparator.comparing(MethodDescription::getName));
        for (MethodDescription m : all) {
            System.out.println(m.getReturnType() + " " + m.getName() + "(" + m.getArguments().stream()
                    .map(a -> a.getType() + " " + a.getName()).collect(Collectors.joining(", ")) + ")");
        }
    }

    void deploy(String keyFile, String sourceFile) throws IOException, TException, InterruptedException {
        Ed25519PrivateKeyParameters key = readKey(keyFile);
        byte[] pk = key.generatePublicKey().getEncoded();
        String source = Files.readString(Path.of(sourceFile));
        // the node's executor compiles the source and checks it against its rules
        SmartContractCompileResult compiled = client.SmartContractCompile(source);
        check(compiled.getStatus(), "SmartContractCompile");
        List<ByteCodeObject> classes = compiled.getByteCodeObjects();
        System.out.printf("compiled: %d class(es), token standard %d%n", classes.size(), compiled.getTokenStandard());
        long innerId = nextInnerId(pk);
        // the address of the new contract (SmartContracts::get_valid_smart_address):
        // blake2s-256(deployer public key, 6-byte inner id, the byte code of every class in order)
        Blake2sDigest blake = new Blake2sDigest(256);
        blake.update(pk, 0, pk.length);
        blake.update(Arrays.copyOf(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(innerId).array(), 6), 0, 6);
        for (ByteCodeObject c : classes) {
            blake.update(c.getByteCode(), 0, c.getByteCode().length);
        }
        byte[] target = new byte[32];
        blake.doFinal(target, 0);

        SmartContractDeploy deploy = new SmartContractDeploy();
        deploy.setSourceCode(source);
        deploy.setByteCodeObjects(classes);
        deploy.setHashState("");
        deploy.setTokenStandard(compiled.getTokenStandard());
        deploy.setLang((byte) 0);
        Transaction t = contractTransaction(key, target, invocation("", new ArrayList<>(), deploy, false),
                TransactionType.TT_ContractDeploy, innerId);
        TransactionFlowResult r = client.TransactionFlow(t);
        check(r.getStatus(), "TransactionFlow");
        System.out.printf("sent: inner id %d, round %d, fee %s CS%n", innerId, r.getRoundNum(), formatAmount(r.getFee()));
        // the contract exists once the deploy is in a block and its executions are done
        long deadline = System.currentTimeMillis() + 180_000;
        while (client.SmartContractGet(ByteBuffer.wrap(target)).getStatus().getCode() != 0) {
            if (System.currentTimeMillis() > deadline) {
                throw new IllegalStateException(Base58.encode(target) + " is not deployed after 180 s");
            }
            Thread.sleep(2000);
        }
        System.out.println("contract: " + Base58.encode(target));
    }

    void call(String keyFile, String contractText, String method, String[] texts) throws IOException, TException {
        Ed25519PrivateKeyParameters key = readKey(keyFile);
        byte[] contract = address(contractText);
        List<Variant> params = typedParams(contract, method, texts);
        long innerId = nextInnerId(key.generatePublicKey().getEncoded());
        Transaction t = contractTransaction(key, contract, invocation(method, params, null, false),
                TransactionType.TT_ContractCall, innerId);
        // the node answers once the confidants executed the call: the result is the method's return value
        TransactionFlowResult r = client.TransactionFlow(t);
        check(r.getStatus(), "TransactionFlow");
        System.out.printf("sent: inner id %d, round %d, fee %s CS%n", innerId, r.getRoundNum(), formatAmount(r.getFee()));
        System.out.println("returned: " + show(r.getSmart_contract_result()));
    }

    void get(String contractText, String method, String[] texts) throws TException {
        byte[] contract = address(contractText);
        List<Variant> params = typedParams(contract, method, texts);
        // a getter: TransactionFlow with forgetNewState runs the method on the node's own executor against
        // the current state and throws the new state away; nothing is sent, no fee, no signature is checked
        Transaction t = new Transaction();
        t.setId(0);
        t.setSource(new byte[32]);
        t.setTarget(contract);
        t.setAmount(new Amount(0, 0));
        t.setBalance(new Amount(0, 0));
        t.setCurrency(CURRENCY_CS);
        t.setSignature(new byte[64]);
        t.setSmartContract(invocation(method, params, null, true));
        t.setFee(new AmountCommission(CONTRACT_MAX_FEE));
        t.setTimeCreation(System.currentTimeMillis());
        t.setType(TransactionType.TT_ContractCall);
        TransactionFlowResult r = client.TransactionFlow(t);
        check(r.getStatus(), "TransactionFlow");
        System.out.println(show(r.getSmart_contract_result()));
    }

    private static void usage() {
        System.err.println("""
                usage: java -jar credits-client.jar [--node HOST:PORT] <command>
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
                  methods <contract>                   a contract's methods and their argument types
                --node defaults to 127.0.0.1:9090""");
        System.exit(2);
    }

    public static void main(String[] argv) {
        String node = "127.0.0.1:9090";
        int i = 0;
        if (argv.length >= 2 && argv[0].equals("--node")) {
            node = argv[1];
            i = 2;
        }
        String[] args = Arrays.copyOfRange(argv, i, argv.length);
        if (args.length == 0) {
            usage();
        }
        try {
            if (args[0].equals("sign") && args.length == 5) {
                sign(args[1], args[2], args[3], Long.parseLong(args[4]));
                return;
            }
            int colon = node.lastIndexOf(':');
            // TSocket buffers its streams: this is the "buffered transport" the node expects (no framing)
            try (TTransport transport = new TSocket(node.substring(0, colon), Integer.parseInt(node.substring(colon + 1)), 120_000)) {
                transport.open();
                CreditsClient c = new CreditsClient(new API.Client(new TBinaryProtocol(transport)));
                switch (args[0]) {
                    case "status" -> c.status();
                    case "balance" -> {
                        if (args.length != 2) usage();
                        c.balance(args[1]);
                    }
                    case "blocks" -> {
                        if (args.length > 2) usage();
                        c.blocks(args.length == 2 ? Long.parseLong(args[1]) : 10);
                    }
                    case "tx" -> {
                        if (args.length < 2 || args.length > 3) usage();
                        c.transactions(args[1], args.length == 3 ? Long.parseLong(args[2]) : 10);
                    }
                    case "transfer" -> {
                        if (args.length != 4) usage();
                        c.transfer(args[1], args[2], args[3]);
                    }
                    case "deploy" -> {
                        if (args.length != 3) usage();
                        c.deploy(args[1], args[2]);
                    }
                    case "call" -> {
                        if (args.length < 4) usage();
                        c.call(args[1], args[2], args[3], Arrays.copyOfRange(args, 4, args.length));
                    }
                    case "get" -> {
                        if (args.length < 3) usage();
                        c.get(args[1], args[2], Arrays.copyOfRange(args, 3, args.length));
                    }
                    case "methods" -> {
                        if (args.length != 2) usage();
                        c.methods(args[1]);
                    }
                    default -> usage();
                }
            }
        } catch (Exception e) {
            System.err.println(e.getMessage() != null ? e.getMessage() : e.toString());
            System.exit(1);
        }
    }
}
