# Python client

`credits_client.py`: the node API from Python 3.8+. [thriftpy2](https://github.com/Thriftpy/thriftpy2)
reads the Thrift IDL at run time, so nothing is generated; see [../README.md](../README.md) for the
commands, the key file format and the transfer format.

## Install

```sh
git submodule update --init third-party/thrift-interface-definitions   # from the repository root
python3 -m venv .venv && . .venv/bin/activate
pip install -r samples/clients/python/requirements.txt
```

The IDL is looked up in `../../../third-party/thrift-interface-definitions` next to the script;
give another directory with `--idl DIR` or `CREDITS_IDL=DIR`.

## Run

```sh
cd samples/clients/python
python3 credits_client.py --node 127.0.0.1:9090 status
python3 credits_client.py balance u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w
python3 credits_client.py blocks 3
python3 credits_client.py tx u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 5
python3 credits_client.py sign ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5 7
# SPENDS REAL CS ON MAINNET: try it on a test network or the fake node first
python3 credits_client.py --node 127.0.0.1:19090 transfer ../testdata/test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5
# smart contracts: deploy and call SPEND REAL CS ON MAINNET too; get and methods are free
python3 credits_client.py --node 127.0.0.1:19090 deploy ../testdata/test-key.txt ../../contracts/Counter.java
python3 credits_client.py --node 127.0.0.1:19090 call ../testdata/test-key.txt <contract> add 5
python3 credits_client.py --node 127.0.0.1:19090 get <contract> isOpen
python3 credits_client.py --node 127.0.0.1:19090 methods <contract>
```

`--node` defaults to `127.0.0.1:9090`. Contract arguments are given as text and converted to the
type the method declares; see [../README.md](../README.md#smart-contracts) and the examples in
[../../contracts](../../contracts/).
