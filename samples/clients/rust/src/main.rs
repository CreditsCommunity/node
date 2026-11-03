//! A small client of the Credits node API (Thrift, binary protocol, buffered transport).
//!
//! ```text
//! credits-client [--node HOST:PORT] status
//! credits-client [--node HOST:PORT] balance <address>
//! credits-client [--node HOST:PORT] blocks [n]
//! credits-client [--node HOST:PORT] tx <address> [n]
//! credits-client [--node HOST:PORT] transfer <key-file> <to-address> <amount>
//! credits-client sign <key-file> <to-address> <amount> <inner-id>
//! credits-client [--node HOST:PORT] deploy <key-file> <Contract.java>
//! credits-client [--node HOST:PORT] call <key-file> <contract> <method> [args...]
//! credits-client [--node HOST:PORT] get <contract> <method> [args...]
//! credits-client [--node HOST:PORT] methods <contract>
//! ```
//!
//! transfer, deploy and call SIGN AND SEND REAL TRANSACTIONS: on mainnet they spend real CS. Try
//! them on a private or test network first. sign builds a transfer offline and prints it; get runs
//! a contract method as a getter, which costs nothing and changes nothing.
//!
//! Contract arguments are given as text and converted to the type the method declares (the node
//! refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
//! their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.

#[allow(dead_code, unused_imports, unused_extern_crates, clippy::all)]
mod general {
    include!(concat!(env!("OUT_DIR"), "/general.rs"));
}
#[allow(dead_code, unused_imports, unused_extern_crates, clippy::all)]
mod api {
    include!(concat!(env!("OUT_DIR"), "/api.rs"));
}

use std::error::Error;
use std::process::exit;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use api::{APISyncClient, TAPISyncClient};
use blake2::{Blake2s256, Digest};
use ed25519_dalek::{Signer, SigningKey};
use thrift::protocol::{TBinaryInputProtocol, TBinaryOutputProtocol, TSerializable};
use thrift::OrderedFloat;
use thrift::transport::{
    ReadHalf, TBufferedReadTransport, TBufferedWriteTransport, TIoChannel, TTcpChannel, WriteHalf,
};

type Client = APISyncClient<
    TBinaryInputProtocol<TBufferedReadTransport<ReadHalf<TTcpChannel>>>,
    TBinaryOutputProtocol<TBufferedWriteTransport<WriteHalf<TTcpChannel>>>,
>;
type Result<T> = std::result::Result<T, Box<dyn Error>>;

const CURRENCY_CS: i8 = 1;
/// Contracts cost more: a deploy, or a call that runs long.
const CONTRACT_FEE: f64 = 10.0;
const FRACTION: i128 = 1_000_000_000_000_000_000; // general.Amount: integral + fraction / 10^18

/// csdb::AmountCommission(double) as its 16-bit pattern (5-bit exponent, 10-bit mantissa).
fn encode_max_fee(value: f64) -> i16 {
    let mut v = value.abs();
    let expf = v.log10();
    let mut expi = if expf >= 0.0 { (expf + 0.5) as i32 } else { (expf - 0.5) as i32 };
    v /= 10f64.powi(expi);
    if v >= 1.0 {
        v *= 0.1;
        expi += 1;
    }
    ((((expi + 18) & 0x1F) << 10) | (((v * 1024.0).round() as i32) & 0x3FF)) as i16
}

fn fmt_amount(a: &general::Amount) -> String {
    let units = a.integral as i128 * FRACTION + a.fraction as i128;
    let (int, frac) = (units.div_euclid(FRACTION), units.rem_euclid(FRACTION));
    let frac = format!("{frac:018}");
    let frac = frac.trim_end_matches('0');
    if frac.is_empty() { int.to_string() } else { format!("{int}.{frac}") }
}

