#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <base58.h>
#include <config.hpp>
#include <cscrypto/cscrypto.hpp>
#include <csnode/communitybootstrap.hpp>
#include <csnode/idatastream.hpp>

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

TEST(CommunityBootstrap, OneTablePerHalfRoundElapseUnlessHeavierFromALowerStarter) {
    CommunityBootstrapGate gate;
    gate.onLastSequence(500, 0);
    const auto w5 = communityTableWeight(kInitial, kCommunity, table({1, 3, 5, 7, 9}));
    const auto w4 = communityTableWeight(kInitial, kCommunity, table({1, 3, 5, 7}));
    const uint64_t t = kStall;
    EXPECT_TRUE(gate.mayTake(w4, key(1), t, kElapse));
    gate.onTaken(w4, key(1), t);
    // a repeat or a lighter table right after: dropped, so a listed key cannot force restarts
    EXPECT_FALSE(gate.mayTake(w4, key(1), t + 1, kElapse));
    EXPECT_FALSE(gate.mayTake(w4 - 1, key(1), t + kElapse / 2 - 1, kElapse));
    // a heavier one of the same starter at once: the table of more nodes wins
    EXPECT_TRUE(gate.mayTake(w5, key(1), t + 1, kElapse));
    gate.onTaken(w5, key(1), t + 1);
    EXPECT_FALSE(gate.mayTake(w4, key(1), t + 2, kElapse));
    // the starter's repeat, a round_elapse_time later
    EXPECT_TRUE(gate.mayTake(w5, key(1), t + 1 + kElapse, kElapse));
    EXPECT_TRUE(gate.mayTake(w4, key(1), t + 1 + kElapse / 2, kElapse));
    // progress forgets the tables of this stall
    gate.onProgress(t + 3);
    EXPECT_TRUE(gate.mayTake(w4, key(3), t + 4, kElapse));
}

// A hostile listed key above the starter pads its table with keys of stopped nodes: heavier, but it
// cannot displace the running table; a lower starter that saw more nodes can
TEST(CommunityBootstrap, AHigherListedKeyCannotDisplaceATableByPaddingIt) {
    const std::set<cs::PublicKey> initial = keys({1, 2, 4, 6, 8});
    const std::set<cs::PublicKey> community = keys({3, 5, 7, 9, 11, 13, 15});  // 4 make a majority
    CommunityBootstrapGate gate;
    gate.onLastSequence(500, 0);
    const uint64_t t = kStall;
    const auto running = table({3, 5, 7, 9});
    ASSERT_TRUE(isCommunityTable(initial, community, running));
    const auto wRunning = communityTableWeight(initial, community, running);
    ASSERT_TRUE(gate.mayTake(wRunning, running.front(), t, kElapse));
    gate.onTaken(wRunning, running.front(), t);
    // key 5 pads with the stopped 2, 4, 6, 8, 11, 13: more keys, heavier, refused within the window
    const auto padded = table({2, 4, 5, 6, 7, 8, 9, 11, 13});
    ASSERT_TRUE(isCommunityTable(initial, community, padded));
    ASSERT_GT(communityTableWeight(initial, community, padded), wRunning);
    EXPECT_FALSE(gate.mayTake(communityTableWeight(initial, community, padded), key(5), t + 1, kElapse));
    // the same starter, or a lower one, that saw more nodes: taken
    const auto more = table({3, 5, 7, 9, 11});
    EXPECT_TRUE(gate.mayTake(communityTableWeight(initial, community, more), key(3), t + 1, kElapse));
    const auto lower = table({1, 3, 5, 7, 9});
    EXPECT_TRUE(gate.mayTake(communityTableWeight(initial, community, lower), key(1), t + 1, kElapse));
    // after half a round_elapse_time any table: a repeat is needed when a round fails
    EXPECT_TRUE(gate.mayTake(communityTableWeight(initial, community, padded), key(5), t + kElapse / 2, kElapse));
}

TEST(CommunityBootstrap, ANeighbourOneBlockAheadHoldsTheGateOnlyForAFurtherStall) {
    using cs::isNeighbourAhead;
    EXPECT_FALSE(isNeighbourAhead(500, 500, kStall, kStall));
    EXPECT_FALSE(isNeighbourAhead(499, 500, kStall, kStall));
    // two blocks ahead: always, this node is behind
    EXPECT_TRUE(isNeighbourAhead(502, 500, 100 * kStall, kStall));
    // one block ahead: sync gets it in seconds; while it does not, only for a further stall_minutes
    EXPECT_TRUE(isNeighbourAhead(501, 500, kStall, kStall));
    EXPECT_TRUE(isNeighbourAhead(501, 500, 2 * kStall - 1, kStall));
    EXPECT_FALSE(isNeighbourAhead(501, 500, 2 * kStall, kStall));
}

