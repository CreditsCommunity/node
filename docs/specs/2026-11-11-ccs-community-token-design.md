# CCS, the Community CS token: design

Status: draft for the owner's approval, 2026-11-11.
Scope: a token contract on the Credits mainnet, an optional feature of the community node, the community wallet, and an economic simulation.

Decisions are marked in two ways:
- **[owner]**: the project owner decided it in the design conversation;
- **[proposal]**: a choice made while writing this document, which the builder may revisit if they find a better option.

## 1. Intent

CCS rewards the people who keep community nodes running and taking part in consensus. It is also the currency for services built on the Credits chain, starting with the services a full node can sell.

**Why:**
- Running a node pays in CS today, split by stake among the trusted nodes of each round. Nothing rewards running the community software as such, or keeping the full history.
- Users depend on one official monitor for the history of a wallet or a contract. That monitor recently turned out to carry a wrong wallet state.
- Certified history has a concrete market. Third-party wallets, such as Baba Wallet, can ask for a signed, verifiable statement of all movements of their users' wallets, for example for tax returns.
- Every mechanism should revolve around CCS, so that the service economy is not tied to CS **[owner]**. CS remains the only real money that buys CCS.

## 2. Goals

1. **Participation reward.** A community node that takes part in consensus earns CCS on its node wallet, the same wallet that receives its CS reward **[owner]**.
2. **Reward choice.** The operator chooses how the CS reward is taken: all CS, all CCS, or a share of each. The share taken as CCS is converted, not created: the CS actually received is used to buy CCS **[owner]**.
3. **A reason to convert.** Converting a reward costs a lower fee than an ordinary purchase **[owner]**.
4. **Fixed total supply.** The total number of CCS is fixed. A node that does not convert still earns CCS, but less and less over time **[owner]**.
5. **Full nodes get more.** A full node must get more than a pruned one **[owner]**. It gets more because it can sell services on the historical data that a pruned node does not have: wallet history, contract history, search and processing **[owner]**.
6. **Services.** A registry on the chain lists services; a call through the contract pays for one in CCS. The service itself may run off-chain **[owner]**. Registering a service costs CCS **[owner]**. Later services include certified storage, messaging, logging, computation and distributed processing **[owner]**.
7. **Self-adjusting parameters.** Prices, fees and costs follow demand and availability by fixed rules. No participant can change them **[owner]**.
8. **Fix or replace without one owner.** The contract can be paused, restored or replaced by an elected committee, never by one person **[owner]**.
9. **First customer.** The community wallet is the first customer, used by node owners both pruned and full **[owner]**. Third-party apps come next **[owner]**.

## 3. Non-goals and anti-goals

**Non-goals:**
- **No change to the consensus rules or the block format.** The official nodes must keep working unchanged. CS rewards are still paid by the protocol as today **[owner]** (rewards reach the node wallet).
- **Storage, messaging, computation and distributed processing are out of scope for the first version.** The registry must be able to hold them later.
- **No exchange listing and no external market.** The only market is the one inside the contract.

**Anti-goals: the project fails, even if it works technically, if:**
- **Node owners have to do more than run the node.** That includes voting, signing or reacting to an incident **[owner]**: "the owners only want to take part to earn".
- **One person can change the rules, move funds or assign themselves CCS.**
- **The rules are too complicated to explain in a few sentences to a node owner.**
- **A correct node can end up with a different chain state** because of the CCS feature.

## 4. Constraints from the platform

Verified in `smart-contract-api` and from what we know of the mainnet.

**What a contract can do:**
- it sees the initiator of a call (`initiator`) and its own address;
- it can read any address's CS balance (`getBalance`) and receive CS: a payable call, or a transfer with the contract as target;
- it can send CS with `sendTransaction(from, to, double amount, double fee)`, as an emitted transaction;
- it gets a seed (`getSeed`); v3 contracts can read the blockchain time (`getBlockchainTimeMills`).

**What it cannot do:**
- **It cannot see blocks, rounds, confidants or rewards.** Participation in consensus must therefore be reported to it (section 7).
- **It cannot call other contracts.** Cross-contract calls are disabled on mainnet in practice. So token, reserve, market, registry, service payments, committee and participation ledger must all live in **one contract**.

**Risks to design and test for:**
- **Emitted CS transactions:**
  - their fees are charged by the node's fee rule;
  - CreditsCommunity/node #94 fixed a syncing node's attribution of those fees, and an attribution error caused the 7hWM state divergence fixed in community.18.2;
  - every CS payout must be tested on mixed official and community networks, including a node that syncs from scratch.
- **Amounts are `double` in `sendTransaction`.** The contract keeps its own accounting in `BigDecimal` and rounds every CS payout down to a granularity that a double represents exactly. Leftovers stay in the reserve.
- **Executor:**
  - the mainnet executor is build 1518, which reports v2 token interfaces as "not a token" (see `docs/tokens.md`); the contract must work with build 1518;
  - the contract must stay within the executor's time and size limits: no loops over all holders and no unbounded lists in one call.

