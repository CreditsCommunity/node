using System;
using System.Linq;
using System.Numerics;
using System.Text;

namespace CreditsClient
{
    /// <summary>Base58 with the Bitcoin alphabet, as Credits addresses and keys use.</summary>
    internal static class Base58
    {
        private const string Alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

        public static string Encode(byte[] data)
        {
            data ??= Array.Empty<byte>();
            // BigInteger reads little endian, two's complement: reverse and add a zero sign byte
            var n = new BigInteger(data.Reverse().Append((byte)0).ToArray());
            var sb = new StringBuilder();
            while (n > 0)
            {
                n = BigInteger.DivRem(n, 58, out BigInteger r);
                sb.Insert(0, Alphabet[(int)r]);
            }
            foreach (byte b in data)
            {
                if (b != 0) break;
                sb.Insert(0, '1');
            }
            return sb.ToString();
        }

        public static byte[] Decode(string text)
        {
            BigInteger n = 0;
            foreach (char c in text)
            {
                int digit = Alphabet.IndexOf(c);
                if (digit < 0) throw new ArgumentException($"{text}: not base58");
                n = n * 58 + digit;
            }
            byte[] bytes = n.IsZero ? Array.Empty<byte>() : n.ToByteArray(isUnsigned: true, isBigEndian: true);
            int zeros = text.TakeWhile(c => c == '1').Count();
            return new byte[zeros].Concat(bytes).ToArray();
        }
    }
}