TEST(CommunityBootstrap, AClockJumpIsProgress) {
    constexpr uint64_t kGap = 5 * kMinute;
    constexpr uint64_t kWall = 1'700'000'000'000ULL;
    CommunityBootstrapGate gate;
    gate.onClock(0, kWall, kGap);
    gate.onLastSequence(500, 0);
    // looks every round_elapse_time while stalled: no jump
    for (uint64_t t = kElapse; t <= kStall; t += kElapse) {
        gate.onClock(t, kWall + t, kGap);
    }
    ASSERT_TRUE(gate.isStalled(kStall, kStall));
    // the host slept for an hour: the wall clock jumped (the steady one may not count the sleep)
    gate.onClock(kStall + kElapse, kWall + kStall + kElapse + 60 * kMinute, kGap);
    EXPECT_FALSE(gate.isStalled(kStall + kElapse + 1, kStall));
    EXPECT_TRUE(gate.isStalled(2 * kStall + kElapse, kStall));
    // a wall clock set back, and a steady clock jump, as well
    for (const bool wallBack : {true, false}) {
        CommunityBootstrapGate other;
        other.onClock(0, kWall, kGap);
        other.onLastSequence(500, 0);
        for (uint64_t t = kElapse; t <= kStall; t += kElapse) {
            other.onClock(t, kWall + t, kGap);
        }
        ASSERT_TRUE(other.isStalled(kStall, kStall));
        const uint64_t steady = wallBack ? kStall + kElapse : kStall + 10 * kMinute;
        const uint64_t wall = wallBack ? kWall + kStall - 10 * kMinute : kWall + kStall + kElapse;
        other.onClock(steady, wall, kGap);
        EXPECT_FALSE(other.isStalled(steady, kStall)) << (wallBack ? "wall clock set back" : "steady clock jump");
    }
}

// lists of even size: the starter must not drop the key that holds the majority of the list
TEST(CommunityBootstrap, AnEvenSetKeepsItsMajorityOfTheList) {
    using cs::dropForOddTable;
    const std::set<cs::PublicKey> initial = keys({2, 4, 6, 8, 10});
    const std::set<cs::PublicKey> six = keys({1, 3, 5, 7, 9, 11});
    // 4 of 6 listed left: dropping one leaves 3, not more than half: kept even
    EXPECT_FALSE(dropForOddTable(initial, six, table({1, 3, 5, 7})));
    // 6 of 6: 5 still hold the majority, the official rule applies
    EXPECT_TRUE(dropForOddTable(initial, six, table({1, 3, 5, 7, 9, 11})));
    // a list of 10, 6 left: kept; 8 left: dropped to 7
    const std::set<cs::PublicKey> ten = keys({1, 3, 5, 7, 9, 11, 13, 15, 17, 19});
    EXPECT_FALSE(dropForOddTable(initial, ten, table({1, 3, 5, 7, 9, 11})));
    EXPECT_TRUE(dropForOddTable(initial, ten, table({1, 3, 5, 7, 9, 11, 13, 15})));
    // odd sets are never touched; regular sets (no list, or initial keys only) as in the official code
    EXPECT_FALSE(dropForOddTable(initial, six, table({1, 3, 5, 7, 9})));
    EXPECT_TRUE(dropForOddTable(initial, {}, table({2, 4, 6, 8})));
    EXPECT_TRUE(dropForOddTable(initial, six, table({2, 4, 6, 8})));
    // a community table whose only key outside the initial confidants is its last: the rest is a
    // regular table, the official rule applies
    EXPECT_TRUE(dropForOddTable(kInitial, kCommunity, table({1, 2, 3, 4, 5, 7})));
    EXPECT_FALSE(isCommunityTable(kInitial, kCommunity, table({1, 2, 3, 4, 5})));
}

