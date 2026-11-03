# Rust client

`credits-client`: the node API from Rust, with the Thrift code generated from the IDL by
`build.rs`. See [../README.md](../README.md) for the commands, the key file format and the transfer
format.

## Install

- a recent stable Rust ([rustup.rs](https://rustup.rs));
- the Thrift compiler 0.19.0, as `thrift` on the `PATH` (`sudo apt install thrift-compiler` on
  Ubuntu 24.04), or set `THRIFT=/path/to/thrift`;
- the IDL submodule: `git submodule update --init third-party/thrift-interface-definitions` from the
  repository root (another directory with `CREDITS_IDL=DIR`).

Dependencies (pinned in `Cargo.toml`, locked in `Cargo.lock`): `thrift` 0.17.0 (the latest crate on
crates.io; the code of the 0.19.0 compiler builds with it), `ed25519-dalek`, `bs58`, `hex`, `chrono`.

## Build

```sh
cd samples/clients/rust
cargo build --release     # target/release/credits-client
```

## Run

```sh
target/release/credits-client --node 127.0.0.1:9090 status
target/release/credits-client balance u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w
target/release/credits-client blocks 3
target/release/credits-client tx u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 5
target/release/credits-client sign ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5 7
# SPENDS REAL CS ON MAINNET: try it on a test network or the fake node first
target/release/credits-client --node 127.0.0.1:19090 transfer ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5
```

`--node` defaults to `127.0.0.1:9090`. In the generated code every struct field is an `Option`.
