#!/usr/bin/env node
// A small client of the Credits node API (Thrift, binary protocol, buffered transport).
//
//   credits-client.js [--node HOST:PORT] status
//   credits-client.js [--node HOST:PORT] balance <address>
//   credits-client.js [--node HOST:PORT] blocks [n]
//   credits-client.js [--node HOST:PORT] tx <address> [n]
//   credits-client.js [--node HOST:PORT] transfer <key-file> <to-address> <amount>
//   credits-client.js sign <key-file> <to-address> <amount> <inner-id>
//   credits-client.js [--node HOST:PORT] deploy <key-file> <Contract.java>
//   credits-client.js [--node HOST:PORT] call <key-file> <contract> <method> [args...]
//   credits-client.js [--node HOST:PORT] get <contract> <method> [args...]
//   credits-client.js [--node HOST:PORT] methods <contract>
//
// transfer, deploy and call SIGN AND SEND REAL TRANSACTIONS: on mainnet they spend real CS. Try
// them on a private or test network first. sign builds a transfer offline and prints it; get runs
// a contract method as a getter, which costs nothing and changes nothing.
//
// Contract arguments are given as text and converted to the type the method declares (the node
// refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
// their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.
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
const CONTRACT_MAX_FEE = encodeMaxFee(10.0); // contracts cost more: a deploy, or a call that runs long

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

// ---- smart contracts ------------------------------------------------------------------------------

// the sender's last inner id + 1 (replay protection); a wallet the node has never seen: 1
async function nextInnerId(c, pk) {
  const last = await c.WalletTransactionsCountGet(pk);
  return (last.status.code === 0 ? big(last.lastTransactionInnerId) : 0n) + 1n;
}

// Every field the IDL does not mark optional is set: these are the bytes the node serializes again
// to check the signature.
function invocation(method, params, deploy, getter) {
  return new apiTypes.SmartContractInvocation({
    method,
    params,
    usedContracts: [],
    forgetNewState: getter,
    smartContractDeploy: deploy || undefined,
    version: 1,
  });
}

// a Thrift struct as its binary protocol bytes
function serialize(struct) {
  let out;
  const transport = new thrift.TBufferedTransport(null, (buf) => { out = buf; });
  const protocol = new thrift.TBinaryProtocol(transport);
  struct.write(protocol);
  protocol.flush();
  return out;
}

// A signed contract transaction (deploy or call). The invocation travels as the transaction's one
// user field: its Thrift binary serialization, which the signature covers.
function contractTransaction(key, target, sci, type, innerId) {
  const code = serialize(sci);
  const signed = Buffer.alloc(6 + 32 + 32 + 4 + 8 + 2 + 1 + 1 + 4 + code.length);
  let o = 0;
  signed.writeUIntLE(Number(innerId), o, 6); o += 6;
  key.publicKey.copy(signed, o); o += 32;
  target.copy(signed, o); o += 32;
  o += 12;                                                   // amount: none (int32 0, uint64 0)
  signed.writeUInt16LE(CONTRACT_MAX_FEE, o); o += 2;
  signed.writeUInt8(CURRENCY_CS, o); o += 1;
  signed.writeUInt8(1, o); o += 1;                           // one user field: u32 size + bytes
  signed.writeUInt32LE(code.length, o); o += 4;
  code.copy(signed, o);
  return new apiTypes.Transaction({
    id: Number(innerId),
    source: key.publicKey,
    target,
    amount: new generalTypes.Amount({ integral: 0, fraction: 0 }),
    balance: new generalTypes.Amount({ integral: 0, fraction: 0 }),
    currency: CURRENCY_CS,
    signature: crypto.sign(null, signed, key.privateKey),
    smartContract: sci,
    fee: new apiTypes.AmountCommission({ commission: CONTRACT_MAX_FEE }),
    timeCreation: Date.now(),
    type,
  });
}

const DECIMAL = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/;
const INTEGER = /^[+-]?\d+$/;

function integer(text, bits) {
  if (!INTEGER.test(text)) throw new Error('not an integer');
  const n = BigInt(text);
  if (BigInt.asIntN(bits, n) !== n) throw new Error('out of range');
  return n;
}

function flag(text) {
  const t = text.toLowerCase();
  if (t !== 'true' && t !== 'false') throw new Error('not a boolean');
  return t === 'true';
}