/// "12.5" -> (12, 500000000000000000), exactly.
fn parse_amount(text: &str) -> Result<general::Amount> {
    let (int, frac) = text.split_once('.').unwrap_or((text, ""));
    if frac.len() > 18 || !frac.chars().all(|c| c.is_ascii_digit()) {
        return Err(format!("{text}: not an amount (at most 18 decimals)").into());
    }
    let integral: i32 = int.parse().map_err(|_| format!("{text}: not an amount"))?;
    let fraction: i64 = format!("{frac:0<18}").parse()?;
    if integral < 0 || (integral == 0 && fraction == 0) {
        return Err(format!("{text}: not a positive amount").into());
    }
    Ok(general::Amount::new(integral, fraction))
}

fn fmt_time(ms: i64) -> String {
    chrono::DateTime::from_timestamp_millis(ms)
        .map(|t| t.format("%Y-%m-%d %H:%M:%S UTC").to_string())
        .unwrap_or_default()
}

fn address(text: &str) -> Result<Vec<u8>> {
    match bs58::decode(text).into_vec() {
        Ok(key) if key.len() == 32 => Ok(key),
        _ => Err(format!("{text}: not a base58 public key (32 bytes)").into()),
    }
}

/// A base58 ed25519 secret key as in the node's NodePrivate.txt: 64 bytes (seed + public key), or
/// a bare 32-byte seed.
fn read_key(path: &str) -> Result<SigningKey> {
    let text = std::fs::read_to_string(path)?;
    let secret = bs58::decode(text.trim()).into_vec().unwrap_or_default();
    if secret.len() != 32 && secret.len() != 64 {
        return Err(format!("{path}: expected a base58 ed25519 secret key of 64 bytes (or a 32-byte seed)").into());
    }
    let key = SigningKey::from_bytes(secret[..32].try_into()?);
    if secret.len() == 64 && key.verifying_key().as_bytes()[..] != secret[32..] {
        return Err(format!("{path}: the public half does not match the seed").into());
    }
    Ok(key)
}

fn check(status: &Option<general::APIResponse>, call: &str) -> Result<()> {
    match status {
        Some(s) if s.code.unwrap_or(0) != 0 => Err(format!(
            "{call}: {} (code {})",
            s.message.clone().unwrap_or_default(),
            s.code.unwrap_or(0)
        )
        .into()),
        _ => Ok(()),
    }
}

fn connect(node: &str) -> Result<Client> {
    let stream = std::net::TcpStream::connect(node)?;
    stream.set_read_timeout(Some(Duration::from_secs(120)))?;
    let (read, write) = TTcpChannel::with_stream(stream).split()?;
    Ok(APISyncClient::new(
        TBinaryInputProtocol::new(TBufferedReadTransport::new(read), true),
        TBinaryOutputProtocol::new(TBufferedWriteTransport::new(write), true),
    ))
}

fn status(c: &mut Client) -> Result<()> {
    let s = c.sync_state_get()?;
    let pools = c.pool_list_get(0, 1)?;
    check(&pools.status, "PoolListGet")?;
    // one wallet, by transaction count: the cheapest way to the total
    let wallets = c.wallets_get(0, 1, 2, true)?;
    let head = pools.pools.unwrap_or_default().into_iter().next().ok_or("no blocks")?;
    println!("last block   : {}", s.last_block.unwrap_or(0));
    println!("current round: {}", s.curr_round.unwrap_or(0));
    println!("head hash    : {}", hex::encode(head.hash.unwrap_or_default()));
    println!("head time    : {}", fmt_time(head.time.unwrap_or(0)));
    println!("wallets      : {}", wallets.count.unwrap_or(0));
    Ok(())
}

fn balance(c: &mut Client, addr: &str) -> Result<()> {
    let b = c.wallet_balance_get(address(addr)?)?;
    check(&b.status, "WalletBalanceGet")?;
    println!("{addr}: {} CS", fmt_amount(&b.balance.unwrap_or(general::Amount::new(0, 0))));
    Ok(())
}

