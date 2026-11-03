#!/usr/bin/env node
// A small client of the Credits node API (Thrift, binary protocol, buffered transport).
//
//   credits-client.js [--node HOST:PORT] status
//   credits-client.js [--node HOST:PORT] balance <address>
//   credits-client.js [--node HOST:PORT] blocks [n]
//   credits-client.js [--node HOST:PORT] tx <address> [n]
//   credits-client.js [--node HOST:PORT] transfer <key-file> <to-address> <amount>
//   credits-client.js sign <key-file> <to-address> <amount> <inner-id>
//
// transfer SIGNS AND SENDS A REAL TRANSACTION: on mainnet it spends real CS. Try it on a private
// or test network first. sign builds the same transaction offline and prints it.
//
// The Thrift code is generated into gen/ by `npm install` (npm run generate). ed25519 comes from
// node:crypto; i64 values arrive as node-int64 objects and are read as BigInt for exactness.
'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const thrift = require('thrift');
const Int64 = require('node-int64');
const API = require('./gen/API');
const apiTypes = require('./gen/api_types');
const generalTypes = require('./gen/general_types');

const CURRENCY_CS = 1;
const FRACTION = 10n ** 18n; // general.Amount: integral + fraction / 10^18

// csdb::AmountCommission(double) as its 16-bit pattern (5-bit exponent, 10-bit mantissa)
function encodeMaxFee(value) {
  let v = Math.abs(value);
  const expf = Math.log10(v);
  let expi = expf >= 0 ? Math.trunc(expf + 0.5) : Math.trunc(expf - 0.5);
  v /= 10 ** expi;
  if (v >= 1.0) {
    v *= 0.1;
    expi += 1;
  }
  return (((expi + 18) & 0x1f) << 10) | (Math.round(v * 1024) & 0x3ff);
}
const MAX_FEE = encodeMaxFee(1.0); // the most the sender accepts to pay; the node charges the actual fee

// an i64 from the generated code (node-int64, or a plain number) as a BigInt
function big(x) {
  if (x === null || x === undefined) return 0n;
  if (x instanceof Int64) return BigInt.asIntN(64, BigInt('0x' + x.toOctetString()));
  return BigInt(x);
}

function fmtAmount(a) {
  if (!a) return '0';
  const units = big(a.integral) * FRACTION + big(a.fraction);
  let int = units / FRACTION;
  let frac = units % FRACTION;
  if (frac < 0n) { int -= 1n; frac += FRACTION; }
  const f = frac.toString().padStart(18, '0').replace(/0+$/, '');
  return f ? `${int}.${f}` : `${int}`;
}

// "12.5" -> (12n, 500000000000000000n), exactly
function parseAmount(text) {
  const m = /^(\d+)(?:\.(\d{1,18}))?$/.exec(text);
  if (!m) throw new Error(`${text}: not an amount (at most 18 decimals)`);
  const integral = BigInt(m[1]);
  const fraction = BigInt((m[2] || '').padEnd(18, '0'));
  if (integral > 2147483647n) throw new Error(`${text}: too large`);
  if (integral === 0n && fraction === 0n) throw new Error(`${text}: not a positive amount`);
  return { integral, fraction };
}

function fmtTime(ms) {
  return new Date(Number(big(ms))).toISOString().replace('T', ' ').replace(/\.\d+Z$/, ' UTC');
}

const ALPHABET = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';

function b58encode(bytes) {
  bytes = Buffer.from(bytes || []);
  let n = bytes.length ? BigInt('0x' + bytes.toString('hex')) : 0n;
  let out = '';
  while (n > 0n) { out = ALPHABET[Number(n % 58n)] + out; n /= 58n; }
  for (const b of bytes) { if (b !== 0) break; out = '1' + out; }
  return out;
}

function b58decode(text) {
  let n = 0n;
  for (const c of text) {
    const d = ALPHABET.indexOf(c);
    if (d < 0) throw new Error(`${text}: not base58`);
    n = n * 58n + BigInt(d);
  }
  let hex = n === 0n ? '' : n.toString(16);
  if (hex.length % 2) hex = '0' + hex;
  const zeros = text.length - text.replace(/^1+/, '').length;
  return Buffer.concat([Buffer.alloc(zeros), Buffer.from(hex, 'hex')]);
}

