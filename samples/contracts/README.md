# Smart contract examples

Small Java contracts for the Credits network, each commented, and how to deploy and call them with
the [sample clients](../clients/). Every example here is deployed and used on a private network by
CI (see [Proof on a private network](#proof-on-a-private-network)).

| File | What it shows |
|---|---|
| [Counter.java](Counter.java) | the simplest contract: state, an owner set by the constructor, calls that change the state, getters, a method that refuses a call, a `payable()` that refuses CS |
| [TestToken.java](TestToken.java) | **Community Test Token (CTT), for tests only**: a token of standard 4 (`ExtensionTokenStandard` v1) that the node's token API lists; transfer, approve, allowance, transferFrom |
| [Token.java](Token.java) | a token template to start a real token from: amounts checked against the decimals, approve with ERC-20 semantics, a sale through `payable()` |
| [TipJar.java](TipJar.java) | a payable contract: it records who paid how much, and its owner withdraws with `sendTransaction()` |
| [Faucet.java](Faucet.java) | a contract that reads its own balance (`getBalance(contractAddress)`), and why its result depends on the moment it runs |

**TestToken is not a real asset.** Use it on a private network or a test network only; the samples
and the CI never deploy it on mainnet. For a token of your own start from Token.java, change its
name, symbol, decimals and supply, and test it on a private network first.

## How a contract is written

A contract is one public Java class that extends `SmartContract` from the contract API
([CreditsCommunity/smart-contract-api](https://github.com/CreditsCommunity/smart-contract-api)).
Use the v3 API, `com.credits.scapi.v3.SmartContract`, as the examples do:

- **State.** The fields are the state. After every call that changes it the executor serializes
  the whole object, and the block records the hash of the new state. Fields must be serializable
  (strings, numbers, `BigDecimal`, `HashMap`, `HashSet`, your own `Serializable` classes).
- **Constructor.** It runs once, at deployment. `initiator` is then the deployer: keep it in a
  field if the contract has an owner.
- **Methods.** Every public method can be called. Inside a call, `initiator` is the base58
  address of the sender and `contractAddress` the contract's own. A method that throws changes
  nothing (the fee is still paid); the exception message is in the node's log.
- **Argument types.** Use `String` for addresses, `BigDecimal` for amounts, and `int`, `long`,
  `boolean`, `double` or their wrappers otherwise. A call must pass each argument as the Variant of
  the declared type (`v_big_decimal` for a `BigDecimal`, `v_string` for a `String`): the node
  refuses a call no method matches. The sample clients read the declared types from the node and
  convert the text you type.
- **`payable(BigDecimal amount, byte[] userData)`.** v3 makes every contract declare it. A plain
  CS transfer to a contract calls it, with `initiator` set to the payer. Returning normally
  accepts the payment; throwing refuses it, and the node gives the CS back to the payer, who still
  pays the fee.
- **API methods** of `SmartContract`: `sendTransaction(contractAddress, to, amount)` pays CS from
  the contract's balance (the transfer lands only if the call succeeds);
  `getBalance(address)` reads a balance at the moment the method runs;
  `invokeExternalContract(...)` calls another contract (see the note below); `getSeed()` gives the
  block's random seed.
- **What the executor refuses.** The executor compiles the source with its own class path and runs
  it in a sandbox: no files, network, threads, reflection, system time or class loading. Anything
  of that kind fails at deployment or when called. `SmartContractCompile` reports compile errors.
- **`@Getter`** (`com.credits.scapi.annotations.Getter`) only documents which methods are meant to
  be read without a transaction.

Contract-to-contract calls (`invokeExternalContract`, `@UsingContract`) do not work with the
mainnet node and executor (see [ci/testnet/contracts.py](../../ci/testnet/contracts.py)); the
examples do not use them.

## Deploying and calling

The [sample clients](../clients/) have four contract commands, the same in every language
(Python shown; [samples/clients/README.md](../clients/README.md) has the others):

```sh
cd samples/clients/python
# on a private or test network: deploy and call SPEND REAL CS ON MAINNET
python3 credits_client.py --node 127.0.0.1:9090 deploy my-key.txt ../../contracts/Counter.java
#   compiled: 1 class(es), token standard 0
#   sent: inner id 3, round 1234, fee 0.0123 CS
#   contract: 7Hq...
python3 credits_client.py methods 7Hq...                     # the methods and their argument types
python3 credits_client.py call my-key.txt 7Hq... add 4       # a transaction: returned: 4
python3 credits_client.py get 7Hq... getValue                # a getter: 4
python3 credits_client.py transfer my-key.txt 9Ab...TIPJAR 3 # a CS payment: payable()
```

- **deploy** asks the node to compile the source (`SmartContractCompile`, done by the node's
  executor), derives the contract's address (blake2s of the deployer's key, the transaction's
  inner id and the byte code), signs a deploy transaction and waits until the node lists the
  contract.
- **call** sends a signed transaction. The confidants execute the method; the node answers once
  the call is in a block, with the method's return value. A call that throws is answered with an
  error ("call to contract failed").
- **get** runs the method as a getter: `TransactionFlow` with `forgetNewState`, which the node runs
  on its own executor against the current state and then forgets. Nothing is signed, sent or paid,
  and nothing changes. Use it for every method that only reads. A getter can call any method, but
  what a state-changing method returns there is only what it would return now.
- **methods** lists the public methods with their declared types (`ContractAllMethodsGet`).

## Fees

A deploy and every call are transactions: the sender pays a fee, taken from the CS balance. The
clients set a maximum fee of 10 CS for contract transactions (1 CS for transfers); the node
charges the actual one, which the clients print (`fee ... CS`), and which grows with the size of
the transaction (a deploy carries the source and the byte code) and the execution. A call that
throws still pays its fee. A transfer the contract emits with `sendTransaction()` has its own
fee. Getters are free.

## A contract that reads its own balance

Faucet decides from `getBalance(contractAddress)`. That makes its result depend on *when* the call
runs, not only on the calls before it: on transfers to and from the contract, and on where the call
falls among them in its block. The confidants that execute a call live agree on that moment, and the
chain records the hash of the resulting state.

A node that rebuilds contract states by executing the calls again while it syncs reads the balance
as of the block it is replaying, which may already include that block's payouts. Such a call can
then fail where it succeeded live, and the node's state differs from the one the chain declares.
On mainnet this happened with dice games; the community node therefore takes the declared state
from the confidants instead of trusting its own re-execution: see
[differences.md, "Contract states while syncing"](../../docs/differences.md).

When you can, keep the numbers the contract needs in its own state, as TipJar does (what was paid
in minus what was taken out): its result is then the same whenever and wherever it is executed.

## Proof on a private network

The workflow [testnet.yml](../../.github/workflows/testnet.yml) with the inputs `contracts=1` and
`sample_contracts=1` starts a private network with a contract executor per node and runs
[ci/testnet/sample_contracts.sh](../../ci/testnet/sample_contracts.sh) with the Python and the Java
client: every example above except Token.java is deployed and called, getters and balances are
checked, calls that must fail are refused, and the node's token API must report TestToken.
[samples.yml](../../.github/workflows/samples.yml) checks every client's contract commands offline
against a fake node.

To run the same locally, see [ci/testnet/run.sh](../../ci/testnet/run.sh) (`CONTRACTS=1 FUND=1
SAMPLE_CONTRACTS=1`, with a `-DCREDITS_TESTNET=ON` build and `EXECUTOR_JAR`).
