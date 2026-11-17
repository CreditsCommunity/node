#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <base58.h>
#include <config.hpp>
#include <csnode/communitybootstrap.hpp>

using cs::CommunityBootstrapGate;
using cs::communityTableWeight;
using cs::isCommunityFallbackOpen;
using cs::isCommunityTable;

namespace {
// key k: every byte k, so that keys sort as their numbers
cs::PublicKey key(uint8_t k) {
    cs::PublicKey result{};
    result.fill(k);
    return result;
}

std::set<cs::PublicKey> keys(std::initializer_list<int> ks) {
    std::set<cs::PublicKey> result;
    for (int k : ks) {
        result.insert(key(uint8_t(k)));
    }
    return result;
}

std::vector<cs::PublicKey> table(std::initializer_list<int> ks) {
    std::vector<cs::PublicKey> result;
    for (int k : ks) {
        result.push_back(key(uint8_t(k)));
    }
    return result;
}

// the CI layout: initial confidants 1-5 (2 and 4 official), community nodes 1, 3, 5, 7, 9
const std::set<cs::PublicKey> kInitial = keys({1, 2, 3, 4, 5});
const std::set<cs::PublicKey> kCommunity = keys({1, 3, 5, 7, 9});

constexpr uint64_t kMinute = 60'000;
constexpr uint64_t kStall = 30 * kMinute;
constexpr uint64_t kElapse = 20'000;
}  // namespace

TEST(CommunityBootstrap, TheCommunityNodesLeftAfterTheOfficialOnesMakeATable) {
    EXPECT_TRUE(isCommunityTable(kInitial, kCommunity, table({1, 3, 5, 7, 9})));
    EXPECT_TRUE(isCommunityTable(kInitial, kCommunity, table({1, 3, 7, 9})));
    // with initial confidants that are still there
    EXPECT_TRUE(isCommunityTable(kInitial, kCommunity, table({1, 2, 3, 7, 9})));
}

TEST(CommunityBootstrap, ATableOfInitialConfidantsOnlyIsARegularOne) {
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 2, 3, 4, 5})));
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 3, 4, 5})));
}

TEST(CommunityBootstrap, NoListNoTable) {
    EXPECT_FALSE(isCommunityTable(kInitial, {}, table({1, 3, 5, 7, 9})));
}

TEST(CommunityBootstrap, EveryKeyMustBeInitialOrListed) {
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 3, 5, 7, 8})));
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 3, 5, 7, 9, 10})));
}

TEST(CommunityBootstrap, MoreThanHalfOfTheListMustBeInIt) {
    // 3 of 5 listed: a majority
    EXPECT_TRUE(isCommunityTable(kInitial, kCommunity, table({1, 2, 4, 7, 9})));
    // 2 of 5 listed: another part of a split community could hold the other 3
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({2, 3, 4, 7})));
    const auto six = keys({1, 3, 5, 7, 9, 11});
    EXPECT_FALSE(isCommunityTable(kInitial, six, table({2, 4, 7, 9, 11})));  // 3 of 6
    EXPECT_TRUE(isCommunityTable(kInitial, six, table({4, 5, 7, 9, 11})));   // 4 of 6
}

TEST(CommunityBootstrap, SizeAndOrderAsAStarterSendsIt) {
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 7, 9})));      // below MinTrustedNodes
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 3, 9, 7})));   // not ascending
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 3, 7, 7, 9}))); // a repeated key
    std::set<cs::PublicKey> many;
    std::vector<cs::PublicKey> big;
    for (int k = 1; k <= 26; ++k) {
        many.insert(key(uint8_t(k)));
        big.push_back(key(uint8_t(k)));
    }
    EXPECT_FALSE(isCommunityTable(kInitial, many, big));  // above MaxTrustedNodes
    big.pop_back();
    EXPECT_TRUE(isCommunityTable(kInitial, many, big));
}

