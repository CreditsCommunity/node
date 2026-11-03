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
comes from `node:crypto` and base58 is a few lines of the sample. i64 values arrive as `node-int64`
objects: the sample reads them as `BigInt`, so amounts stay exact.

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
```

`--node` defaults to `127.0.0.1:9090`.
