// A small client of the Credits node API (Thrift, binary protocol, buffered transport).
//
//   credits-client [--node HOST:PORT] status
//   credits-client [--node HOST:PORT] balance <address>
//   credits-client [--node HOST:PORT] blocks [n]
//   credits-client [--node HOST:PORT] tx <address> [n]
//   credits-client [--node HOST:PORT] transfer <key-file> <to-address> <amount>
//   credits-client sign <key-file> <to-address> <amount> <inner-id>
//
// transfer SIGNS AND SENDS A REAL TRANSACTION: on mainnet it spends real CS. Try it on a private
// or test network first. sign builds the same transaction offline and prints it.
//
// The Thrift classes are generated at build time (see CreditsClient.csproj) into Credits.Api
// (API, Pool, Transaction...) and Credits.General (Amount, APIResponse).
using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Numerics;
using System.Threading.Tasks;
using Credits.Api;
using Credits.General;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Crypto.Signers;
using Thrift;
using Thrift.Protocol;
using Thrift.Transport;
using Thrift.Transport.Client;

namespace CreditsClient
{
    public static class Program
    {
        private const sbyte CurrencyCs = 1;
        private static readonly BigInteger Fraction = BigInteger.Pow(10, 18); // integral + fraction / 10^18

        // the most the sender accepts to pay; the node charges the actual fee
        private static readonly short MaxFee = EncodeMaxFee(1.0);

        /// <summary>csdb::AmountCommission(double) as its 16-bit pattern (5-bit exponent, 10-bit mantissa).</summary>
        private static short EncodeMaxFee(double value)
        {
            double v = Math.Abs(value);
            double expf = Math.Log10(v);
            int expi = expf >= 0 ? (int)(expf + 0.5) : (int)(expf - 0.5);
            v /= Math.Pow(10, expi);
            if (v >= 1.0)
            {
                v *= 0.1;
                expi++;
            }
            return (short)((((expi + 18) & 0x1F) << 10) | ((int)Math.Round(v * 1024, MidpointRounding.AwayFromZero) & 0x3FF));
        }

        private static string FormatAmount(Amount a)
        {
            if (a == null) return "0";
            BigInteger units = a.Integral * Fraction + a.Fraction;
            BigInteger integral = BigInteger.DivRem(units, Fraction, out BigInteger frac);
            if (frac < 0)
            {
                integral -= 1;
                frac += Fraction;
            }
            string f = frac.ToString().PadLeft(18, '0').TrimEnd('0');
            return f.Length == 0 ? integral.ToString() : $"{integral}.{f}";
        }

        /// <summary>"12.5" -> (12, 500000000000000000), exactly.</summary>
        private static Amount ParseAmount(string text)
        {
            string[] parts = text.Split('.');
            string frac = parts.Length > 1 ? parts[1] : "";
            if (parts.Length > 2 || frac.Length > 18 || !frac.All(char.IsAsciiDigit)
                || !int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out int integral))
            {
                throw new ArgumentException($"{text}: not an amount (at most 18 decimals)");
            }
            long fraction = long.Parse(frac.PadRight(18, '0'), CultureInfo.InvariantCulture);
            if (integral == 0 && fraction == 0) throw new ArgumentException($"{text}: not a positive amount");
            return new Amount(integral, fraction);
        }

        private static string FormatTime(long ms) =>
            DateTimeOffset.FromUnixTimeMilliseconds(ms).UtcDateTime.ToString("yyyy-MM-dd HH:mm:ss 'UTC'", CultureInfo.InvariantCulture);

        private static string Hex(byte[] bytes) => Convert.ToHexString(bytes ?? Array.Empty<byte>()).ToLowerInvariant();

        private static byte[] Address(string text)
        {
            byte[] key = Base58.Decode(text);
            if (key.Length != 32) throw new ArgumentException($"{text}: not a base58 public key (32 bytes)");
            return key;
        }