TEST(CommunityBootstrap, MoreKeysWeighMoreThenEarlierKeys) {
    const auto w5 = communityTableWeight(kInitial, kCommunity, table({1, 3, 5, 7, 9}));
    const auto w4 = communityTableWeight(kInitial, kCommunity, table({1, 3, 5, 7}));
    const auto w4late = communityTableWeight(kInitial, kCommunity, table({3, 5, 7, 9}));
    EXPECT_GT(w5, w4);
    EXPECT_GT(w4, w4late);
    // a table of fewer keys never outweighs a larger one, whatever its keys
    EXPECT_GT(communityTableWeight(kInitial, kCommunity, table({5, 7, 9, 3, 1})), communityTableWeight(kInitial, kCommunity, table({1, 2, 3, 4})));
    EXPECT_EQ(w5, communityTableWeight(kInitial, kCommunity, table({1, 3, 5, 7, 9})));
}

TEST(CommunityBootstrap, ClosedUntilTheNodeStalledForStallMinutes) {
    CommunityBootstrapGate gate;
    gate.onLastSequence(500, 0);
    EXPECT_FALSE(gate.isStalled(0, kStall));
    EXPECT_FALSE(gate.isStalled(kStall - 1, kStall));
    EXPECT_TRUE(gate.isStalled(kStall, kStall));
    EXPECT_TRUE(gate.isStalled(kStall + kMinute, kStall));
    // stall_minutes 0 is never stalled (the config allows 1..10080)
    EXPECT_FALSE(gate.isStalled(kStall, 0));
}

TEST(CommunityBootstrap, ANodeJustStartedHasNotWatchedLongEnough) {
    // the clock is ms since the node started: no progress yet counts as progress at the start
    CommunityBootstrapGate gate;
    EXPECT_FALSE(gate.isStalled(kStall - 1, kStall));
    gate.onLastSequence(500, kStall - 1);  // the first look at the chain is progress too
    EXPECT_FALSE(gate.isStalled(2 * kStall - 2, kStall));
    EXPECT_TRUE(gate.isStalled(2 * kStall - 1, kStall));
}

TEST(CommunityBootstrap, ABlockARoundOrARegularBootstrapClosesIt) {
    CommunityBootstrapGate gate;
    gate.onLastSequence(500, 0);
    ASSERT_TRUE(gate.isStalled(kStall, kStall));
    gate.onProgress(kStall);  // a round package, a regular table
    EXPECT_FALSE(gate.isStalled(kStall + 1, kStall));
    ASSERT_TRUE(gate.isStalled(2 * kStall, kStall));
    gate.onLastSequence(500, 2 * kStall);  // the same last block: no progress
    EXPECT_TRUE(gate.isStalled(2 * kStall, kStall));
    gate.onLastSequence(501, 2 * kStall);  // a block (synced or written)
    EXPECT_FALSE(gate.isStalled(2 * kStall + 1, kStall));
}

// The safety property: while blocks are produced (official nodes alive), the fallback never opens.
// A day of rounds, one a second or one every round_elapse_time at worst, a block each
TEST(CommunityBootstrap, NeverOpenWhileTheNetworkProducesBlocks) {
    for (uint64_t gap : {uint64_t(1000), kElapse, 10 * kMinute, kStall - 1}) {
        CommunityBootstrapGate gate;
        cs::Sequence seq = 100;
        for (uint64_t now = 0; now < 24 * 60 * kMinute; now += gap) {
            gate.onLastSequence(++seq, now);
            for (uint64_t t = now; t < now + gap; t += std::max<uint64_t>(gap / 4, 1)) {
                ASSERT_FALSE(isCommunityFallbackOpen(true, gate.isStalled(t, kStall), false)) << "gap " << gap << " at " << t;
            }
        }
    }
    // bootstrap rounds that end in blocks are progress just as well (a regular table every round_elapse_time)
    CommunityBootstrapGate gate;
    gate.onLastSequence(100, 0);
    for (uint64_t now = 0; now < 4 * kStall; now += kElapse) {
        gate.onProgress(now);
        ASSERT_FALSE(gate.isStalled(now + kElapse - 1, kStall));
    }
}

TEST(CommunityBootstrap, OpenOnlyWhenConfiguredStalledAndNoNeighbourAhead) {
    EXPECT_TRUE(isCommunityFallbackOpen(true, true, false));
    EXPECT_FALSE(isCommunityFallbackOpen(false, true, false));  // off by default: no list
    EXPECT_FALSE(isCommunityFallbackOpen(true, false, false));
    EXPECT_FALSE(isCommunityFallbackOpen(true, true, true));    // behind a neighbour: sync instead
}