function address(text) {
  const key = b58decode(text);
  if (key.length !== 32) throw new Error(`${text}: not a base58 public key (32 bytes)`);
  return key;
}

// A base58 ed25519 secret key as in the node's NodePrivate.txt: 64 bytes (seed + public key), or a
// bare 32-byte seed. node:crypto takes the seed wrapped in PKCS#8.
function readKey(path) {
  const secret = b58decode(fs.readFileSync(path, 'utf8').trim());
  if (secret.length !== 32 && secret.length !== 64) {
    throw new Error(`${path}: expected a base58 ed25519 secret key of 64 bytes (or a 32-byte seed)`);
  }
  const pkcs8 = Buffer.concat([Buffer.from('302e020100300506032b657004220420', 'hex'), secret.subarray(0, 32)]);
  const privateKey = crypto.createPrivateKey({ key: pkcs8, format: 'der', type: 'pkcs8' });
  const publicKey = crypto.createPublicKey(privateKey).export({ format: 'der', type: 'spki' }).subarray(-32);
  if (secret.length === 64 && !publicKey.equals(secret.subarray(32))) {
    throw new Error(`${path}: the public half does not match the seed`);
  }
  return { privateKey, publicKey };
}

function check(status, call) {
  if (status && status.code !== 0) throw new Error(`${call}: ${status.message} (code ${status.code})`);
}

async function status(c) {
  const s = await c.SyncStateGet();
  const pools = await c.PoolListGet(0, 1);
  check(pools.status, 'PoolListGet');
  // one wallet, by transaction count: the cheapest way to the total
  const wallets = await c.WalletsGet(0, 1, 2, true);
  const head = pools.pools[0];
  console.log(`last block   : ${big(s.lastBlock)}`);
  console.log(`current round: ${big(s.currRound)}`);
  console.log(`head hash    : ${Buffer.from(head.hash).toString('hex')}`);
  console.log(`head time    : ${fmtTime(head.time)}`);
  console.log(`wallets      : ${wallets.count}`);
}

async function balance(c, addr) {
  const b = await c.WalletBalanceGet(address(addr));
  check(b.status, 'WalletBalanceGet');
  console.log(`${addr}: ${fmtAmount(b.balance)} CS`);
}

async function blocks(c, n) {
  for (const p of (await c.PoolListGet(0, n)).pools) {
    console.log(`${String(big(p.poolNumber)).padStart(11)}  ${Buffer.from(p.hash).toString('hex')}  ` +
      `${String(p.transactionsCount).padStart(4)} tx  ${fmtTime(p.time)}`);
  }
}

async function transactions(c, addr, n) {
  const r = await c.TransactionsGet(address(addr), 0, n);
  check(r.status, 'TransactionsGet');
  console.log(`${r.total_trxns_count} transactions, the last ${r.transactions.length}:`);
  for (const t of r.transactions) {
    const x = t.trxn;
    console.log(`${String(big(t.id.poolSeq)).padStart(11)}.${String(t.id.index).padEnd(4)} ${fmtTime(x.timeCreation)}  ` +
      `${b58encode(x.source)} -> ${b58encode(x.target)}  ${fmtAmount(x.amount)} CS  type ${x.type}`);
  }
}