fn blocks(c: &mut Client, n: i64) -> Result<()> {
    for p in c.pool_list_get(0, n)?.pools.unwrap_or_default() {
        println!(
            "{:>11}  {}  {:>4} tx  {}",
            p.pool_number.unwrap_or(0),
            hex::encode(p.hash.unwrap_or_default()),
            p.transactions_count.unwrap_or(0),
            fmt_time(p.time.unwrap_or(0))
        );
    }
    Ok(())
}

fn transactions(c: &mut Client, addr: &str, n: i64) -> Result<()> {
    let r = c.transactions_get(address(addr)?, 0, n)?;
    check(&r.status, "TransactionsGet")?;
    let list = r.transactions.unwrap_or_default();
    println!("{} transactions, the last {}:", r.total_trxns_count.unwrap_or(0), list.len());
    for t in list {
        let (id, x) = (t.id.unwrap_or_default(), t.trxn.unwrap_or_default());
        println!(
            "{:>11}.{:<4} {}  {} -> {}  {} CS  type {}",
            id.pool_seq.unwrap_or(0),
            id.index.unwrap_or(0),
            fmt_time(x.time_creation.unwrap_or(0)),
            bs58::encode(x.source.unwrap_or_default()).into_string(),
            bs58::encode(x.target.unwrap_or_default()).into_string(),
            fmt_amount(&x.amount.unwrap_or(general::Amount::new(0, 0))),
            x.type_.map(|t| t.0).unwrap_or(0)
        );
    }
    Ok(())
}

/// A signed CS transfer, and the bytes its signature covers:
/// csdb::Transaction::to_byte_stream_for_sig(), little endian.
fn build_transfer(key: &SigningKey, to: &str, amount: &str, inner_id: i64) -> Result<(api::Transaction, Vec<u8>)> {
    let max_fee = encode_max_fee(1.0); // the most the sender accepts to pay; the node charges the actual fee
    let source = key.verifying_key().to_bytes().to_vec();
    let target = address(to)?;
    let amount = parse_amount(amount)?;
    let mut signed = Vec::with_capacity(86);
    signed.extend_from_slice(&inner_id.to_le_bytes()[..6]); // inner id, 6 bytes
    signed.extend_from_slice(&source); // 32 bytes
    signed.extend_from_slice(&target); // 32 bytes
    signed.extend_from_slice(&amount.integral.to_le_bytes()); // amount
    signed.extend_from_slice(&(amount.fraction as u64).to_le_bytes());
    signed.extend_from_slice(&(max_fee as u16).to_le_bytes()); // max fee
    signed.push(CURRENCY_CS as u8); // currency
    signed.push(0); // number of user fields

    let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_millis() as i64;
    let t = api::Transaction {
        id: Some(inner_id),
        source: Some(source),
        target: Some(target),
        amount: Some(amount),
        balance: Some(general::Amount::new(0, 0)),
        currency: Some(CURRENCY_CS),
        signature: Some(key.sign(&signed).to_bytes().to_vec()),
        fee: Some(api::AmountCommission::new(max_fee)),
        time_creation: Some(now),
        user_fields: Some(Vec::new()),
        type_: Some(api::TransactionType::T_T_TRANSFER),
        pool_number: Some(0),
        ..Default::default()
    };
    Ok((t, signed))
}

fn sign(key_file: &str, to: &str, amount: &str, inner_id: &str) -> Result<()> {
    let (t, signed) = build_transfer(&read_key(key_file)?, to, amount, inner_id.parse()?)?;
    println!("source   : {}", bs58::encode(t.source.unwrap_or_default()).into_string());
    println!("signed   : {}", hex::encode(signed));
    println!("signature: {}", hex::encode(t.signature.unwrap_or_default()));
    Ok(())
}

