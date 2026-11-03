//! A small client of the Credits node API (Thrift, binary protocol, buffered transport).
//!
//! ```text
//! credits-client [--node HOST:PORT] status
//! credits-client [--node HOST:PORT] balance <address>
//! credits-client [--node HOST:PORT] blocks [n]
//! credits-client [--node HOST:PORT] tx <address> [n]
//! credits-client [--node HOST:PORT] transfer <key-file> <to-address> <amount>
//! credits-client sign <key-file> <to-address> <amount> <inner-id>
//! ```
//!
//! transfer SIGNS AND SENDS A REAL TRANSACTION: on mainnet it spends real CS. Try it on a private
//! or test network first. sign builds the same transaction offline and prints it.

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
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use api::{APISyncClient, TAPISyncClient};
use ed25519_dalek::{Signer, SigningKey};
use thrift::protocol::{TBinaryInputProtocol, TBinaryOutputProtocol};
use thrift::transport::{
    ReadHalf, TBufferedReadTransport, TBufferedWriteTransport, TIoChannel, TTcpChannel, WriteHalf,
};

type Client = APISyncClient<
    TBinaryInputProtocol<TBufferedReadTransport<ReadHalf<TTcpChannel>>>,
    TBinaryOutputProtocol<TBufferedWriteTransport<WriteHalf<TTcpChannel>>>,
>;
type Result<T> = std::result::Result<T, Box<dyn Error>>;

const CURRENCY_CS: i8 = 1;
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
    stream.set_read_timeout(Some(Duration::from_secs(30)))?;
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

fn usage() -> ! {
    eprintln!(
        "usage: credits-client [--node HOST:PORT] <command>
  status                               last block, round, head hash and time, wallet count
  balance <address>                    balance of a wallet (base58 public key)
  blocks [n]                           the last n blocks (default 10)
  tx <address> [n]                     the last n transactions of a wallet (default 10)
  transfer <key-file> <to> <amount>    sign and send a transfer: SPENDS REAL CS ON MAINNET
  sign <key-file> <to> <amount> <id>   build and sign a transfer with inner id <id>, print it, send nothing
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
