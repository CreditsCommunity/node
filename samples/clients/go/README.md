# Go client

`credits-client`: the node API from Go, with the Thrift code generated from the IDL by
`go generate`. See [../README.md](../README.md) for the commands, the key file format and the
transfer format.

## Install

- Go 1.22 or later ([go.dev/dl](https://go.dev/dl/));
- the Thrift compiler 0.19.0, as `thrift` on the `PATH` (`sudo apt install thrift-compiler` on
  Ubuntu 24.04), to match `github.com/apache/thrift v0.19.0` in `go.mod`;
- the IDL submodule: `git submodule update --init third-party/thrift-interface-definitions` from the
  repository root.

Dependencies (pinned in `go.mod` and `go.sum`): `github.com/apache/thrift`,
`github.com/mr-tron/base58` and `golang.org/x/crypto` (blake2s, for contract addresses); ed25519 is
`crypto/ed25519` from the standard library.

## Build

```sh
cd samples/clients/go
go generate     # writes gen/api and gen/general (not committed)
go build -o credits-client .
```

## Run

```sh
./credits-client -node 127.0.0.1:9090 status
./credits-client balance u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w
./credits-client blocks 3
./credits-client tx u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 5
./credits-client sign ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5 7
# SPENDS REAL CS ON MAINNET: try it on a test network or the fake node first
./credits-client -node 127.0.0.1:19090 transfer ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5
# smart contracts: deploy and call SPEND REAL CS ON MAINNET too; get and methods are free
./credits-client -node 127.0.0.1:19090 deploy ../testdata/test-key.txt ../../contracts/Counter.java
./credits-client -node 127.0.0.1:19090 call ../testdata/test-key.txt <contract> add 5
./credits-client -node 127.0.0.1:19090 get <contract> isOpen
./credits-client -node 127.0.0.1:19090 methods <contract>
```

`-node` defaults to `127.0.0.1:9090`.
Contract arguments are given as text and converted to the type the method declares (the node
refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.
`deploy` prints the new contract's address once the deploy is in a block (it waits up to 180 s).