fn transfer(c: &mut Client, key_file: &str, to: &str, amount: &str) -> Result<()> {
    let key = read_key(key_file)?;
    // the inner id must be the sender's last one + 1 (replay protection); a wallet the node has
    // never seen answers "not found": its first transaction has inner id 1
    let last = c.wallet_transactions_count_get(key.verifying_key().to_bytes().to_vec())?;
    let found = last.status.as_ref().and_then(|s| s.code).unwrap_or(0) == 0;
    let inner_id = if found { last.last_transaction_inner_id.unwrap_or(0) } else { 0 } + 1;
    let (t, _) = build_transfer(&key, to, amount, inner_id)?;
    let r = c.transaction_flow(t)?;
    check(&r.status, "TransactionFlow")?;
    println!(
        "sent: inner id {inner_id}, round {}, fee {} CS",
        r.round_num.unwrap_or(0),
        fmt_amount(&r.fee.unwrap_or(general::Amount::new(0, 0)))
    );
    Ok(())
}

// ---- smart contracts ----------------------------------------------------------------------

fn inner_id6(inner_id: i64) -> [u8; 6] {
    inner_id.to_le_bytes()[..6].try_into().unwrap()
}

fn next_inner_id(c: &mut Client, pk: &[u8]) -> Result<i64> {
    // the sender's last inner id + 1 (replay protection); a wallet the node has never seen: 1
    let last = c.wallet_transactions_count_get(pk.to_vec())?;
    let found = last.status.as_ref().and_then(|s| s.code).unwrap_or(0) == 0;
    Ok(if found { last.last_transaction_inner_id.unwrap_or(0) } else { 0 } + 1)
}

fn invocation(
    method: &str,
    params: Vec<general::Variant>,
    deploy: Option<api::SmartContractDeploy>,
    getter: bool,
) -> api::SmartContractInvocation {
    // every field the IDL does not mark optional is set: these are the bytes the node
    // serializes again to check the signature
    api::SmartContractInvocation {
        method: Some(method.to_string()),
        params: Some(params),
        used_contracts: Some(Vec::new()),
        forget_new_state: Some(getter),
        smart_contract_deploy: deploy,
        version: Some(1),
    }
}

/// A signed contract transaction (deploy or call). The invocation travels as the transaction's one
/// user field: its Thrift binary serialization, which the signature covers.
fn contract_transaction(
    key: &SigningKey,
    target: Vec<u8>,
    sci: api::SmartContractInvocation,
    kind: api::TransactionType,
    inner_id: i64,
) -> Result<api::Transaction> {
    let max_fee = encode_max_fee(CONTRACT_FEE);
    let source = key.verifying_key().to_bytes().to_vec();
    let mut code: Vec<u8> = Vec::new();
    sci.write_to_out_protocol(&mut TBinaryOutputProtocol::new(&mut code, true))?;
    let mut signed = Vec::with_capacity(90 + code.len());
    signed.extend_from_slice(&inner_id6(inner_id));
    signed.extend_from_slice(&source);
    signed.extend_from_slice(&target);
    signed.extend_from_slice(&0i32.to_le_bytes()); // amount: none
    signed.extend_from_slice(&0u64.to_le_bytes());
    signed.extend_from_slice(&(max_fee as u16).to_le_bytes());
    signed.push(CURRENCY_CS as u8);
    signed.push(1); // one user field: u32 size + bytes
    signed.extend_from_slice(&(code.len() as u32).to_le_bytes());
    signed.extend_from_slice(&code);

    let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_millis() as i64;
    Ok(api::Transaction {
        id: Some(inner_id),
        source: Some(source),
        target: Some(target),
        amount: Some(general::Amount::new(0, 0)),
        balance: Some(general::Amount::new(0, 0)),
        currency: Some(CURRENCY_CS),
        signature: Some(key.sign(&signed).to_bytes().to_vec()),
        smart_contract: Some(sci),
        fee: Some(api::AmountCommission::new(max_fee)),
        time_creation: Some(now),
        type_: Some(kind),
        ..Default::default()
    })
}

