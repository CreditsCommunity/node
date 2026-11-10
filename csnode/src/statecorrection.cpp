#include <csnode/statecorrection.hpp>

#include <algorithm>

#include <csnode/blockchain.hpp>
#include <solver/consensus.hpp>

namespace cs {

namespace {
constexpr uint8_t kUntrustedMarker = 255;

// 7hWM6wvsojdPy3A2pQjqWKtfJM4hQRCWe3YC2hB2WzbE
constexpr cs::PublicKey kWallet7hWM = {
    0x63, 0x86, 0x89, 0x45, 0xde, 0x6a, 0x10, 0x02, 0xea, 0x7c, 0xbc, 0xa5, 0x0b, 0xbe, 0x46, 0x13,
    0xbc, 0xa6, 0xa6, 0x56, 0x38, 0xcc, 0x56, 0xcd, 0x8c, 0xba, 0x98, 0xed, 0xdb, 0x08, 0xa6, 0x1d
};

}  // namespace

const std::vector<KnownStateCorrection>& knownStateCorrections() {
    // Mainnet, 2026-06-05: the payout 176760929.2 of the dice contract (block 176,760,929) pays
    // MooNRor8... the win of its call 176760925.1. The lineage of the published pruned snapshots
    // replayed that contract state with a stale ref_execute (the previous successful execution, a
    // call by 7hWM6wvs...), and charged the payout's contract-state fee (0.008740234375 CS) to
    // 7hWM6wvs... (see docs/differences.md, "Known state corrections"). Every block of November
    // 2026 where 7hWM6wvs... is a confidant reproduces the stored reward split bit for bit with +1
    // fee on it and with no other value (checked over 40,000 blocks around 187,722,000).
    // No counterpart: MooNRor8... paid the emitted fee with its call on every lineage, and the
    // nodes of the majority report the same MooNRor8... balance as this lineage (public API of 8
    // majority and 3 minority nodes, block 187,731,527, 2026-11-10), while 7hWM6wvs... differs by
    // exactly one fee. Adjusting MooNRor8... would create a difference, not remove one.
    static const csdb::Amount kFee(0, 8740234375000000ULL);  // 0.008740234375 CS
    static const std::vector<KnownStateCorrection> corrections = {
        { kMainnetUuid, 176'760'929, kWallet7hWM, kFee, {},
          "mainnet 7hWM6wvs: fee of dice payout 176760929.2 charged to it while syncing" }
    };
    return corrections;
}

csdb::Amount confidantStake(const csdb::Amount& balance,
                            const std::map<cs::PublicKey, std::vector<cs::TimeMoney>>* delegateSources) {
    // Mirrors SolverCore::setBlockReward (solver/src/solvercore.cpp): keep the same operations and
    // the same operand types, the csdb::Amount conversions decide the result.
    csdb::Amount nodeConfidantAndStake;
    csdb::Amount nodeConfidantAndFreezenStake;
    csdb::Amount totalNodeStake = 0;
    totalNodeStake += balance;
    nodeConfidantAndStake += balance;
    if (delegateSources != nullptr && delegateSources->size() > 0) {
        for (auto& keyAndStake : *delegateSources) {
            for (auto& tm : keyAndStake.second) {
                if (tm.coeff == StakingCoefficient::NoStaking) {
                    nodeConfidantAndStake += tm.amount;
                }
                else {
                    nodeConfidantAndFreezenStake += tm.amount * BlockChain::getStakingCoefficient(tm.coeff);
                }
            }
        }
    }
    totalNodeStake = nodeConfidantAndStake + nodeConfidantAndFreezenStake;

    csdb::Amount totaNodeCutStake = totalNodeStake;
    if (totaNodeCutStake > Consensus::MaxStakeValue) {
        totaNodeCutStake = Consensus::MaxStakeValue;
    }

    return (nodeConfidantAndStake * BlockChain::getStakingCoefficient(StakingCoefficient::NoStaking) + nodeConfidantAndFreezenStake) *
           (totalNodeStake > csdb::Amount{ 1 } ? totaNodeCutStake / totalNodeStake : csdb::Amount{ 1 });
}

std::vector<csdb::Amount> blockRewardSplit(const std::vector<csdb::Amount>& stakes,
                                           const cs::Bytes& realTrusted,
                                           const csdb::Amount& minedValue,
                                           bool stakingOn) {
    std::vector<csdb::Amount> result;
    const size_t n = std::min(stakes.size(), realTrusted.size());
    csdb::Amount totalStake = 0;
    int32_t realTrustedNumber = 0;
    for (size_t i = 0; i < n; ++i) {
        if (realTrusted[i] == kUntrustedMarker) {
            continue;
        }
        ++realTrustedNumber;
        totalStake += stakes[i];
    }
    if (totalStake < csdb::Amount{ 1 }) {
        totalStake = csdb::Amount{ 1 };
    }
    csdb::Amount oneMiningPart = stakingOn ? minedValue / totalStake : csdb::Amount{ 0 };
    csdb::Amount payedReward = 0;
    int32_t numPayedTrusted = 0;
    for (size_t i = 0; i < n; ++i) {
        csdb::Amount rewardToPay = 0;
        if (realTrusted[i] != kUntrustedMarker) {
            if (numPayedTrusted == realTrustedNumber - 1) {
                rewardToPay = minedValue - payedReward;
            }
            else {
                rewardToPay = oneMiningPart * stakes[i];
            }
            ++numPayedTrusted;
        }
        if (rewardToPay > minedValue || rewardToPay < csdb::Amount{ 0 }) {
            rewardToPay = csdb::Amount{ 0 };
        }
        result.push_back(rewardToPay);
        payedReward += rewardToPay;
    }
    return result;
}

StateCorrectionDecision decideKnownStateCorrection(const std::vector<KnownStateCorrection>& table,
                                                   const StateCorrectionBlock& block,
                                                   const StateCorrectionLookup& lookup) {
    StateCorrectionDecision none;
    if (table.empty() || block.confidants == nullptr || block.realTrusted == nullptr || block.storedReward == nullptr) {
        return none;
    }
    // with staking off the split does not depend on the stakes, so it proves nothing
    if (!block.stakingOn) {
        return none;
    }
    const auto& confidants = *block.confidants;
    const auto& realTrusted = *block.realTrusted;
    const auto& rewards = *block.storedReward;

    bool prepared = false;
    std::vector<csdb::Amount> stakes;
    StateCorrectionDecision failed;  // the first entry that is not applied, if no other one is
    csdb::Amount mined = 0;
    for (size_t e = 0; e < table.size(); ++e) {
        const auto& c = table[e];
        if (block.networkUuid != c.networkUuid || block.sequence < c.fromSequence) {
            continue;
        }
        auto it = std::find(confidants.begin(), confidants.end(), c.wallet);
        if (it == confidants.end()) {
            continue;
        }
        const size_t idx = static_cast<size_t>(it - confidants.begin());
        if (!prepared) {
            prepared = true;
            if (confidants.empty() || realTrusted.size() != confidants.size() || rewards.size() != confidants.size()) {
                return none;
            }
            for (const auto& r : rewards) {
                mined += r;
            }
            if (mined <= csdb::Amount{ 0 }) {
                return none;  // below StartingDPOS, or nothing mined: the split proves nothing
            }
            for (size_t i = 0; i < confidants.size(); ++i) {
                if (realTrusted[i] == kUntrustedMarker) {
                    stakes.emplace_back(0);
                    continue;
                }
                auto w = lookup(confidants[i]);
                stakes.push_back(confidantStake(w.balance, w.delegateSources));
            }
            // the stored split is the one the writer computed from the majority's stakes
            if (blockRewardSplit(stakes, realTrusted, mined, block.stakingOn) == rewards) {
                return none;  // this node's stakes give the stored split: nothing to correct
            }
        }
        if (realTrusted[idx] == kUntrustedMarker) {
            continue;
        }
        auto w = lookup(c.wallet);
        std::vector<csdb::Amount> corrected = stakes;
        corrected[idx] = confidantStake(w.balance + c.delta, w.delegateSources);
        if (blockRewardSplit(corrected, realTrusted, mined, block.stakingOn) != rewards) {
            if (failed.verdict == StateCorrectionVerdict::None) {
                failed = { StateCorrectionVerdict::Unexplained, e };
            }
            continue;
        }
        bool counterpartsCover = true;
        for (const auto& adj : c.counterparts) {
            // += as the caller applies it (Amount::operator+ does not carry a fraction of exactly 1)
            csdb::Amount after = lookup(adj.wallet).balance;
            after += adj.delta;
            if (after < csdb::Amount{ 0 }) {
                counterpartsCover = false;
                break;
            }
        }
        if (!counterpartsCover) {
            if (failed.verdict == StateCorrectionVerdict::None) {
                failed = { StateCorrectionVerdict::CounterpartTooLow, e };
            }
            continue;
        }
        return { StateCorrectionVerdict::Apply, e };
    }
    return failed;
}

}  // namespace cs
