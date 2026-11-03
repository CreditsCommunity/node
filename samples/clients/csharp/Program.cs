// A small client of the Credits node API (Thrift, binary protocol, buffered transport).
//
//   credits-client [--node HOST:PORT] status
//   credits-client [--node HOST:PORT] balance <address>
//   credits-client [--node HOST:PORT] blocks [n]
//   credits-client [--node HOST:PORT] tx <address> [n]
//   credits-client [--node HOST:PORT] transfer <key-file> <to-address> <amount>
//   credits-client sign <key-file> <to-address> <amount> <inner-id>
//   credits-client [--node HOST:PORT] deploy <key-file> <Contract.java>
//   credits-client [--node HOST:PORT] call <key-file> <contract> <method> [args...]
//   credits-client [--node HOST:PORT] get <contract> <method> [args...]
//   credits-client [--node HOST:PORT] methods <contract>
//
// transfer, deploy and call SIGN AND SEND REAL TRANSACTIONS: on mainnet they spend real CS. Try
// them on a private or test network first. sign builds a transfer offline and prints it; get runs
// a contract method as a getter, which costs nothing and changes nothing.
//
// Contract arguments are given as text and converted to the type the method declares (the node
// refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
// their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.
//
// The Thrift classes are generated at build time (see CreditsClient.csproj) into Credits.Api
// (API, Pool, Transaction...) and Credits.General (Amount, APIResponse).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Numerics;
using System.Threading;
using System.Threading.Tasks;
using Credits.Api;
using Credits.General;
using Org.BouncyCastle.Crypto.Digests;
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
        // contracts cost more: a deploy, or a call that runs long
        private static readonly short ContractMaxFee = EncodeMaxFee(10.0);

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

        // ---- smart contracts ------------------------------------------------------------------

        private static byte[] InnerId6(long innerId) => BitConverter.GetBytes(innerId)[..6]; // little endian

        private static async Task<long> NextInnerId(API.Client c, byte[] pk)
        {
            // the sender's last inner id + 1 (replay protection); a wallet the node has never seen: 1
            WalletTransactionsCountGetResult last = await c.WalletTransactionsCountGet(pk);
            return (last.Status.Code == 0 ? last.LastTransactionInnerId : 0) + 1;
        }

        private static SmartContractInvocation Invocation(string method, List<Variant> parameters, SmartContractDeploy deploy, bool getter)
        {
            // every field the IDL does not mark optional is set: these are the bytes the node
            // serializes again to check the signature
            var sci = new SmartContractInvocation
            {
                Method = method,
                Params = parameters,
                UsedContracts = new List<byte[]>(),
                ForgetNewState = getter,
                Version = 1,
            };
            if (deploy != null) sci.SmartContractDeploy = deploy;
            return sci;
        }

        /// <summary>
        /// A signed contract transaction (deploy or call). The invocation travels as the transaction's
        /// one user field: its Thrift binary serialization, which the signature covers.
        /// </summary>
        private static async Task<Transaction> ContractTransaction(Ed25519PrivateKeyParameters key, byte[] target,
            SmartContractInvocation sci, TransactionType type, long innerId)
        {
            byte[] source = key.GeneratePublicKey().GetEncoded();
            byte[] code;
            using (var buffer = new TMemoryBufferTransport(new TConfiguration()))
            {
                await sci.WriteAsync(new TBinaryProtocol(buffer), CancellationToken.None);
                code = buffer.GetBuffer();
            }
            var signed = new MemoryStream();
            var w = new BinaryWriter(signed); // little endian
            w.Write(InnerId6(innerId));
            w.Write(source);
            w.Write(target);
            w.Write(0);                       // amount: none
            w.Write(0UL);
            w.Write((ushort)ContractMaxFee);
            w.Write((byte)CurrencyCs);
            w.Write((byte)1);                 // one user field: u32 size + bytes
            w.Write((uint)code.Length);
            w.Write(code);
            w.Flush();

            var signer = new Ed25519Signer();
            signer.Init(true, key);
            byte[] bytes = signed.ToArray();
            signer.BlockUpdate(bytes, 0, bytes.Length);

            return new Transaction
            {
                Id = innerId,
                Source = source,
                Target = target,
                Amount = new Amount(0, 0),
                Balance = new Amount(0, 0),
                Currency = CurrencyCs,
                Signature = signer.GenerateSignature(),
                SmartContract = sci,
                Fee = new AmountCommission(ContractMaxFee),
                TimeCreation = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                Type = type,
            };
        }

        private static bool Flag(string text) => text.ToLowerInvariant() switch
        {
            "true" => true,
            "false" => false,
            _ => throw new FormatException(text),
        };

        /// <summary>The Variant the executor matches with a declared Java type.</summary>
        private static Variant ToVariant(string declared, string text)
        {
            CultureInfo inv = CultureInfo.InvariantCulture;
            try
            {
                switch (declared)
                {
                    case "java.lang.String": return new Variant { V_string = text };
                    case "java.math.BigDecimal":
                        return new Variant { V_big_decimal = decimal.Parse(text, NumberStyles.Float, inv).ToString(inv) };
                    case "int": return new Variant { V_int = int.Parse(text, NumberStyles.AllowLeadingSign, inv) };
                    case "java.lang.Integer": return new Variant { V_int_box = int.Parse(text, NumberStyles.AllowLeadingSign, inv) };
                    case "long": return new Variant { V_long = long.Parse(text, NumberStyles.AllowLeadingSign, inv) };
                    case "java.lang.Long": return new Variant { V_long_box = long.Parse(text, NumberStyles.AllowLeadingSign, inv) };
                    case "boolean": return new Variant { V_boolean = Flag(text) };
                    case "java.lang.Boolean": return new Variant { V_boolean_box = Flag(text) };
                    case "double": return new Variant { V_double = double.Parse(text, NumberStyles.Float, inv) };
                    case "java.lang.Double": return new Variant { V_double_box = double.Parse(text, NumberStyles.Float, inv) };
                    case "byte[]": return new Variant { V_byte_array = Convert.FromHexString(text) };
                }
            }
            catch (Exception e) when (e is FormatException || e is OverflowException)
            {
                throw new ArgumentException($"'{text}' is not a {declared}");
            }
            throw new ArgumentException($"arguments of type {declared} are not supported by this client");
        }

        /// <summary>A returned Variant as text, the same way in every sample.</summary>
        private static string Show(Variant v)
        {
            if (v == null) return "(nothing)";
            CultureInfo inv = CultureInfo.InvariantCulture;
            Variant.Isset s = v.__isset;
            if (s.v_boolean) return v.V_boolean ? "true" : "false";
            if (s.v_boolean_box) return v.V_boolean_box ? "true" : "false";
            if (s.v_string) return v.V_string;
            if (s.v_big_decimal) return v.V_big_decimal;
            if (s.v_byte) return v.V_byte.ToString(inv);
            if (s.v_byte_box) return v.V_byte_box.ToString(inv);
            if (s.v_short) return v.V_short.ToString(inv);
            if (s.v_short_box) return v.V_short_box.ToString(inv);
            if (s.v_int) return v.V_int.ToString(inv);
            if (s.v_int_box) return v.V_int_box.ToString(inv);
            if (s.v_long) return v.V_long.ToString(inv);
            if (s.v_long_box) return v.V_long_box.ToString(inv);
            if (s.v_float) return v.V_float.ToString("R", inv);
            if (s.v_float_box) return v.V_float_box.ToString("R", inv);
            if (s.v_double) return v.V_double.ToString("R", inv);
            if (s.v_double_box) return v.V_double_box.ToString("R", inv);
            if (s.v_byte_array) return Hex(v.V_byte_array);
            if (s.v_void) return "void";
            if (s.v_null) return "null";
            return "(nothing)";
        }

        /// <summary>The public methods of a deployed contract, as the node's executor lists them from its byte code.</summary>
        private static async Task<List<MethodDescription>> ContractMethods(API.Client c, byte[] contract)
        {
            SmartContractGetResult deployed = await c.SmartContractGet(contract);
            Check(deployed.Status, "SmartContractGet");
            ContractAllMethodsGetResult r =
                await c.ContractAllMethodsGet(deployed.SmartContract.SmartContractDeploy.ByteCodeObjects);
            if (r.Code != 0) throw new InvalidOperationException($"ContractAllMethodsGet: {r.Message}");
            return r.Methods ?? new List<MethodDescription>();
        }

        private static async Task<List<Variant>> TypedParams(API.Client c, byte[] contract, string method, string[] texts)
        {
            List<MethodDescription> candidates = (await ContractMethods(c, contract)).Where(m => m.Name == method).ToList();
            MethodDescription match = candidates.FirstOrDefault(m => m.Arguments.Count == texts.Length);
            if (match == null)
            {
                string there = candidates.Count == 0 ? "" : "; there is " + string.Join(", ",
                    candidates.Select(m => $"{m.Name}({string.Join(", ", m.Arguments.Select(a => a.Type))})"));
                throw new ArgumentException($"no method {method} with {texts.Length} arguments{there}");
            }
            return texts.Select((t, i) => ToVariant(match.Arguments[i].Type, t)).ToList();
        }

        private static async Task Methods(API.Client c, string contract)
        {
            foreach (MethodDescription m in (await ContractMethods(c, Address(contract))).OrderBy(m => m.Name, StringComparer.Ordinal))
                Console.WriteLine($"{m.ReturnType} {m.Name}({string.Join(", ", m.Arguments.Select(a => $"{a.Type} {a.Name}"))})");
        }

        private static async Task Deploy(API.Client c, string keyFile, string sourceFile)
        {
            Ed25519PrivateKeyParameters key = ReadKey(keyFile);
            byte[] pk = key.GeneratePublicKey().GetEncoded();
            string source = File.ReadAllText(sourceFile);
            // the node's executor compiles the source and checks it against its rules
            SmartContractCompileResult compiled = await c.SmartContractCompile(source);
            Check(compiled.Status, "SmartContractCompile");
            List<ByteCodeObject> classes = compiled.ByteCodeObjects;
            Console.WriteLine($"compiled: {classes.Count} class(es), token standard {compiled.TokenStandard}");
            long innerId = await NextInnerId(c, pk);
            // the address of the new contract (SmartContracts::get_valid_smart_address):
            // blake2s-256(deployer public key, 6-byte inner id, the byte code of every class in order)
            var blake = new Blake2sDigest(256);
            blake.BlockUpdate(pk, 0, pk.Length);
            blake.BlockUpdate(InnerId6(innerId), 0, 6);
            foreach (ByteCodeObject o in classes) blake.BlockUpdate(o.ByteCode, 0, o.ByteCode.Length);
            byte[] target = new byte[32];
            blake.DoFinal(target, 0);

            var deploy = new SmartContractDeploy
            {
                SourceCode = source,
                ByteCodeObjects = classes,
                HashState = "",
                TokenStandard = compiled.TokenStandard,
                Lang = 0,
            };
            Transaction t = await ContractTransaction(key, target, Invocation("", new List<Variant>(), deploy, false),
                TransactionType.TT_ContractDeploy, innerId);
            TransactionFlowResult r = await c.TransactionFlow(t);
            Check(r.Status, "TransactionFlow");
            Console.WriteLine($"sent: inner id {innerId}, round {r.RoundNum}, fee {FormatAmount(r.Fee)} CS");
            // the contract exists once the deploy is in a block and its executions are done
            DateTime deadline = DateTime.UtcNow.AddSeconds(180);
            while ((await c.SmartContractGet(target)).Status.Code != 0)
            {
                if (DateTime.UtcNow > deadline) throw new InvalidOperationException($"{Base58.Encode(target)} is not deployed after 180 s");
                await Task.Delay(2000);
            }
            Console.WriteLine($"contract: {Base58.Encode(target)}");
        }

        private static async Task Call(API.Client c, string keyFile, string contractText, string method, string[] texts)
        {
            Ed25519PrivateKeyParameters key = ReadKey(keyFile);
            byte[] contract = Address(contractText);
            List<Variant> parameters = await TypedParams(c, contract, method, texts);
            long innerId = await NextInnerId(c, key.GeneratePublicKey().GetEncoded());
            Transaction t = await ContractTransaction(key, contract, Invocation(method, parameters, null, false),
                TransactionType.TT_ContractCall, innerId);
            // the node answers once the confidants executed the call: the result is the method's return value
            TransactionFlowResult r = await c.TransactionFlow(t);
            Check(r.Status, "TransactionFlow");
            Console.WriteLine($"sent: inner id {innerId}, round {r.RoundNum}, fee {FormatAmount(r.Fee)} CS");
            Console.WriteLine($"returned: {Show(r.Smart_contract_result)}");
        }

        private static async Task Get(API.Client c, string contractText, string method, string[] texts)
        {
            byte[] contract = Address(contractText);
            List<Variant> parameters = await TypedParams(c, contract, method, texts);
            // a getter: TransactionFlow with forgetNewState runs the method on the node's own executor against
            // the current state and throws the new state away; nothing is sent, no fee, no signature is checked
            var t = new Transaction
            {
                Id = 0,
                Source = new byte[32],
                Target = contract,
                Amount = new Amount(0, 0),
                Balance = new Amount(0, 0),
                Currency = CurrencyCs,
                Signature = new byte[64],
                SmartContract = Invocation(method, parameters, null, true),
                Fee = new AmountCommission(ContractMaxFee),
                TimeCreation = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                Type = TransactionType.TT_ContractCall,
            };
            TransactionFlowResult r = await c.TransactionFlow(t);
            Check(r.Status, "TransactionFlow");
            Console.WriteLine(Show(r.Smart_contract_result));
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
  deploy <key-file> <Contract.java>    compile and deploy a contract: SPENDS REAL CS ON MAINNET
  call <key-file> <contract> <method> [args...]
                                       call a method in a transaction: SPENDS REAL CS ON MAINNET
  get <contract> <method> [args...]    run a method as a getter: free, changes nothing
  methods <contract>                   a contract's methods and their argument types
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
                    new TConfiguration(), 120_000);
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
                    case ["deploy", var k, var src]: await Deploy(client, k, src); break;
                    case ["call", var k, var contract, var method, .. var rest]: await Call(client, k, contract, method, rest); break;
                    case ["get", var contract, var method, .. var rest]: await Get(client, contract, method, rest); break;
                    case ["methods", var contract]: await Methods(client, contract); break;
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