fn flag(text: &str) -> Result<bool> {
    match text.to_lowercase().as_str() {
        "true" => Ok(true),
        "false" => Ok(false),
        _ => Err("not a boolean".into()),
    }
}

/// A decimal number as BigDecimal text: digits, a point, an exponent.
fn decimal(text: &str) -> Result<String> {
    let plain = text.chars().all(|c| c.is_ascii_digit() || "+-.eE".contains(c));
    if plain && text.parse::<f64>().is_ok() { Ok(text.to_string()) } else { Err("not a decimal".into()) }
}

/// The Variant the executor matches with a declared Java type.
fn variant(declared: &str, text: &str) -> Result<general::Variant> {
    use general::Variant as V;
    let converted: Result<V> = match declared {
        "java.lang.String" => Ok(V::VString(text.to_string())),
        "java.math.BigDecimal" => decimal(text).map(V::VBigDecimal),
        "int" => text.parse().map(V::VInt).map_err(Into::into),
        "java.lang.Integer" => text.parse().map(V::VIntBox).map_err(Into::into),
        "long" => text.parse().map(V::VLong).map_err(Into::into),
        "java.lang.Long" => text.parse().map(V::VLongBox).map_err(Into::into),
        "boolean" => flag(text).map(V::VBoolean),
        "java.lang.Boolean" => flag(text).map(V::VBooleanBox),
        "double" => text.parse().map(|d| V::VDouble(OrderedFloat(d))).map_err(Into::into),
        "java.lang.Double" => text.parse().map(|d| V::VDoubleBox(OrderedFloat(d))).map_err(Into::into),
        "byte[]" => hex::decode(text).map(V::VByteArray).map_err(Into::into),
        _ => return Err(format!("arguments of type {declared} are not supported by this client").into()),
    };
    converted.map_err(|_| format!("'{text}' is not a {declared}").into())
}

/// A returned Variant as text, the same way in every sample.
fn show(v: &Option<general::Variant>) -> String {
    use general::Variant as V;
    match v {
        None => "(nothing)".into(),
        Some(V::VBoolean(b)) | Some(V::VBooleanBox(b)) => b.to_string(),
        Some(V::VString(s)) | Some(V::VBigDecimal(s)) => s.clone(),
        Some(V::VByte(n)) | Some(V::VByteBox(n)) => n.to_string(),
        Some(V::VShort(n)) | Some(V::VShortBox(n)) => n.to_string(),
        Some(V::VInt(n)) | Some(V::VIntBox(n)) => n.to_string(),
        Some(V::VLong(n)) | Some(V::VLongBox(n)) => n.to_string(),
        Some(V::VFloat(d)) | Some(V::VFloatBox(d)) | Some(V::VDouble(d)) | Some(V::VDoubleBox(d)) => format!("{:?}", d.0),
        Some(V::VByteArray(b)) => hex::encode(b),
        Some(V::VVoid(_)) => "void".into(),
        Some(V::VNull(_)) => "null".into(),
        Some(other) => format!("{other:?}"),
    }
}

/// The public methods of a deployed contract, as the node's executor lists them from its byte code.
fn contract_methods(c: &mut Client, contract: &[u8]) -> Result<Vec<general::MethodDescription>> {
    let deployed = c.smart_contract_get(contract.to_vec())?;
    check(&deployed.status, "SmartContractGet")?;
    let classes = deployed
        .smart_contract
        .and_then(|s| s.smart_contract_deploy)
        .and_then(|d| d.byte_code_objects)
        .unwrap_or_default();
    let r = c.contract_all_methods_get(classes)?;
    if r.code.unwrap_or(0) != 0 {
        return Err(format!("ContractAllMethodsGet: {}", r.message.unwrap_or_default()).into());
    }
    Ok(r.methods.unwrap_or_default())
}

