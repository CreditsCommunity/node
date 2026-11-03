# Test data of the sample clients

- `test-key.txt`: a throwaway ed25519 key (seed bytes 0, 1, …, 31, then the public key, base58);
  its address is `FAe4sisG95oZ42w7buUn5qEE4TAnfTTFPiguZUHmhiF`. Everybody knows its secret:
  **never send funds to it.**
- `sign-expected.txt`: what every sample prints for
  `sign test-key.txt u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w 12.5 7`, the same bytes and signature
  as `ci/testnet/fund.py` builds.
- `fake_node.py`: a stand-in for a node that checks the transfers it receives (see
  [../README.md](../README.md#transfers)).