// a set with a listed key that holds no majority of the list must not hold back the regular restart
// the initial confidants in it would make without the list
TEST(CommunityBootstrap, AMixedSetWithoutAMajorityOfTheListFallsBackToTheRegularTable) {
    using cs::bootstrapTableOf;
    const std::set<cs::PublicKey> initial = keys({1, 2, 3, 4, 6});
    const std::set<cs::PublicKey> list = keys({5, 7, 9, 11, 13});
    // the 5 initial confidants and listed 5 alive: 1 of 5 listed is no community table; the 5 initial
    // ones make the regular table they make on a node without the list
    EXPECT_EQ(bootstrapTableOf(initial, list, keys({1, 2, 3, 4, 5, 6})), keys({1, 2, 3, 4, 6}));
    EXPECT_EQ(bootstrapTableOf(initial, {}, keys({1, 2, 3, 4, 6})), keys({1, 2, 3, 4, 6}));
    // 4 initial ones and listed 5: even, the official odd rule leaves 3, below MinTrustedNodes: wait
    EXPECT_EQ(bootstrapTableOf(initial, list, keys({1, 2, 3, 4, 5})), keys({1, 2, 3}));
    // a community table stays one
    EXPECT_EQ(bootstrapTableOf(initial, list, keys({1, 5, 7, 9})), keys({1, 5, 7, 9}));
    EXPECT_EQ(bootstrapTableOf(initial, list, keys({1, 3, 5, 7, 9, 11})), keys({1, 3, 5, 7, 9}));
    // initial keys only: as before
    EXPECT_EQ(bootstrapTableOf(initial, list, keys({1, 2, 3, 4})), keys({1, 2, 3}));
}

namespace {
struct Starter {
    cs::PublicKey pub;
    cs::PrivateKey priv;
};

// keys live in sodium's guarded memory: the library must be initialised first
std::vector<Starter> makeStarters(size_t n) {
    EXPECT_TRUE(cscrypto::cryptoInit());
    std::vector<Starter> result(n);
    for (auto& s : result) {
        s.priv = cscrypto::generateKeyPair(s.pub);
    }
    std::sort(result.begin(), result.end(), [](const Starter& a, const Starter& b) { return a.pub < b.pub; });
    return result;
}

// what Node::getBootstrapTable reads of a table with a key that is not an initial confidant: the
// count, the keys, the signature over bootstrapSignedBytes(round, count and keys); the rest ignored
bool readsAsCommunityTable(const cs::Bytes& payload, cs::RoundNumber rNum, const std::set<cs::PublicKey>& initial,
                           const std::set<cs::PublicKey>& community) {
    cs::IDataStream stream(payload.data(), payload.size());
    uint8_t confSize = 0;
    stream >> confSize;
    cs::PublicKeys confidants;
    for (int i = 0; i < confSize; ++i) {
        cs::PublicKey k;
        stream >> k;
        confidants.push_back(k);
    }
    cs::Signature sig;
    stream >> sig;
    if (!stream.isValid() || confidants.empty()) {
        return false;
    }
    const size_t signedLen = 1 + size_t(confSize) * cscrypto::kPublicKeySize;
    const cs::Bytes buf = cs::bootstrapSignedBytes(rNum, payload.data(), signedLen);
    return isCommunityTable(initial, community, confidants)
           && cscrypto::verifySignature(sig, confidants.front(), buf.data(), buf.size());
}
}  // namespace

TEST(CommunityBootstrap, TheStartersTableVerifiesAsTheReceiverReadsIt) {
    const auto s = makeStarters(5);
    std::set<cs::PublicKey> listed;
    for (const auto& st : s) {
        listed.insert(st.pub);
    }
    const std::set<cs::PublicKey> initial = keys({200, 201, 202, 203, 204});
    const cs::RoundNumber round = 123'456;
    const cs::Bytes payload = cs::makeCommunityTable(round, listed, s.front().priv, 0x01020304);
    // count, keys, signature, 4 random bytes
    ASSERT_EQ(payload.size(), 1 + 5 * cscrypto::kPublicKeySize + cscrypto::kSignatureSize + sizeof(uint32_t));
    EXPECT_EQ(payload[0], 5);
    EXPECT_TRUE(readsAsCommunityTable(payload, round, initial, listed));
    // signed by another listed key than the first: not its starter's table
    const cs::Bytes notStarter = cs::makeCommunityTable(round, listed, s[1].priv, 0x01020304);
    EXPECT_FALSE(readsAsCommunityTable(notStarter, round, initial, listed));
    // a key changed after signing
    cs::Bytes altered = payload;
    altered[1 + cscrypto::kPublicKeySize] ^= 1;
    EXPECT_FALSE(readsAsCommunityTable(altered, round, initial, listed));
}

