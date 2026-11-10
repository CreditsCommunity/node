// Known state corrections: the reward split of SolverCore::setBlockReward reproduced from the
// wallet state, checked against blocks the mainnet stored (November 2026).
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <base58.h>
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

const char* const kMooNRor8 = "MooNRor8TcLT3xpAw3UvA2Q6xT8huRoQojn6ZSveDpP";
const csdb::Amount kMooNRor8Balance(900.8214765625476);  // as this lineage holds it (2026-11-10)
constexpr uint64_t kTestnetUuid = 5283967947175248524ULL;

cs::PublicKey keyOf(const char* base58) {
    std::vector<uint8_t> bytes;
    DecodeBase58(base58, bytes);
    cs::PublicKey key{};
    if (bytes.size() == key.size()) {
        std::copy(bytes.begin(), bytes.end(), key.begin());
    }
    return key;
}

// The wallet state WalletsCache::Updater::applyKnownStateCorrections reads, injected: the
// confidants of a fixture plus the counterpart.
struct State {
    std::map<cs::PublicKey, csdb::Amount> balance;
    std::map<cs::PublicKey, std::map<cs::PublicKey, std::vector<cs::TimeMoney>>> sources;

    explicit State(const Fixture& f) {
        for (const auto& c : f.confidants) {
            balance[c.key] = c.balanceBefore;
            sources[c.key] = sourcesOf(c);
        }
        balance[keyOf(kMooNRor8)] = kMooNRor8Balance;
    }

    cs::StateCorrectionLookup lookup() const {
        return [this](const cs::PublicKey& key) {
            cs::StateCorrectionWallet w;
            if (auto it = balance.find(key); it != balance.end()) {
                w.balance = it->second;
            }
            if (auto it = sources.find(key); it != sources.end()) {
                w.delegateSources = &it->second;
            }
            return w;
        };
    }

    // what applyKnownStateCorrections does on an Apply verdict
    void apply(const cs::KnownStateCorrection& c) {
        balance[c.wallet] += c.delta;
        for (const auto& adj : c.counterparts) {
            balance[adj.wallet] += adj.delta;
        }
    }
};

struct Block {
    cs::ConfidantsKeys confidants;
    cs::Bytes realTrusted;
    std::vector<csdb::Amount> stored;
    cs::StateCorrectionBlock view;

    Block(const Fixture& f, cs::Sequence sequence, uint64_t uuid = cs::kMainnetUuid, bool stakingOn = true)
    : realTrusted(maskOf(f)), stored(f.storedReward) {
        for (const auto& c : f.confidants) {
            confidants.push_back(c.key);
        }
        view.networkUuid = uuid;
        view.sequence = sequence;
        view.stakingOn = stakingOn;
        view.confidants = &confidants;
        view.realTrusted = &realTrusted;
        view.storedReward = &stored;
    }
    Block(const Block&) = delete;
};

// the mainnet entry with a counterpart (MooNRor8... -1 fee), to exercise paired adjustments
const std::vector<cs::KnownStateCorrection>& pairedTable() {
    static const std::vector<cs::KnownStateCorrection> table = [] {
        auto t = cs::knownStateCorrections();
        t[0].counterparts.push_back({ keyOf(kMooNRor8), csdb::Amount(0) - kOneContractStateFee });
        return t;
    }();
    return table;
}

cs::StateCorrectionDecision decide(const Block& b, const State& s) {
    return cs::decideKnownStateCorrection(pairedTable(), b.view, s.lookup());
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
    EXPECT_EQ(table[0].networkUuid, cs::kMainnetUuid);
    EXPECT_EQ(table[0].fromSequence, 176'760'929ULL);
    // MooNRor8... holds the same balance on the majority's nodes: no counterpart
    EXPECT_TRUE(table[0].counterparts.empty());
    bool found = false;
    for (const auto& c : kWith7hWM.confidants) {
        if (std::string(c.base58) == k7hWM) {
            found = (c.key == table[0].wallet);
        }
    }
    EXPECT_TRUE(found);
}

// The decision WalletsCache::Updater::applyKnownStateCorrections takes, with the wallet state injected
// and the mainnet entry given a counterpart (pairedTable)
// (a WalletsCache needs a BlockChain and its databases; the adapter only reads the wallets, calls
// decideKnownStateCorrection and applies the entry on Apply, as State::apply does here).
using cs::StateCorrectionVerdict;

TEST(StateCorrectionTrigger, FiresOnceAndAppliesBothAdjustments) {
    State s(kWith7hWM);
    const auto k7 = keyOf(k7hWM);
    const auto kMoon = keyOf(kMooNRor8);
    const csdb::Amount before7 = s.balance[k7];
    Block b(kWith7hWM, kWith7hWM.sequence);

    const auto d = decide(b, s);
    ASSERT_EQ(d.verdict, StateCorrectionVerdict::Apply);
    ASSERT_EQ(d.entry, 0U);
    s.apply(pairedTable()[d.entry]);
    EXPECT_TRUE(s.balance[k7] == before7 + kOneContractStateFee);
    EXPECT_TRUE(s.balance[kMoon] + kOneContractStateFee == kMooNRor8Balance);
    // the reward split this node now computes is the stored one
    EXPECT_TRUE(splitOf(kWith7hWM, k7hWM, kOneContractStateFee) == kWith7hWM.storedReward);
}

