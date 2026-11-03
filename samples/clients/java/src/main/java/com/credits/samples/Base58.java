package com.credits.samples;

import java.math.BigInteger;
import java.util.Arrays;

/** Base58 with the Bitcoin alphabet, as Credits addresses and keys use. */
final class Base58 {
    private static final String ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    private static final BigInteger BASE = BigInteger.valueOf(58);

    private Base58() {
    }

    static String encode(byte[] data) {
        StringBuilder sb = new StringBuilder();
        BigInteger n = new BigInteger(1, data);
        while (n.signum() > 0) {
            BigInteger[] qr = n.divideAndRemainder(BASE);
            sb.append(ALPHABET.charAt(qr[1].intValue()));
            n = qr[0];
        }
        for (int i = 0; i < data.length && data[i] == 0; i++) {
            sb.append('1');
        }
        return sb.reverse().toString();
    }

    static byte[] decode(String text) {
        BigInteger n = BigInteger.ZERO;
        for (char c : text.toCharArray()) {
            int digit = ALPHABET.indexOf(c);
            if (digit < 0) {
                throw new IllegalArgumentException(text + ": not base58");
            }
            n = n.multiply(BASE).add(BigInteger.valueOf(digit));
        }
        byte[] bytes = n.toByteArray();
        if (bytes.length > 1 && bytes[0] == 0) {
            bytes = Arrays.copyOfRange(bytes, 1, bytes.length); // the sign byte of BigInteger
        }
        int zeros = 0;
        while (zeros < text.length() && text.charAt(zeros) == '1') {
            zeros++;
        }
        byte[] out = new byte[zeros + (n.signum() == 0 ? 0 : bytes.length)];
        System.arraycopy(bytes, 0, out, zeros, out.length - zeros);
        return out;
    }
}