// The p2p layer drops a broadcast whose bytes it has seen (Host::IsDuplicate, SHA1 of the data): the
// starter's repeat in the same round must differ, and still verify
TEST(CommunityBootstrap, ARepeatInTheSameRoundIsANewMessage) {
    const auto s = makeStarters(4);
    std::set<cs::PublicKey> listed;
    for (const auto& st : s) {
        listed.insert(st.pub);
    }
    const std::set<cs::PublicKey> initial = keys({200, 201, 202, 203, 204});
    const cs::RoundNumber round = 777;
    const cs::Bytes first = cs::makeCommunityTable(round, listed, s.front().priv, 1);
    const cs::Bytes repeat = cs::makeCommunityTable(round, listed, s.front().priv, 2);
    EXPECT_NE(first, repeat);
    EXPECT_NE(cscrypto::calculateHash(first.data(), first.size()), cscrypto::calculateHash(repeat.data(), repeat.size()));
    EXPECT_TRUE(readsAsCommunityTable(first, round, initial, listed));
    EXPECT_TRUE(readsAsCommunityTable(repeat, round, initial, listed));
    // only the random bytes differ: the signature is deterministic
    const size_t signedEnd = first.size() - sizeof(uint32_t);
    EXPECT_TRUE(std::equal(first.begin(), first.begin() + signedEnd, repeat.begin()));
}

// a table recorded in one round and sent again in another: the round is in the signed bytes
TEST(CommunityBootstrap, ATableReplayedFromAnotherRoundDoesNotVerify) {
    const auto s = makeStarters(4);
    std::set<cs::PublicKey> listed;
    for (const auto& st : s) {
        listed.insert(st.pub);
    }
    const std::set<cs::PublicKey> initial = keys({200, 201, 202, 203, 204});
    const cs::Bytes payload = cs::makeCommunityTable(1000, listed, s.front().priv, 42);
    EXPECT_TRUE(readsAsCommunityTable(payload, 1000, initial, listed));
    EXPECT_FALSE(readsAsCommunityTable(payload, 1001, initial, listed));
    EXPECT_FALSE(readsAsCommunityTable(payload, 999, initial, listed));
}

