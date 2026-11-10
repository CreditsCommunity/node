#ifndef STATECORRECTION_HPP
#define STATECORRECTION_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include <csdb/amount.hpp>
#include <csnode/nodecore.hpp>
#include <lib/system/common.hpp>

namespace cs {

// The chain uuid of the Credits mainnet (BlockChain::uuid(), from the hash of block 1). The test
// network and private networks have other genesis blocks, so other uuids.
constexpr uint64_t kMainnetUuid = 11024959585341937636ULL;

// A known, verified difference between the wallet state this node's lineage carries and the one
// the network majority computes. A correction is applied only at a block that proves it: the
// proven wallet is a trusted confidant of the block, the block's stored reward split (signed by
// the majority) does not match what this node's stakes give, and it matches exactly once the delta
// is applied. The counterpart adjustments are applied in the same step; they cannot be proven by
// the reward split (the counterparts are not confidants), they come from the transaction that
// caused the difference. A node whose state is already right never applies the correction, and an
// applied correction cannot be applied twice (the proven wallet's split then matches).
struct KnownStateCorrection {
    struct Adjustment {
        cs::PublicKey wallet;
        csdb::Amount delta;   // added to balance_
    };

    uint64_t networkUuid;       // the chain the difference exists on (BlockChain::uuid())
    cs::Sequence fromSequence;  // the block that caused it: no block below can prove it
    cs::PublicKey wallet;       // the proven wallet
    csdb::Amount delta;         // added to its balance_
    std::vector<Adjustment> counterparts;
    const char* note;
};

const std::vector<KnownStateCorrection>& knownStateCorrections();

// The stake value SolverCore::setBlockReward gives a trusted confidant (solver/src/solvercore.cpp),
// reproduced with the same csdb::Amount operations, from the wallet's balance and the delegations
// it received (delegateSources_).
csdb::Amount confidantStake(const csdb::Amount& balance,
                            const std::map<cs::PublicKey, std::vector<cs::TimeMoney>>* delegateSources);

// The reward split SolverCore::setBlockReward writes for the given stakes (one per confidant,
// ignored for untrusted ones), the real trusted mask (255 = untrusted) and the mined value.
// stakingOn mirrors Consensus::stakingOn: when it is off every share is 0 but the last trusted
// confidant's, which takes the whole mined value. Below Consensus::StartingDPOS the solver mines 0,
// so the stored split sums to 0 and the caller passes minedValue 0.
std::vector<csdb::Amount> blockRewardSplit(const std::vector<csdb::Amount>& stakes,
                                           const cs::Bytes& realTrusted,
                                           const csdb::Amount& minedValue,
                                           bool stakingOn = true);

// What a block says about the known corrections, given this node's wallet state.
struct StateCorrectionBlock {
    uint64_t networkUuid = 0;
    cs::Sequence sequence = 0;
    bool stakingOn = true;
    const cs::ConfidantsKeys* confidants = nullptr;
    const cs::Bytes* realTrusted = nullptr;                // bitsToMask of the block (255 = untrusted)
    const std::vector<csdb::Amount>* storedReward = nullptr;  // the block's reward split
};

struct StateCorrectionWallet {
    csdb::Amount balance;
    const std::map<cs::PublicKey, std::vector<cs::TimeMoney>>* delegateSources = nullptr;
};

using StateCorrectionLookup = std::function<StateCorrectionWallet(const cs::PublicKey&)>;

enum class StateCorrectionVerdict {
    None,               // the block proves nothing (no entry applies, or the state already matches)
    Apply,              // apply the entry: the proven wallet and all its counterparts
    Unexplained,        // the split differs and the entry's delta does not explain it
    CounterpartTooLow   // proven, but a counterpart's balance would become negative: apply nothing
};

struct StateCorrectionDecision {
    StateCorrectionVerdict verdict = StateCorrectionVerdict::None;
    size_t entry = 0;   // index in the table, for every verdict but None
};

// The decision WalletsCache::Updater::applyKnownStateCorrections takes before a block's own
// changes, with the wallet state read through lookup. At most one entry per block.
StateCorrectionDecision decideKnownStateCorrection(const std::vector<KnownStateCorrection>& table,
                                                   const StateCorrectionBlock& block,
                                                   const StateCorrectionLookup& lookup);

}  // namespace cs

#endif  // STATECORRECTION_HPP