        /// <summary>A base58 ed25519 secret key as in the node's NodePrivate.txt: 64 bytes (seed + public key), or a 32-byte seed.</summary>
        private static Ed25519PrivateKeyParameters ReadKey(string path)
        {
            byte[] secret = Base58.Decode(File.ReadAllText(path).Trim());
            if (secret.Length != 32 && secret.Length != 64)
                throw new ArgumentException($"{path}: expected a base58 ed25519 secret key of 64 bytes (or a 32-byte seed)");
            var key = new Ed25519PrivateKeyParameters(secret, 0);
            if (secret.Length == 64 && !key.GeneratePublicKey().GetEncoded().SequenceEqual(secret.Skip(32)))
                throw new ArgumentException($"{path}: the public half does not match the seed");
            return key;
        }

        private static void Check(APIResponse status, string call)
        {
            if (status != null && status.Code != 0)
                throw new InvalidOperationException($"{call}: {status.Message} (code {status.Code})");
        }

        private static async Task Status(API.Client c)
        {
            SyncStateResult s = await c.SyncStateGet();
            PoolListGetResult pools = await c.PoolListGet(0, 1);
            Check(pools.Status, "PoolListGet");
            // one wallet, by transaction count: the cheapest way to the total
            WalletsGetResult wallets = await c.WalletsGet(0, 1, 2, true);
            Pool head = pools.Pools[0];
            Console.WriteLine($"last block   : {s.LastBlock}");
            Console.WriteLine($"current round: {s.CurrRound}");
            Console.WriteLine($"head hash    : {Hex(head.Hash)}");
            Console.WriteLine($"head time    : {FormatTime(head.Time)}");
            Console.WriteLine($"wallets      : {wallets.Count}");
        }

        private static async Task Balance(API.Client c, string address)
        {
            WalletBalanceGetResult b = await c.WalletBalanceGet(Address(address));
            Check(b.Status, "WalletBalanceGet");
            Console.WriteLine($"{address}: {FormatAmount(b.Balance)} CS");
        }

        private static async Task Blocks(API.Client c, long n)
        {
            foreach (Pool p in (await c.PoolListGet(0, n)).Pools)
                Console.WriteLine($"{p.PoolNumber,11}  {Hex(p.Hash)}  {p.TransactionsCount,4} tx  {FormatTime(p.Time)}");
        }

        private static async Task Transactions(API.Client c, string address, long n)
        {
            TransactionsGetResult r = await c.TransactionsGet(Address(address), 0, n);
            Check(r.Status, "TransactionsGet");
            Console.WriteLine($"{r.Total_trxns_count} transactions, the last {r.Transactions.Count}:");
            foreach (SealedTransaction t in r.Transactions)
            {
                Transaction x = t.Trxn;
                Console.WriteLine($"{t.Id.PoolSeq,11}.{t.Id.Index,-4} {FormatTime(x.TimeCreation)}  " +
                    $"{Base58.Encode(x.Source)} -> {Base58.Encode(x.Target)}  {FormatAmount(x.Amount)} CS  type {(int)x.Type}");
            }
        }

        /// <summary>
        /// A signed CS transfer, and the bytes its signature covers:
        /// csdb::Transaction::to_byte_stream_for_sig(), little endian.
        /// </summary>
        private static (Transaction, byte[]) BuildTransfer(Ed25519PrivateKeyParameters key, string to, string amountText, long innerId)
        {
            byte[] source = key.GeneratePublicKey().GetEncoded();
            byte[] target = Address(to);
            Amount amount = ParseAmount(amountText);
            var signed = new MemoryStream();
            var w = new BinaryWriter(signed); // little endian
            w.Write(BitConverter.GetBytes(innerId), 0, 6); // inner id, 6 bytes (BitConverter: little endian on x86/ARM)
            w.Write(source);                // 32 bytes
            w.Write(target);                // 32 bytes
            w.Write(amount.Integral);       // amount
            w.Write((ulong)amount.Fraction);
            w.Write((ushort)MaxFee);        // max fee
            w.Write((byte)CurrencyCs);      // currency
            w.Write((byte)0);               // number of user fields
            w.Flush();
            byte[] bytes = signed.ToArray();

            var signer = new Ed25519Signer();
            signer.Init(true, key);
            signer.BlockUpdate(bytes, 0, bytes.Length);

            var t = new Transaction
            {
                Id = innerId,
                Source = source,
                Target = target,
                Amount = amount,
                Balance = new Amount(0, 0),
                Currency = CurrencyCs,
                Signature = signer.GenerateSignature(),
                Fee = new AmountCommission(MaxFee),
                TimeCreation = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                UserFields = Array.Empty<byte>(),
                Type = TransactionType.TT_Transfer,
                PoolNumber = 0,
            };
            return (t, bytes);
        }

