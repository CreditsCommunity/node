// Known state corrections: the reward split of SolverCore::setBlockReward reproduced from the
// wallet state, checked against blocks the mainnet stored (November 2026).
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include <csnode/statecorrection.hpp>

namespace {

struct Donor {
    csdb::Amount sum;
    int coeff;
};

struct Confidant {
    const char* base58;
    cs::PublicKey key;
    csdb::Amount balanceBefore;
    std::vector<Donor> donors;
};

struct Fixture {
    uint64_t sequence;
    uint8_t numberTrusted;
    uint64_t realTrusted;
    std::vector<Confidant> confidants;
    std::vector<csdb::Amount> storedReward;
};

#include "statecorrection_fixtures.inc"

const char* const k7hWM = "7hWM6wvsojdPy3A2pQjqWKtfJM4hQRCWe3YC2hB2WzbE";
const csdb::Amount kOneContractStateFee(0, 8740234375000000ULL);  // 0.008740234375 CS

cs::Bytes maskOf(const Fixture& f) {
    cs::Bytes mask;
    for (uint8_t i = 0; i < f.numberTrusted; ++i) {
        mask.push_back(((f.realTrusted >> i) & 1U) ? 0 : 255);
    }
    return mask;
}

std::map<cs::PublicKey, std::vector<cs::TimeMoney>> sourcesOf(const Confidant& c) {
    std::map<cs::PublicKey, std::vector<cs::TimeMoney>> res;
    cs::PublicKey donor{};
    for (const auto& d : c.donors) {
        ++donor[0];
        cs::TimeMoney tm(0, 0, d.sum);
        tm.coeff = static_cast<cs::StakingCoefficient>(d.coeff);
        res[donor].push_back(tm);
    }
    return res;
}

std::vector<csdb::Amount> splitOf(const Fixture& f, const std::string& adjustKey, const csdb::Amount& delta) {
    std::vector<csdb::Amount> stakes;
    csdb::Amount mined = 0;
    for (const auto& r : f.storedReward) {
        mined += r;
    }
    for (const auto& c : f.confidants) {
        auto sources = sourcesOf(c);
        csdb::Amount balance = c.balanceBefore;
        if (adjustKey == c.base58) {
            balance += delta;
        }
        stakes.push_back(cs::confidantStake(balance, &sources));
    }
    return cs::blockRewardSplit(stakes, maskOf(f), mined);
}

}  // namespace

TEST(StateCorrection, AmountOfTheFee) {
    EXPECT_TRUE(kOneContractStateFee == csdb::Amount(0.008740234375));
}

TEST(StateCorrection, BlockWithoutTheWalletIsReproducedExactly) {
    EXPECT_TRUE(splitOf(kWithout7hWM, "", csdb::Amount(0)) == kWithout7hWM.storedReward);
}

TEST(StateCorrection, BlockWithTheWalletNeedsExactlyOneFee) {
    const auto& f = kWith7hWM;
    const auto asHeld = splitOf(f, "", csdb::Amount(0));
    EXPECT_TRUE(asHeld != f.storedReward);
    // only the wallet's share and the remainder of the last paid confidant differ
    size_t differing = 0;
    for (size_t i = 0; i < asHeld.size(); ++i) {
        if (asHeld[i] != f.storedReward[i]) {
            ++differing;
        }
    }
    EXPECT_EQ(differing, 2U);

    EXPECT_TRUE(splitOf(f, k7hWM, kOneContractStateFee) == f.storedReward);
    EXPECT_TRUE(splitOf(f, k7hWM, kOneContractStateFee * 2) != f.storedReward);
    EXPECT_TRUE(splitOf(f, k7hWM, csdb::Amount(0, 1000000000000000ULL)) != f.storedReward);  // 0.001 CS
}

TEST(StateCorrection, CorrectionTable) {
    const auto& table = cs::knownStateCorrections();
    ASSERT_EQ(table.size(), 1U);
    EXPECT_TRUE(table[0].delta == kOneContractStateFee);
    bool found = false;
    for (const auto& c : kWith7hWM.confidants) {
        if (std::string(c.base58) == k7hWM) {
            found = (c.key == table[0].wallet);
        }
    }
    EXPECT_TRUE(found);
}