// A signed CS transfer, and the bytes its signature covers:
// csdb::Transaction::to_byte_stream_for_sig(), little endian.
function buildTransfer(key, to, amountText, innerId) {
  const target = address(to);
  const amount = parseAmount(amountText);
  const signed = Buffer.alloc(6 + 32 + 32 + 4 + 8 + 2 + 1 + 1);
  let o = 0;
  signed.writeUIntLE(Number(innerId), o, 6); o += 6;          // inner id, 6 bytes
  key.publicKey.copy(signed, o); o += 32;                    // source, 32 bytes
  target.copy(signed, o); o += 32;                           // target, 32 bytes
  signed.writeInt32LE(Number(amount.integral), o); o += 4;   // amount
  signed.writeBigUInt64LE(amount.fraction, o); o += 8;
  signed.writeUInt16LE(MAX_FEE, o); o += 2;                  // max fee
  signed.writeUInt8(CURRENCY_CS, o); o += 1;                 // currency
  signed.writeUInt8(0, o);                                   // number of user fields

  const fraction = Buffer.alloc(8);
  fraction.writeBigUInt64BE(amount.fraction);
  const t = new apiTypes.Transaction({
    id: Number(innerId),
    source: key.publicKey,
    target,
    amount: new generalTypes.Amount({ integral: Number(amount.integral), fraction: new Int64(fraction) }),
    balance: new generalTypes.Amount({ integral: 0, fraction: 0 }),
    currency: CURRENCY_CS,
    signature: crypto.sign(null, signed, key.privateKey),
    fee: new apiTypes.AmountCommission({ commission: MAX_FEE }),
    timeCreation: Date.now(),
    userFields: Buffer.alloc(0),
    type: apiTypes.TransactionType.TT_Transfer,
    poolNumber: 0,
  });
  return { t, signed };
}

function sign(keyFile, to, amount, innerId) {
  const { t, signed } = buildTransfer(readKey(keyFile), to, amount, BigInt(innerId));
  console.log(`source   : ${b58encode(t.source)}`);
  console.log(`signed   : ${signed.toString('hex')}`);
  console.log(`signature: ${Buffer.from(t.signature).toString('hex')}`);
}

async function transfer(c, keyFile, to, amount) {
  const key = readKey(keyFile);
  // the inner id must be the sender's last one + 1 (replay protection); a wallet the node has
  // never seen answers "not found": its first transaction has inner id 1
  const last = await c.WalletTransactionsCountGet(key.publicKey);
  const innerId = (last.status.code === 0 ? big(last.lastTransactionInnerId) : 0n) + 1n;
  const { t } = buildTransfer(key, to, amount, innerId);
  const r = await c.TransactionFlow(t);
  check(r.status, 'TransactionFlow');
  console.log(`sent: inner id ${innerId}, round ${r.roundNum}, fee ${fmtAmount(r.fee)} CS`);
}

function usage() {
  console.error(`usage: credits-client.js [--node HOST:PORT] <command>
  status                               last block, round, head hash and time, wallet count
  balance <address>                    balance of a wallet (base58 public key)
  blocks [n]                           the last n blocks (default 10)
  tx <address> [n]                     the last n transactions of a wallet (default 10)
  transfer <key-file> <to> <amount>    sign and send a transfer: SPENDS REAL CS ON MAINNET
  sign <key-file> <to> <amount> <id>   build and sign a transfer with inner id <id>, print it, send nothing
--node defaults to 127.0.0.1:9090`);
  process.exit(2);
}

async function main() {
  let args = process.argv.slice(2);
  let node = '127.0.0.1:9090';
  if (args[0] === '--node' && args.length >= 2) {
    node = args[1];
    args = args.slice(2);
  }
  const [cmd, ...rest] = args;
  if (cmd === 'sign' && rest.length === 4) return sign(...rest);

  const calls = {
    status: [0, 0, (c) => status(c)],
    balance: [1, 1, (c) => balance(c, rest[0])],
    blocks: [0, 1, (c) => blocks(c, Number(rest[0] || 10))],
    tx: [1, 2, (c) => transactions(c, rest[0], Number(rest[1] || 10))],
    transfer: [3, 3, (c) => transfer(c, ...rest)],
  };
  const call = calls[cmd];
  if (!call || rest.length < call[0] || rest.length > call[1]) usage();

  const colon = node.lastIndexOf(':');
  const connection = thrift.createConnection(node.slice(0, colon), Number(node.slice(colon + 1)), {
    transport: thrift.TBufferedTransport,
    protocol: thrift.TBinaryProtocol,
    connect_timeout: 10000,
    timeout: 30000,
  });
  const failed = new Promise((_, reject) => connection.on('error', reject));
  try {
    await Promise.race([call[2](thrift.createClient(API, connection)), failed]);
  } finally {
    connection.end();
  }
}

main().catch((e) => {
  console.error(e.message || String(e));
  process.exit(1);
});