function double(text) {
  if (!DECIMAL.test(text)) throw new Error('not a number');
  return Number(text);
}

function int64(n) {
  const b = Buffer.alloc(8);
  b.writeBigInt64BE(n);
  return new Int64(b);
}

// the Variant the executor matches with a declared Java type
function variant(declared, text) {
  const converters = {
    'java.lang.String': () => ({ v_string: text }),
    'java.math.BigDecimal': () => {
      if (!DECIMAL.test(text)) throw new Error('not a number');
      return { v_big_decimal: text.replace(/^\+/, '') };
    },
    int: () => ({ v_int: Number(integer(text, 32)) }),
    'java.lang.Integer': () => ({ v_int_box: Number(integer(text, 32)) }),
    long: () => ({ v_long: int64(integer(text, 64)) }),
    'java.lang.Long': () => ({ v_long_box: int64(integer(text, 64)) }),
    boolean: () => ({ v_boolean: flag(text) }),
    'java.lang.Boolean': () => ({ v_boolean_box: flag(text) }),
    double: () => ({ v_double: double(text) }),
    'java.lang.Double': () => ({ v_double_box: double(text) }),
    'byte[]': () => {
      if (!/^([0-9a-fA-F]{2})*$/.test(text)) throw new Error('not hex');
      return { v_byte_array: Buffer.from(text, 'hex') };
    },
  };
  if (!Object.hasOwn(converters, declared)) {
    throw new Error(`arguments of type ${declared} are not supported by this client`);
  }
  let fields;
  try {
    fields = converters[declared]();
  } catch (e) {
    throw new Error(`'${text}' is not a ${declared}`);
  }
  return new generalTypes.Variant(fields);
}

// a returned Variant as text, the same way in every sample
function show(v) {
  if (!v) return '(nothing)';
  for (const name of ['v_boolean', 'v_boolean_box']) {
    if (v[name] !== null && v[name] !== undefined) return v[name] ? 'true' : 'false';
  }
  for (const name of ['v_float', 'v_float_box', 'v_double', 'v_double_box']) {
    if (v[name] !== null && v[name] !== undefined) {
      const text = String(v[name]);
      return /^-?\d+$/.test(text) ? `${text}.0` : text;
    }
  }
  for (const name of ['v_string', 'v_big_decimal', 'v_byte', 'v_byte_box', 'v_short', 'v_short_box', 'v_int',
    'v_int_box', 'v_long', 'v_long_box']) {
    if (v[name] !== null && v[name] !== undefined) {
      return name.startsWith('v_long') ? String(big(v[name])) : String(v[name]);
    }
  }
  if (v.v_byte_array !== null && v.v_byte_array !== undefined) return Buffer.from(v.v_byte_array).toString('hex');
  if (v.v_void !== null && v.v_void !== undefined) return 'void';
  if (v.v_null !== null && v.v_null !== undefined) return 'null';
  return '(nothing)';
}

// the public methods of a deployed contract, as the node's executor lists them from its byte code
async function contractMethods(c, contract) {
  const deployed = await c.SmartContractGet(contract);
  check(deployed.status, 'SmartContractGet');
  const r = await c.ContractAllMethodsGet(deployed.smartContract.smartContractDeploy.byteCodeObjects);
  if (r.code !== 0) throw new Error(`ContractAllMethodsGet: ${r.message}`);
  return r.methods;
}

async function typedParams(c, contract, method, texts) {
  const candidates = (await contractMethods(c, contract)).filter((m) => m.name === method);
  const match = candidates.find((m) => m.arguments.length === texts.length);
  if (!match) {
    const known = candidates.map((m) => `${m.name}(${m.arguments.map((a) => a.type).join(', ')})`).join(', ');
    throw new Error(`no method ${method} with ${texts.length} arguments` + (known ? `; there is ${known}` : ''));
  }
  return match.arguments.map((a, i) => variant(a.type, texts[i]));
}

async function methods(c, contractText) {
  const all = await contractMethods(c, address(contractText));
  all.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  for (const m of all) {
    console.log(`${m.returnType} ${m.name}(${m.arguments.map((a) => `${a.type} ${a.name}`).join(', ')})`);
  }
}