TEST(CommunityBootstrap, OneTablePerHalfRoundElapseUnlessHeavier) {
    CommunityBootstrapGate gate;
    gate.onLastSequence(500, 0);
    const auto w5 = communityTableWeight(kInitial, kCommunity, table({1, 3, 5, 7, 9}));
    const auto w4 = communityTableWeight(kInitial, kCommunity, table({1, 3, 5, 7}));
    const uint64_t t = kStall;
    EXPECT_TRUE(gate.mayTake(w4, t, kElapse));
    gate.onTaken(w4, t);
    // a repeat or a lighter table right after: dropped, so a listed key cannot force restarts
    EXPECT_FALSE(gate.mayTake(w4, t + 1, kElapse));
    EXPECT_FALSE(gate.mayTake(w4 - 1, t + kElapse / 2 - 1, kElapse));
    // a heavier one at once: the table of more nodes wins
    EXPECT_TRUE(gate.mayTake(w5, t + 1, kElapse));
    gate.onTaken(w5, t + 1);
    EXPECT_FALSE(gate.mayTake(w4, t + 2, kElapse));
    // the starter's repeat, a round_elapse_time later
    EXPECT_TRUE(gate.mayTake(w5, t + 1 + kElapse, kElapse));
    EXPECT_TRUE(gate.mayTake(w4, t + 1 + kElapse / 2, kElapse));
    // progress forgets the tables of this stall
    gate.onProgress(t + 3);
    EXPECT_TRUE(gate.mayTake(w4, t + 4, kElapse));
}

namespace {
CommunityBootstrapData parse(const std::string& ini) {
    std::istringstream in(ini);
    boost::property_tree::ptree tree;
    boost::property_tree::read_ini(in, tree);
    return Config::parseCommunityBootstrapData(tree);
}

const char* const kKeyA = "3rUevsW5xfob6qDxWMDFwwTQCq39SYhzstuyfUGSDvF2";
const char* const kKeyB = "ESWr7ZThCmP2DVNNgrcsCvZZ2rrhDnFRW8oUFEpKpgdD";

cs::PublicKey decoded(const char* text) {
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(DecodeBase58(text, bytes));
    cs::PublicKey result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}
}  // namespace

TEST(CommunityBootstrapConfig, OffWithoutTheSection) {
    const auto c = parse("[params]\nipv6=false\n");
    EXPECT_EQ(c, CommunityBootstrapData{});
    EXPECT_TRUE(c.confidants.empty());
    EXPECT_EQ(c.stallMinutes, 30u);
}

TEST(CommunityBootstrapConfig, KeysAndStallMinutes) {
    const auto c = parse(std::string("[community_bootstrap]\nconfidants=") + kKeyA + ", " + kKeyB + "," + kKeyA + "\nstall_minutes=45\n");
    ASSERT_EQ(c.confidants.size(), 2u);
    EXPECT_EQ(c.confidants[0], decoded(kKeyA));
    EXPECT_EQ(c.confidants[1], decoded(kKeyB));
    EXPECT_EQ(c.stallMinutes, 45u);
}

TEST(CommunityBootstrapConfig, BadValuesAreLeftOut) {
    const auto c = parse(std::string("[community_bootstrap]\nconfidants=notakey ") + kKeyB + "\nstall_minutes=0\n");
    ASSERT_EQ(c.confidants.size(), 1u);
    EXPECT_EQ(c.confidants[0], decoded(kKeyB));
    EXPECT_EQ(c.stallMinutes, 30u);
    EXPECT_EQ(parse("[community_bootstrap]\nstall_minutes=10081\n").stallMinutes, 30u);
    EXPECT_EQ(parse("[community_bootstrap]\nstall_minutes=5m\n").stallMinutes, 30u);
    EXPECT_EQ(parse("[community_bootstrap]\nstall_minutes=1\n").stallMinutes, 1u);
    // an empty list keeps it off
    EXPECT_TRUE(parse("[community_bootstrap]\nconfidants=\n").confidants.empty());
}
