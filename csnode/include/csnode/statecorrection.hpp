#ifndef STATECORRECTION_HPP
#define STATECORRECTION_HPP

#include <cstdint>
#include <map>
#include <vector>

#include <csdb/amount.hpp>
#include <csnode/nodecore.hpp>
#include <lib/system/common.hpp>

namespace cs {

// A known, verified difference between the wallet state this node's lineage carries and the one
// the network majority computes. A correction is applied only at a block that proves it: the
// wallet is a trusted confidant of the block, the block's stored reward split (signed by the
// majority) does not match what this node's stakes give, and it matches exactly once the delta
// is applied. A node whose state is already right never applies it, and an applied correction
// cannot be applied twice.
struct KnownStateCorrection {
    cs::PublicKey wallet;
    csdb::Amount delta;      // added to balance_
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
std::vector<csdb::Amount> blockRewardSplit(const std::vector<csdb::Amount>& stakes,
                                           const cs::Bytes& realTrusted,
                                           const csdb::Amount& minedValue);

}  // namespace cs

#endif  // STATECORRECTION_HPP
