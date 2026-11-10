#include <csnode/statecorrection.hpp>

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
    // Mainnet, October 2026: the lineage of the published pruned snapshots synced the dice-contract
    // blocks around 176,761,291 with the official replay rule, which charged one contract-state fee
    // (0.008740234375 CS) of a later payout to 7hWM6wvs... (see docs/differences.md, "Fees of
    // contract payouts"). The majority that executed the contract did not. Every block of November
    // 2026 where 7hWM6wvs... is a confidant reproduces the stored reward split bit for bit with
    // this delta and with no other value (checked over 40,000 blocks around 187,722,000).
    static const std::vector<KnownStateCorrection> corrections = {
        { kWallet7hWM, csdb::Amount(0, 8740234375000000ULL), "mainnet 7hWM6wvs: contract-state fee charged while syncing (block 176,761,291 area)" }
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
                                           const csdb::Amount& minedValue) {
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
    csdb::Amount oneMiningPart = minedValue / totalStake;
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

}  // namespace cs