**Current reward numbers** (mainnet, measured 2026-10-31):
- **Totals:** 1 CS per block, about 72,600 CS per day, split by stake among the real signers of each round.
- **Per node:** a node with a full stake earns about 4,500 CS per day.

## 5. The token

**Basics:**
- **Standard:** the v1 token standard, `BasicTokenStandard` with `BigDecimal` amounts. The wallet already shows tokens of that standard. Name "Community CS", symbol `CCS` **[owner]**.
- **Supply:** a fixed total supply N, minted entirely to the contract at deployment **[owner]**. No mint function exists afterwards.
- **Transfers:** CCS can be sent between wallets **[owner]**.

**The contract's balance is split into stocks that the contract tracks itself:**
- **Emission stock:** CCS still to be distributed as participation rewards.
- **Sale stock:** CCS available to buy with CS. Sold CCS that come back go here.
- **CS reserve:** the CS paid for CCS, from which sellers are paid.

The split of N between emission and sale, and the value of N, are left to the simulation (section 11).

## 6. The internal market

There is no fixed rate between CS and CCS. The price comes from the market inside the contract **[owner]**.

**[proposal] A price curve:**
- The CS price of one CCS is a function of how many CCS are out of the sale stock: it rises as the stock empties and falls as CCS come back.
- **Buy:** pay CS and receive CCS at the curve price, integrated over the amount bought.
- **Sell:** return CCS and receive CS from the reserve at the curve price, minus the sell fee.
- The reserve backs only the CCS that were bought. CCS earned as participation rewards were never paid for, so the curve alone gives them no CS value once bought CCS have been sold back.

An alternative is a constant-product pool. The owner's wording ("variability of the price depending on requests and availability") fits both. The builder chooses after the simulation and records why.

**Floor fund** **[owner: approved in this form]**:
- **The fund.** The contract keeps a floor fund in CS, separate from the purchase reserve, fed only by a share of every CS fee: conversion, purchase and sale. The share is set by the simulation, for example half of each fee.
- **The floor.** The floor value of one CCS is floor fund ÷ CCS in circulation. CCS in circulation means all CCS outside the contract's own stocks.
- **Selling.** A seller receives the higher of the curve price and the floor value. The CS of a sale at the floor come from the floor fund; the CS of a sale at the curve price come from the reserve.
- **Why it never runs dry.** A sale at the floor removes exactly the seller's share, so the floor value is unchanged for everyone who remains. If everyone sells, the last seller gets as much per CCS as the first. Nobody gains by leaving first.
- **What moves it:**
  - fees raise the floor;
  - CCS spent on services that return to the stocks raise it, because fewer CCS circulate;
  - new participation rewards lower it, because the same fund is shared by more CCS.
- **What it promises.** A CCS is never worth less than its share of the fund. It does not promise a fixed number, and at launch the floor is close to zero. The wallet must say exactly this.

**Solvency property for the simulation and the tests.** Reserve ≥ the curve's buy-back value of all CCS bought and still circulating, and floor fund ≥ floor value × CCS in circulation, at every step, including a run where everyone sells.

**Fees** **[owner: different fees, conversion cheaper]**:

| Operation | Fee |
|---|---|
| Conversion of a node's CS reward | low, e.g. 0–1 % |
| Ordinary purchase | higher, e.g. 3–5 % |
| Sale back to CS | set by the simulation |

- **Defining "conversion"** **[proposal]**: a purchase is a conversion when its initiator is a node wallet registered in the participation ledger (section 7), and the CS amount is at most the CS rewards that ledger credits to that wallet in the period. Anything above that is an ordinary purchase. This stops anyone from routing ordinary purchases through a node wallet without limit.
- **Where fees go** **[proposal]**: part to the floor fund, as described below, and the rest to the reserve. The simulation sets the shares.

## 7. Participation and the participation ledger

A contract cannot see who took part in consensus, so someone must tell it in a way that cannot be faked by a single party.

**[proposal] Attestation by the community nodes:**
1. **Periods.** Time is split into periods, for example one day, by blockchain time (v3 `getBlockchainTimeMills`) or by block sequence carried in the report.
2. **Report.** At the end of a period every community node computes the same participation report from chain data it already stores. The report contains, for each node wallet, the number of blocks in which it was a real trusted signer (`realTrusted` mask) and the CS reward it received. Computing it is deterministic: every honest node gets the same bytes, and the report's hash identifies it.
3. **Submit and confirm.** One node submits the report, split into chunks if needed to stay within executor limits. Other registered community nodes confirm the hash with a cheap call.
4. **Acceptance.** The contract accepts a report when K distinct registered node wallets have confirmed the same hash.
   - **Who may confirm** **[proposal]**: wallets listed as trusted in the previous accepted report, not in the report being confirmed. A report cannot vouch for its own confirmers, so a few fresh keys cannot certify themselves.
   - **The first period** **[owner: all community node wallets take part]**: every node wallet registered before the first period may confirm. This is the one moment a group running many fresh keys could gain an edge. K therefore also has a minimum absolute count, and the first period's report pays only after a delay during which any registered node may publish a conflicting report, which blocks it. K is a fraction of the active community nodes, for example a majority with a minimum count. Conflicting reports for the same period never pay; the period stays open until one reaches K.