namespace {
// the folder of config.ini in a test: none unless a test makes one
const std::string kNoDir = "no-such-dir-for-tests";

CommunityBootstrapData parse(const std::string& ini, const std::string& configDir = kNoDir) {
    std::istringstream in(ini);
    boost::property_tree::ptree tree;
    boost::property_tree::read_ini(in, tree);
    return Config::parseCommunityBootstrapData(tree, configDir);
}

// a folder standing for the one of config.ini, with list files in it, removed with the object
struct ConfigDir {
    std::filesystem::path dir;
    explicit ConfigDir(const char* name = "community_bootstrap_test")
    : dir(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
    }
    ~ConfigDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string path() const {
        return dir.string();
    }
    std::string write(const std::string& text, const char* name = "community_bootstrap.txt") const {
        std::ofstream(dir / name) << text;
        return (dir / name).string();
    }
};

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

// an official-style folder: no section and no list file, the fallback stays off
TEST(CommunityBootstrapConfig, OffWithoutTheSectionAndTheFile) {
    const ConfigDir dir;
    const auto c = parse("[params]\nipv6=false\n", dir.path());
    EXPECT_TRUE(c.confidants.empty());
    EXPECT_TRUE(c.enabled);
    EXPECT_EQ(c.stallMinutes, 10u);
    EXPECT_EQ(c.source, "no " + (dir.dir / "community_bootstrap.txt").string());
}

// a community package: no section, the shipped list file next to config.ini arms it with a 10 minute wait
TEST(CommunityBootstrapConfig, OnFromTheListFileWithoutTheSection) {
    const ConfigDir dir;
    const auto file = dir.write(std::string("# community nodes\n") + kKeyA + "  # one\n\n" + kKeyB + "\r\n" + kKeyA + "\nnotakey\n");
    const auto c = parse("[params]\nipv6=false\n", dir.path());
    ASSERT_EQ(c.confidants.size(), 2u);
    EXPECT_EQ(c.confidants[0], decoded(kKeyA));
    EXPECT_EQ(c.confidants[1], decoded(kKeyB));
    EXPECT_EQ(c.stallMinutes, 10u);
    EXPECT_EQ(c.source, file);
    // a section with only stall_minutes still takes the file
    const auto d = parse("[community_bootstrap]\nstall_minutes=20\n", dir.path());
    EXPECT_EQ(d.confidants.size(), 2u);
    EXPECT_EQ(d.stallMinutes, 20u);
}

// the list is read from the folder of config.ini only: a file in the working directory, or no known
// folder, arms nothing
TEST(CommunityBootstrapConfig, TheListIsNeverReadFromTheWorkingDirectory) {
    const ConfigDir cwd("community_bootstrap_test_cwd");
    cwd.write(kKeyA);
    const ConfigDir dir;
    const auto before = std::filesystem::current_path();
    std::filesystem::current_path(cwd.dir);
    const auto c = parse("[params]\nipv6=false\n", dir.path());
    const auto d = parse("[params]\nipv6=false\n", "");
    const auto e = parse("[community_bootstrap]\nconfidants_file=community_bootstrap.txt\n", dir.path());
    std::filesystem::current_path(before);
    EXPECT_TRUE(c.confidants.empty());
    EXPECT_TRUE(d.confidants.empty());
    EXPECT_TRUE(e.confidants.empty());
}

TEST(CommunityBootstrapConfig, EnabledFalseTurnsItOff) {
    const ConfigDir dir;
    dir.write(kKeyA);
    for (const char* off : {"false", "off", "no", "0", "False"}) {
        const auto c = parse(std::string("[community_bootstrap]\nenabled=") + off + "\n", dir.path());
        EXPECT_TRUE(c.confidants.empty()) << off;
        EXPECT_FALSE(c.enabled) << off;
    }
    EXPECT_TRUE(parse(std::string("[community_bootstrap]\nenabled=false\nconfidants=") + kKeyA + "\n").confidants.empty());
    EXPECT_EQ(parse("[community_bootstrap]\nenabled=true\n", dir.path()).confidants.size(), 1u);
    // anything else is taken as true
    EXPECT_EQ(parse("[community_bootstrap]\nenabled=maybe\n", dir.path()).confidants.size(), 1u);
}

TEST(CommunityBootstrapConfig, TheListOfConfigIniWinsOverTheFile) {
    const ConfigDir dir;
    dir.write(kKeyA);
    const auto c = parse(std::string("[community_bootstrap]\nconfidants=") + kKeyB + "\n", dir.path());
    ASSERT_EQ(c.confidants.size(), 1u);
    EXPECT_EQ(c.confidants[0], decoded(kKeyB));
    // an empty confidants= turns it off, as before
    EXPECT_TRUE(parse("[community_bootstrap]\nconfidants=\n", dir.path()).confidants.empty());
    // confidants_file names another file (relative: in the folder of config.ini; or absolute), or none
    const auto other = dir.write(kKeyB, "other.txt");
    for (const std::string& name : {std::string("other.txt"), other}) {
        const auto d = parse("[community_bootstrap]\nconfidants_file=" + name + "\n", dir.path());
        ASSERT_EQ(d.confidants.size(), 1u) << name;
        EXPECT_EQ(d.confidants[0], decoded(kKeyB)) << name;
    }
    EXPECT_TRUE(parse("[community_bootstrap]\nconfidants_file=\n", dir.path()).confidants.empty());
}

// the config observer compares the settings: turning it off is a change
TEST(CommunityBootstrapConfig, EqualityFollowsEverySetting) {
    const ConfigDir dir;
    dir.write(kKeyA);
    EXPECT_EQ(parse("", dir.path()), parse("", dir.path()));
    EXPECT_NE(parse("", dir.path()), parse("[community_bootstrap]\nenabled=false\n", dir.path()));
    EXPECT_NE(parse("", dir.path()), parse("[community_bootstrap]\nstall_minutes=11\n", dir.path()));
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
    EXPECT_EQ(c.stallMinutes, 10u);
    EXPECT_EQ(parse("[community_bootstrap]\nstall_minutes=10081\n").stallMinutes, 10u);
    EXPECT_EQ(parse("[community_bootstrap]\nstall_minutes=5m\n").stallMinutes, 10u);
    EXPECT_EQ(parse("[community_bootstrap]\nstall_minutes=1\n").stallMinutes, 1u);
    // an empty list keeps it off
    EXPECT_TRUE(parse("[community_bootstrap]\nconfidants=\n").confidants.empty());
}