fn arg_types(m: &general::MethodDescription) -> Vec<String> {
    m.arguments.iter().flatten().map(|a| a.type_.clone().unwrap_or_default()).collect()
}

fn typed_params(c: &mut Client, contract: &[u8], method: &str, texts: &[&str]) -> Result<Vec<general::Variant>> {
    let candidates: Vec<_> =
        contract_methods(c, contract)?.into_iter().filter(|m| m.name.as_deref() == Some(method)).collect();
    let Some(found) = candidates.iter().find(|m| arg_types(m).len() == texts.len()) else {
        let there = if candidates.is_empty() {
            String::new()
        } else {
            let list: Vec<String> = candidates.iter().map(|m| format!("{method}({})", arg_types(m).join(", "))).collect();
            format!("; there is {}", list.join(", "))
        };
        return Err(format!("no method {method} with {} arguments{there}", texts.len()).into());
    };
    arg_types(found).iter().zip(texts).map(|(declared, text)| variant(declared, text)).collect()
}

fn methods(c: &mut Client, contract: &str) -> Result<()> {
    let mut all = contract_methods(c, &address(contract)?)?;
    all.sort_by(|a, b| a.name.cmp(&b.name));
    for m in all {
        let args: Vec<String> = m
            .arguments
            .iter()
            .flatten()
            .map(|a| format!("{} {}", a.type_.clone().unwrap_or_default(), a.name.clone().unwrap_or_default()))
            .collect();
        println!("{} {}({})", m.return_type.unwrap_or_default(), m.name.unwrap_or_default(), args.join(", "));
    }
    Ok(())
}