5. **Registration.** A node wallet registers by calling the contract itself, so the initiator proves it holds the node key. Only nodes running the community software do this, through a node option (section 9). That is what "community node" means for CCS.
6. **What an accepted report drives:**
   - the participation reward;
   - the conversion allowance (section 6);
   - voting rights (section 10).

**Participation reward** **[owner: falls over time; equal base for every node, the full-node advantage comes from services]**:
- **Size:** each period releases a share of the emission stock by a fixed schedule, for example halving every year, so the reward per node shrinks over time and with the number of nodes.
- **Split:** by blocks signed in the period, the same for pruned and full nodes.
- **Payout** **[proposal]**: the contract does not loop over all nodes. Each node claims its own share with a call, which also bounds execution time. Unclaimed shares stay claimable for a long window, and afterwards return to the emission stock.

**Risk:** if most community nodes collude, they can certify a false report. The design accepts this risk, because they are also the beneficiaries and the counterparties of each other. The builder must document it, and make K high enough that one operator with a few nodes cannot reach it.

## 8. Registry and services

**Registering a service** **[owner: registering costs CCS]**:
- **Records:** a provider wallet, a category (e.g. `history.wallet`, `history.contract`, `search`), an endpoint (URL or p2p address), a description and a price factor.
- **Price** **[proposal]**: the contract sets a base price per category, which adjusts by rule (section 12). The provider chooses a factor within fixed bounds, for example 1 to 3 times the base price. The contract charges base × factor, so providers compete on price within the range and nobody prices below the base.
- **Cost:**
  - **[proposal]** a deposit in CCS, which the provider forfeits if it is proved to cheat;
  - a registration cost that rises with the number of services already registered in the same category;
  - a periodic fee to stay listed.
- **Unpaid services:** a service whose fee is not paid drops out after a grace period.

**Using a service:**
1. The client pays the contract in CCS for a call or a bundle of calls, naming the service.
2. The contract records a payment id.
3. The client presents that id to the provider off-chain, and the provider serves the request.
4. **Payment split** **[proposal]**: the provider gets a share, a share returns to the sale or emission stock, and a share goes to the participation reward pool, so the reward gradually moves from emission to service fees. The split adjusts by rule (section 12).

**The first services, for full nodes:**
- wallet history;
- contract history;
- search over history.

These run on a full community node next to the existing API, as a new endpoint of the node.

**Free and paid** **[owner]**: ordinary history stays free, as it is today from the node API and the monitor. Only the **certified** answer is paid in CCS: signed by the provider, with block range and hashes, and compared between two independent providers. Examples are the statement of a wallet's movements for a tax return, and an audit trail of a contract.

**Certification** **[proposal]**:
- **Signing.** The provider signs every answer with its node key, including the block range and the block hashes it covers.
- **Correctness.** Any client can check the answer's transactions against block hashes, through its own node or another provider.
- **Completeness.** That nothing is missing is checked by asking two independent providers and comparing.
- **Cheating.** A signed answer that contradicts the chain is a proof of cheating. Submitting it to the contract, along with confirmations from K community nodes as in section 7, forfeits the provider's deposit and removes it from the registry.

## 9. Node and wallet

**Node:** a community-node option, off by default. `setup.sh` and `setup.ps1` must offer it, and setup-test must cover it, as for every configurable feature.
- `[ccs] enabled`: registers the node wallet and takes part in reports.
- `reward_share = 0..100`: the share of the CS reward converted to CCS.
- **Conversion:** batched per period, so there is one transaction per period rather than one per block, signed with the node key.
- **Reports:** the node computes, submits and confirms participation reports.
- **Claims:** the node claims the participation reward.
- **Services:** on a full node, an option to register and serve the history services. The deposit and fee payments ask for the operator's explicit confirmation in the setup or the wallet, never silently.
- **Logs:** a local log of every CCS transaction the node sends, and the CS fees spent.

**Wallet:**
- shows CCS like other tokens (already supported);
- buy, sell and transfer;
- the conversion share of the node account;
- the participation rewards earned;
- the services listed in the registry;
- the first client of the history services: the free history as today, plus the certified statement paid in CCS.

## 10. Committee: pause, restore, replace

