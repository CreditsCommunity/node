# C# client

`credits-client`: the node API from C# (.NET 8 or later), with the Thrift classes generated from the
IDL by MSBuild before compiling. See [../README.md](../README.md) for the commands, the key file
format and the transfer format.

## Install

- the .NET SDK 8 or later ([dot.net](https://dot.net)); the program targets `net8.0` and rolls
  forward to newer runtimes;
- the Thrift compiler 0.19.0, as `thrift` on the `PATH` (`sudo apt install thrift-compiler` on
  Ubuntu 24.04), to match the ApacheThrift 0.19.0 package; another path with `-p:Thrift=/path/to/thrift`;
- the IDL submodule: `git submodule update --init third-party/thrift-interface-definitions` from the
  repository root (another directory with `-p:CreditsIdl=DIR`).

Dependencies (pinned in `CreditsClient.csproj`): `ApacheThrift` and `BouncyCastle.Cryptography`
(.NET has no ed25519). Base58 is a small class of the sample.

The IDL declares no `netstd` namespace, and generating `api.thrift` and `general.thrift` into one
namespace does not compile (both define the same `DeepCopy` extensions): the build copies the two
files into `obj/thrift/idl` with a first line `namespace netstd Credits.Api` / `Credits.General`.

## Build

```sh
cd samples/clients/csharp
dotnet build -c Release      # bin/Release/net8.0/credits-client.dll
```

## Run

```sh
dotnet bin/Release/net8.0/credits-client.dll --node 127.0.0.1:9090 status
dotnet bin/Release/net8.0/credits-client.dll balance u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w
dotnet bin/Release/net8.0/credits-client.dll blocks 3
dotnet bin/Release/net8.0/credits-client.dll tx u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 5
dotnet bin/Release/net8.0/credits-client.dll sign ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5 7
# SPENDS REAL CS ON MAINNET: try it on a test network or the fake node first
dotnet bin/Release/net8.0/credits-client.dll --node 127.0.0.1:19090 transfer ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5
```

`--node` defaults to `127.0.0.1:9090`. `dotnet run -c Release -- status` works too.
