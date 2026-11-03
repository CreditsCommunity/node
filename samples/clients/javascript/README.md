# JavaScript client

`credits-client.js`: the node API from Node.js 18 or later, with the Thrift code generated into
`gen/` by `npm install`. See [../README.md](../README.md) for the commands, the key file format and
the transfer format.

## Install

- Node.js 18 or later and npm;
- the Thrift compiler 0.19.0, as `thrift` on the `PATH` (`sudo apt install thrift-compiler` on
  Ubuntu 24.04), to match the `thrift` 0.19.0 package;
- the IDL submodule: `git submodule update --init third-party/thrift-interface-definitions` from the
  repository root.

The only dependency is `thrift` (pinned in `package.json`, locked in `package-lock.json`); ed25519
comes from `node:crypto` (so does blake2s, for contract addresses) and base58 is a few lines of the
sample. i64 values arrive as `node-int64` objects: the sample reads them as `BigInt`, so amounts
stay exact.

## Build

```sh
cd samples/clients/javascript
npm ci            # installs thrift, then runs "npm run generate" (gen/, not committed)
```

## Run

```sh
node credits-client.js --node 127.0.0.1:9090 status
node credits-client.js balance u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w
node credits-client.js blocks 3
node credits-client.js tx u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 5
node credits-client.js sign ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5 7
# SPENDS REAL CS ON MAINNET: try it on a test network or the fake node first
node credits-client.js --node 127.0.0.1:19090 transfer ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5
# smart contracts: deploy and call SPEND REAL CS ON MAINNET too; get and methods are free
node credits-client.js --node 127.0.0.1:19090 deploy ../testdata/test-key.txt ../../contracts/Counter.java
node credits-client.js --node 127.0.0.1:19090 call ../testdata/test-key.txt <contract> add 5
node credits-client.js --node 127.0.0.1:19090 get <contract> isOpen
node credits-client.js --node 127.0.0.1:19090 methods <contract>
```

`--node` defaults to `127.0.0.1:9090`.
Contract arguments are given as text and converted to the type the method declares (the node
refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.
`deploy` prints the new contract's address once the deploy is in a block (it waits up to 180 s).