TEST(StateCorrectionTrigger, DoesNotFireAgainOnTheNextProvingBlock) {
    State s(kWith7hWM);
    Block b(kWith7hWM, kWith7hWM.sequence);
    ASSERT_EQ(decide(b, s).verdict, StateCorrectionVerdict::Apply);
    s.apply(pairedTable()[0]);
    const csdb::Amount moon = s.balance[keyOf(kMooNRor8)];

    Block next(kWith7hWM, kWith7hWM.sequence + 2);
    EXPECT_EQ(decide(next, s).verdict, StateCorrectionVerdict::None);
    EXPECT_EQ(decide(b, s).verdict, StateCorrectionVerdict::None);
    EXPECT_TRUE(s.balance[keyOf(kMooNRor8)] == moon);
}

TEST(StateCorrectionTrigger, DoesNotFireOnACorrectState) {
    // a node of the majority: 7hWM6wvs... already holds the fee, MooNRor8... does not
    State s(kWith7hWM);
    s.balance[keyOf(k7hWM)] += kOneContractStateFee;
    s.balance[keyOf(kMooNRor8)] -= kOneContractStateFee;
    Block b(kWith7hWM, kWith7hWM.sequence);
    EXPECT_EQ(decide(b, s).verdict, StateCorrectionVerdict::None);
}

TEST(StateCorrectionTrigger, BlockWithoutTheWalletProvesNothing) {
    State s(kWithout7hWM);
    Block b(kWithout7hWM, kWithout7hWM.sequence);
    EXPECT_EQ(decide(b, s).verdict, StateCorrectionVerdict::None);
}

TEST(StateCorrectionTrigger, DoesNotFireBelowFromSequence) {
    State s(kWith7hWM);
    Block below(kWith7hWM, 176'760'928);
    EXPECT_EQ(decide(below, s).verdict, StateCorrectionVerdict::None);
    Block at(kWith7hWM, 176'760'929);
    EXPECT_EQ(decide(at, s).verdict, StateCorrectionVerdict::Apply);
}

TEST(StateCorrectionTrigger, MainnetOnly) {
    State s(kWith7hWM);
    Block testnet(kWith7hWM, kWith7hWM.sequence, kTestnetUuid);
    EXPECT_EQ(decide(testnet, s).verdict, StateCorrectionVerdict::None);
    Block privateNet(kWith7hWM, kWith7hWM.sequence, 1234567ULL);
    EXPECT_EQ(decide(privateNet, s).verdict, StateCorrectionVerdict::None);
    Block unknown(kWith7hWM, kWith7hWM.sequence, 0);
    EXPECT_EQ(decide(unknown, s).verdict, StateCorrectionVerdict::None);
}

TEST(StateCorrectionTrigger, StakingOffProvesNothing) {
    State s(kWith7hWM);
    Block b(kWith7hWM, kWith7hWM.sequence, cs::kMainnetUuid, false);
    EXPECT_EQ(decide(b, s).verdict, StateCorrectionVerdict::None);
}

TEST(StateCorrectionTrigger, CounterpartBalanceGuard) {
    State s(kWith7hWM);
    s.balance[keyOf(kMooNRor8)] = csdb::Amount(0, 8740234374999999ULL);  // one unit below the fee
    Block b(kWith7hWM, kWith7hWM.sequence);
    const auto d = decide(b, s);
    EXPECT_EQ(d.verdict, StateCorrectionVerdict::CounterpartTooLow);
    EXPECT_EQ(d.entry, 0U);

    s.balance[keyOf(kMooNRor8)] = csdb::Amount(0);
    EXPECT_EQ(decide(b, s).verdict, StateCorrectionVerdict::CounterpartTooLow);

    // exactly the fee: the counterpart ends at 0, never below
    s.balance[keyOf(kMooNRor8)] = kOneContractStateFee;
    ASSERT_EQ(decide(b, s).verdict, StateCorrectionVerdict::Apply);
    s.apply(pairedTable()[0]);
    EXPECT_TRUE(s.balance[keyOf(kMooNRor8)] == csdb::Amount(0));
}

TEST(StateCorrectionTrigger, UnexplainedDifferenceIsOnlyReported) {
    State s(kWith7hWM);
    s.balance[keyOf(k7hWM)] -= kOneContractStateFee;  // two fees short: the entry does not explain it
    Block b(kWith7hWM, kWith7hWM.sequence);
    const auto d = decide(b, s);
    EXPECT_EQ(d.verdict, StateCorrectionVerdict::Unexplained);
    EXPECT_EQ(d.entry, 0U);
}

TEST(StateCorrection, StakingOffSplitGivesAllToTheLastTrusted) {
    const auto& f = kWith7hWM;
    std::vector<csdb::Amount> stakes(f.confidants.size(), csdb::Amount(1000));
    const auto split = cs::blockRewardSplit(stakes, maskOf(f), csdb::Amount(1), false);
    ASSERT_EQ(split.size(), f.confidants.size());
    for (size_t i = 0; i + 1 < split.size(); ++i) {
        EXPECT_TRUE(split[i] == csdb::Amount(0));
    }
    EXPECT_TRUE(split.back() == csdb::Amount(1));
}

TEST(StateCorrectionTrigger, MainnetTableAppliesOnlyTheProvenWallet) {
    State s(kWith7hWM);
    Block b(kWith7hWM, kWith7hWM.sequence);
    const auto d = cs::decideKnownStateCorrection(cs::knownStateCorrections(), b.view, s.lookup());
    ASSERT_EQ(d.verdict, StateCorrectionVerdict::Apply);
    s.apply(cs::knownStateCorrections()[d.entry]);
    EXPECT_TRUE(s.balance[keyOf(kMooNRor8)] == kMooNRor8Balance);
    EXPECT_EQ(cs::decideKnownStateCorrection(cs::knownStateCorrections(), b.view, s.lookup()).verdict, StateCorrectionVerdict::None);
}
