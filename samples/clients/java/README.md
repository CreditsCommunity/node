# Java client

`CreditsClient`: the node API from Java 17, built with Maven, which generates the Thrift classes
from the IDL before compiling. See [../README.md](../README.md) for the commands, the key file
format and the transfer format.

## Install

- a JDK 17 or later and Maven 3.6 or later;
- the Thrift compiler 0.19.0, as `thrift` on the `PATH` (`sudo apt install thrift-compiler` on
  Ubuntu 24.04), to match libthrift 0.19.0; another path with `-Dthrift.executable=/path/to/thrift`;
- the IDL submodule: `git submodule update --init third-party/thrift-interface-definitions` from the
  repository root (another directory with `-Dthrift.idl=DIR`).

Dependencies (pinned in `pom.xml`): `org.apache.thrift:libthrift`, `org.bouncycastle:bcprov-jdk18on`
for ed25519 (the JDK signs with Ed25519 but does not derive a public key from a raw seed) and
`slf4j-nop`, which keeps libthrift's logging quiet. Base58 is a small class of the sample.

## Build

```sh
cd samples/clients/java
mvn -q package          # target/generated-sources/thrift, then target/credits-client.jar
```

## Run

```sh
java -jar target/credits-client.jar --node 127.0.0.1:9090 status
java -jar target/credits-client.jar balance u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w
java -jar target/credits-client.jar blocks 3
java -jar target/credits-client.jar tx u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 5
java -jar target/credits-client.jar sign ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5 7
# SPENDS REAL CS ON MAINNET: try it on a test network or the fake node first
java -jar target/credits-client.jar --node 127.0.0.1:19090 transfer ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5
```

`--node` defaults to `127.0.0.1:9090`. The generated packages are the IDL's:
`com.credits.client.node.thrift.generated` (api) and `com.credits.general.thrift.generated`.