The parameters are automatic and nobody can change them. A committee exists only to stop or replace the contract after a serious error **[owner]**.

**Election** **[owner]**:
- **Voters:** only node wallets that were real trusted signers in an accepted report of the last 30 days **[proposal: 30 days]**.
- **Votes:** each voter gives one vote, to a wallet other than its own. Voting costs CCS, which go back to the sale stock. A vote can be moved, paying again. A vote stops counting when its voter has not been trusted for 30 days.
- **Who sits:** the top 5 by votes form the committee.
- **Candidates** **[owner]**: every community node wallet may be voted for.
- **Confirmation** **[owner]**: a wallet elected to a seat must confirm its nomination with a call. A seat counts only once confirmed. If fewer than 5 seats are confirmed, **the committee does not exist**: nobody can pause, restore or replace the contract. This is accepted.
- **Ties:** the wallet that reached the count first ranks higher.
- **Timing** **[proposal]**: the ranking is live, but committee changes take effect only at the start of each quarter. An action in progress keeps the approvals of the committee that started it.
- **Seats persist** **[proposal]**: a confirmed member keeps the seat until another wallet overtakes it in votes and confirms. Silence by the voters therefore never empties a seated committee.

**Powers:** three of the five must approve, each with an ordinary call from their own wallet.
- **Pause:** buy, transfer, registry and service payments stop at once. Selling back to CS stays open. The pause expires by itself after 14 days unless renewed.
- **Restore:** ends a pause.
- **Replace:**
  - the committee names the address of a new contract that is already deployed;
  - the new contract carries the balances and stocks of the old one, which anyone can verify through the API;
  - after a 14-day notice the old contract sends its CS reserve and stocks to that address and stops for good;
  - during the notice everyone can sell back and leave.

**What the committee cannot do:** change parameters, move funds anywhere other than a replacement announced 14 days earlier, assign CCS, or stop sales back to CS.

**How the balances move** **[proposal]**: the old contract cannot be read by the new one, so the replacement is deployed with the final balances of the old contract, frozen at the start of the notice. During the notice the old contract accepts only sales back to CS, and the new contract replays those sales before it opens.

## 11. Economic simulation (before any code)

A simulation chooses the numbers that cannot be changed later. It models over 5 years:
- community nodes, pruned and full, joining and leaving;
- conversion shares;
- ordinary purchases by apps;
- service demand;
- sales back to CS, including a run where everyone sells.

**It must show:**
- the reserve can always pay every seller;
- the participation reward per node over time;
- the CCS price path;
- the full-node income from services against the pruned-node income;
- how the parameter rules of section 12 behave: no oscillation, and no runaway at high or zero demand.

**It chooses:** N, the split between emission and sale, the curve, the fees, the emission schedule, the service split and the registry costs. It is a script in the repository, re-runnable. Its results go into an appendix of this document, which the owner approves before the contract is written.

## 12. Self-adjusting parameters **[owner: automatic, no intervention]**

Each rule uses only data the contract sees itself: its stocks, reserve, calls and time. Updates happen on the first call after a period ends.

| Parameter | Rule |
|---|---|
| CCS price | the curve over CCS out of the sale stock (section 6) |
| Participation reward per period | fixed falling schedule of the emission stock |
| Base price of a service call, per category | rises when last period's calls exceed a target, falls when below, bounded per period (e.g. ±12.5 %) |
| Registration cost, per category | rises with the number of services listed in it |
| Split of service payments | moves toward the stocks when they are low, toward providers when they are high, bounded per period |

The builder sets the targets and bounds from the simulation.

## 13. Testing and release

- **Contract tests:** unit tests of every rule, with property tests for reserve solvency and conservation of supply. Stocks plus all balances must always equal N.
- **Executor tests:** run against executor build 1518 and the community executor, comparing results with the harness in `ci/contracts`.
- **Private network:** official and community nodes together, with CCS enabled on the community nodes:
  - reports, claims, conversions, services and committee actions;
  - a node syncing the whole run from scratch must end with the same state digest.
- **Review:** a security review of the contract and of the node's signing paths.
- **Release order:**
  1. our own testnet first;
  2. the simulation appendix approved;
  3. a legal check;
  4. mainnet deployment.

## 14. Open points that are not technical

- **Legal: a check against EU MiCA before mainnet.** CCS is a token with a CS reserve, sold to the public and used for services, including certified data for tax purposes.
- **No emergency brake at launch.** Until 5 elected members have confirmed, the contract cannot be paused or replaced **[owner]**. The launch parameters and the security review must assume no rescue is possible in that period.

## 15. Left to the builder

- the curve or pool formula, and every number, from the simulation;
- report format, chunking and the value of K;
- service endpoint protocol and answer format;
- contract file layout and method names;
- how the node stores its CCS state;
- the wallet's screens.