fn deploy(c: &mut Client, key_file: &str, source_file: &str) -> Result<()> {
    let key = read_key(key_file)?;
    let pk = key.verifying_key().to_bytes().to_vec();
    let source = std::fs::read_to_string(source_file)?;
    // the node's executor compiles the source and checks it against its rules
    let compiled = c.smart_contract_compile(source.clone())?;
    check(&compiled.status, "SmartContractCompile")?;
    let classes = compiled.byte_code_objects.unwrap_or_default();
    let token_standard = compiled.token_standard.unwrap_or(0);
    println!("compiled: {} class(es), token standard {token_standard}", classes.len());
    let inner_id = next_inner_id(c, &pk)?;
    // the address of the new contract (SmartContracts::get_valid_smart_address):
    // blake2s-256(deployer public key, 6-byte inner id, the byte code of every class in order)
    let mut blake = Blake2s256::new();
    blake.update(&pk);
    blake.update(inner_id6(inner_id));
    for class in &classes {
        blake.update(class.byte_code.as_deref().unwrap_or_default());
    }
    let target = blake.finalize().to_vec();

    let deploy = api::SmartContractDeploy {
        source_code: Some(source),
        byte_code_objects: Some(classes),
        hash_state: Some(String::new()),
        token_standard: Some(token_standard),
        lang: Some(0),
        methods: None,
    };
    let t = contract_transaction(
        &key,
        target.clone(),
        invocation("", Vec::new(), Some(deploy), false),
        api::TransactionType::T_T_CONTRACT_DEPLOY,
        inner_id,
    )?;
    let r = c.transaction_flow(t)?;
    check(&r.status, "TransactionFlow")?;
    println!(
        "sent: inner id {inner_id}, round {}, fee {} CS",
        r.round_num.unwrap_or(0),
        fmt_amount(&r.fee.unwrap_or(general::Amount::new(0, 0)))
    );
    // the contract exists once the deploy is in a block and its executions are done
    let deadline = Instant::now() + Duration::from_secs(180);
    while c.smart_contract_get(target.clone())?.status.and_then(|s| s.code).unwrap_or(0) != 0 {
        if Instant::now() > deadline {
            return Err(format!("{} is not deployed after 180 s", bs58::encode(&target).into_string()).into());
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    println!("contract: {}", bs58::encode(&target).into_string());
    Ok(())
}

fn call(c: &mut Client, key_file: &str, contract: &str, method: &str, texts: &[&str]) -> Result<()> {
    let key = read_key(key_file)?;
    let contract = address(contract)?;
    let params = typed_params(c, &contract, method, texts)?;
    let inner_id = next_inner_id(c, &key.verifying_key().to_bytes())?;
    let t = contract_transaction(
        &key,
        contract,
        invocation(method, params, None, false),
        api::TransactionType::T_T_CONTRACT_CALL,
        inner_id,
    )?;
    // the node answers once the confidants executed the call: the result is the method's return value
    let r = c.transaction_flow(t)?;
    check(&r.status, "TransactionFlow")?;
    println!(
        "sent: inner id {inner_id}, round {}, fee {} CS",
        r.round_num.unwrap_or(0),
        fmt_amount(&r.fee.unwrap_or(general::Amount::new(0, 0)))
    );
    println!("returned: {}", show(&r.smart_contract_result));
    Ok(())
}

fn get(c: &mut Client, contract: &str, method: &str, texts: &[&str]) -> Result<()> {
    let contract = address(contract)?;
    let params = typed_params(c, &contract, method, texts)?;
    // a getter: TransactionFlow with forgetNewState runs the method on the node's own executor against
    // the current state and throws the new state away; nothing is sent, no fee, no signature is checked
    let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_millis() as i64;
    let t = api::Transaction {
        id: Some(0),
        source: Some(vec![0; 32]),
        target: Some(contract),
        amount: Some(general::Amount::new(0, 0)),
        balance: Some(general::Amount::new(0, 0)),
        currency: Some(CURRENCY_CS),
        signature: Some(vec![0; 64]),
        smart_contract: Some(invocation(method, params, None, true)),
        fee: Some(api::AmountCommission::new(encode_max_fee(CONTRACT_FEE))),
        time_creation: Some(now),
        type_: Some(api::TransactionType::T_T_CONTRACT_CALL),
        ..Default::default()
    };
    let r = c.transaction_flow(t)?;
    check(&r.status, "TransactionFlow")?;
    println!("{}", show(&r.smart_contract_result));
    Ok(())
}

fn usage() -> ! {
    eprintln!(
        "usage: credits-client [--node HOST:PORT] <command>
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
--node defaults to 127.0.0.1:9090"
    );
    exit(2)
}

fn run(args: &[String]) -> Result<()> {
    let (node, args) = match args {
        [flag, node, rest @ ..] if flag == "--node" => (node.as_str(), rest),
        _ => ("127.0.0.1:9090", args),
    };
    let args: Vec<&str> = args.iter().map(String::as_str).collect();
    if let ["sign", key, to, amount, id] = args[..] {
        return sign(key, to, amount, id);
    }
    let n = |s: Option<&&str>| -> Result<i64> { Ok(s.map(|s| s.parse()).transpose()?.unwrap_or(10)) };
    match args[..] {
        ["status"] => status(&mut connect(node)?),
        ["balance", addr] => balance(&mut connect(node)?, addr),
        ["blocks"] | ["blocks", _] => blocks(&mut connect(node)?, n(args.get(1))?),
        ["tx", addr] | ["tx", addr, _] => transactions(&mut connect(node)?, addr, n(args.get(2))?),
        ["transfer", key, to, amount] => transfer(&mut connect(node)?, key, to, amount),
        ["deploy", key, source] => deploy(&mut connect(node)?, key, source),
        ["call", key, contract, method, ..] => call(&mut connect(node)?, key, contract, method, &args[4..]),
        ["get", contract, method, ..] => get(&mut connect(node)?, contract, method, &args[3..]),
        ["methods", contract] => methods(&mut connect(node)?, contract),
        _ => usage(),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(e) = run(&args) {
        eprintln!("{e}");
        exit(1);
    }
}
