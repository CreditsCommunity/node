package com.credits.samples;

import com.credits.client.node.thrift.generated.API;
import com.credits.client.node.thrift.generated.AmountCommission;
import com.credits.client.node.thrift.generated.Pool;
import com.credits.client.node.thrift.generated.PoolListGetResult;
import com.credits.client.node.thrift.generated.SealedTransaction;
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
import org.apache.thrift.TException;
import org.apache.thrift.protocol.TBinaryProtocol;
import org.apache.thrift.transport.TSocket;
import org.apache.thrift.transport.TTransport;
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
import java.util.Arrays;
import java.util.HexFormat;

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
 * </pre>
 *
 * transfer SIGNS AND SENDS A REAL TRANSACTION: on mainnet it spends real CS. Try it on a private
 * or test network first. sign builds the same transaction offline and prints it.
 */
public final class CreditsClient {
    private static final byte CURRENCY_CS = 1;
    private static final BigDecimal FRACTION = BigDecimal.TEN.pow(18); // integral + fraction / 10^18
    /** The most the sender accepts to pay; the node charges the actual fee. */
    private static final short MAX_FEE = encodeMaxFee(1.0);
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

    private static void usage() {
        System.err.println("""
                usage: java -jar credits-client.jar [--node HOST:PORT] <command>
                  status                               last block, round, head hash and time, wallet count
                  balance <address>                    balance of a wallet (base58 public key)
                  blocks [n]                           the last n blocks (default 10)
                  tx <address> [n]                     the last n transactions of a wallet (default 10)
                  transfer <key-file> <to> <amount>    sign and send a transfer: SPENDS REAL CS ON MAINNET
                  sign <key-file> <to> <amount> <id>   build and sign a transfer with inner id <id>, print it, send nothing
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
            try (TTransport transport = new TSocket(node.substring(0, colon), Integer.parseInt(node.substring(colon + 1)), 30_000)) {
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
                    default -> usage();
                }
            }
        } catch (Exception e) {
            System.err.println(e.getMessage() != null ? e.getMessage() : e.toString());
            System.exit(1);
        }
    }
}