async function deploy(c, keyFile, sourceFile) {
  const key = readKey(keyFile);
  const source = fs.readFileSync(sourceFile, 'utf8');
  // the node's executor compiles the source and checks it against its rules
  const compiled = await c.SmartContractCompile(source);
  check(compiled.status, 'SmartContractCompile');
  const classes = compiled.byteCodeObjects;
  console.log(`compiled: ${classes.length} class(es), token standard ${compiled.tokenStandard}`);
  const innerId = await nextInnerId(c, key.publicKey);
  // the address of the new contract (SmartContracts::get_valid_smart_address):
  // blake2s-256(deployer public key, 6-byte inner id, the byte code of every class in order)
  const id = Buffer.alloc(6);
  id.writeUIntLE(Number(innerId), 0, 6);
  const target = crypto.createHash('blake2s256').update(key.publicKey).update(id);
  for (const cls of classes) target.update(cls.byteCode);
  const address32 = target.digest();

  const sci = invocation('', [], new apiTypes.SmartContractDeploy({
    sourceCode: source, byteCodeObjects: classes, hashState: '', tokenStandard: compiled.tokenStandard, lang: 0,
  }), false);
  const r = await c.TransactionFlow(contractTransaction(key, address32, sci, apiTypes.TransactionType.TT_ContractDeploy, innerId));
  check(r.status, 'TransactionFlow');
  console.log(`sent: inner id ${innerId}, round ${r.roundNum}, fee ${fmtAmount(r.fee)} CS`);
  // the contract exists once the deploy is in a block and its executions are done
  const deadline = Date.now() + 180000;
  while ((await c.SmartContractGet(address32)).status.code !== 0) {
    if (Date.now() > deadline) throw new Error(`${b58encode(address32)} is not deployed after 180 s`);
    await new Promise((resolve) => setTimeout(resolve, 2000));
  }
  console.log(`contract: ${b58encode(address32)}`);
}

async function callContract(c, keyFile, contractText, method, ...texts) {
  const key = readKey(keyFile);
  const contract = address(contractText);
  const params = await typedParams(c, contract, method, texts);
  const innerId = await nextInnerId(c, key.publicKey);
  const sci = invocation(method, params, null, false);
  // the node answers once the confidants executed the call: the result is the method's return value
  const r = await c.TransactionFlow(contractTransaction(key, contract, sci, apiTypes.TransactionType.TT_ContractCall, innerId));
  check(r.status, 'TransactionFlow');
  console.log(`sent: inner id ${innerId}, round ${r.roundNum}, fee ${fmtAmount(r.fee)} CS`);
  console.log(`returned: ${show(r.smart_contract_result)}`);
}

async function getContract(c, contractText, method, ...texts) {
  const contract = address(contractText);
  const params = await typedParams(c, contract, method, texts);
  // a getter: TransactionFlow with forgetNewState runs the method on the node's own executor against
  // the current state and throws the new state away; nothing is sent, no fee, no signature is checked
  const t = new apiTypes.Transaction({
    id: 0,
    source: Buffer.alloc(32),
    target: contract,
    amount: new generalTypes.Amount({ integral: 0, fraction: 0 }),
    balance: new generalTypes.Amount({ integral: 0, fraction: 0 }),
    currency: CURRENCY_CS,
    signature: Buffer.alloc(64),
    smartContract: invocation(method, params, null, true),
    fee: new apiTypes.AmountCommission({ commission: CONTRACT_MAX_FEE }),
    timeCreation: Date.now(),
    type: apiTypes.TransactionType.TT_ContractCall,
  });
  const r = await c.TransactionFlow(t);
  check(r.status, 'TransactionFlow');
  console.log(show(r.smart_contract_result));
}

function usage() {
  console.error(`usage: credits-client.js [--node HOST:PORT] <command>
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
    deploy: [2, 2, (c) => deploy(c, ...rest)],
    call: [3, Infinity, (c) => callContract(c, ...rest)],
    get: [2, Infinity, (c) => getContract(c, ...rest)],
    methods: [1, 1, (c) => methods(c, rest[0])],
  };
  const call = calls[cmd];
  if (!call || rest.length < call[0] || rest.length > call[1]) usage();

  const colon = node.lastIndexOf(':');
  const connection = thrift.createConnection(node.slice(0, colon), Number(node.slice(colon + 1)), {
    transport: thrift.TBufferedTransport,
    protocol: thrift.TBinaryProtocol,
    connect_timeout: 10000,
    timeout: 120000,
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