        private static void Sign(string keyFile, string to, string amount, long innerId)
        {
            var (t, signed) = BuildTransfer(ReadKey(keyFile), to, amount, innerId);
            Console.WriteLine($"source   : {Base58.Encode(t.Source)}");
            Console.WriteLine($"signed   : {Hex(signed)}");
            Console.WriteLine($"signature: {Hex(t.Signature)}");
        }

        private static async Task Transfer(API.Client c, string keyFile, string to, string amount)
        {
            Ed25519PrivateKeyParameters key = ReadKey(keyFile);
            // the inner id must be the sender's last one + 1 (replay protection); a wallet the node has
            // never seen answers "not found": its first transaction has inner id 1
            WalletTransactionsCountGetResult last = await c.WalletTransactionsCountGet(key.GeneratePublicKey().GetEncoded());
            long innerId = (last.Status.Code == 0 ? last.LastTransactionInnerId : 0) + 1;
            var (t, _) = BuildTransfer(key, to, amount, innerId);
            TransactionFlowResult r = await c.TransactionFlow(t);
            Check(r.Status, "TransactionFlow");
            Console.WriteLine($"sent: inner id {innerId}, round {r.RoundNum}, fee {FormatAmount(r.Fee)} CS");
        }

        private static int Usage()
        {
            Console.Error.WriteLine(@"usage: credits-client [--node HOST:PORT] <command>
  status                               last block, round, head hash and time, wallet count
  balance <address>                    balance of a wallet (base58 public key)
  blocks [n]                           the last n blocks (default 10)
  tx <address> [n]                     the last n transactions of a wallet (default 10)
  transfer <key-file> <to> <amount>    sign and send a transfer: SPENDS REAL CS ON MAINNET
  sign <key-file> <to> <amount> <id>   build and sign a transfer with inner id <id>, print it, send nothing
--node defaults to 127.0.0.1:9090");
            return 2;
        }

        public static async Task<int> Main(string[] argv)
        {
            string node = "127.0.0.1:9090";
            string[] args = argv;
            if (argv.Length >= 2 && argv[0] == "--node")
            {
                node = argv[1];
                args = argv[2..];
            }
            if (args.Length == 0) return Usage();
            try
            {
                if (args[0] == "sign")
                {
                    if (args.Length != 5) return Usage();
                    Sign(args[1], args[2], args[3], long.Parse(args[4], CultureInfo.InvariantCulture));
                    return 0;
                }
                int colon = node.LastIndexOf(':');
                // by address: TSocketTransport(string host) does a reverse lookup of an IP address
                string host = node[..colon];
                IPAddress ip = IPAddress.TryParse(host, out IPAddress parsed) ? parsed : (await Dns.GetHostAddressesAsync(host))[0];
                var socket = new TSocketTransport(ip, int.Parse(node[(colon + 1)..], CultureInfo.InvariantCulture),
                    new TConfiguration(), 30_000);
                using var transport = new TBufferedTransport(socket);
                using var client = new API.Client(new TBinaryProtocol(transport));
                await client.OpenTransportAsync();
                switch (args)
                {
                    case ["status"]: await Status(client); break;
                    case ["balance", var a]: await Balance(client, a); break;
                    case ["blocks"]: await Blocks(client, 10); break;
                    case ["blocks", var n]: await Blocks(client, long.Parse(n, CultureInfo.InvariantCulture)); break;
                    case ["tx", var a]: await Transactions(client, a, 10); break;
                    case ["tx", var a, var n]: await Transactions(client, a, long.Parse(n, CultureInfo.InvariantCulture)); break;
                    case ["transfer", var k, var to, var amount]: await Transfer(client, k, to, amount); break;
                    default: return Usage();
                }
                return 0;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.Message);
                return 1;
            }
        }
    }
}
