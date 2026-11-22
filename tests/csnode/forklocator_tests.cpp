#define TESTING

#include "gtest/gtest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <config.hpp>
#include <consensus.hpp>
#include <csdb/pool.hpp>
#include <csnode/blockchain.hpp>
#include <csnode/caches_serialization_manager.hpp>
#include <csnode/configholder.hpp>
#include <csnode/forkguardstate.hpp>
#include <csnode/forklocator.hpp>

// The search for the last common block while FORK HELD (cs::forkguard::ForkLocator), the record it
// fills in, and the checkpoints a chain cut leaves behind.

namespace {

using Locator = cs::forkguard::ForkLocator;
using Status = Locator::Status;
using namespace std::chrono_literals;

// two chains sharing blocks 0..fork: ours ("O") and the peer's ("P") above it
struct Chains {
    cs::Sequence fork = 0;
    cs::Sequence ourHead = 0;
    cs::Sequence peerHead = 0;

    static std::string common(cs::Sequence s) {
        return "C" + std::to_string(s);
    }
    std::string ours(cs::Sequence s) const {
        if (s > ourHead) {
            return {};
        }
        return s <= fork ? common(s) : "O" + std::to_string(s);
    }
    std::string theirs(cs::Sequence s) const {
        return s <= fork ? common(s) : "P" + std::to_string(s);
    }
    // verifiable: the node can verify the peer's first block after the fork point against its own
    // confidants (false: e.g. a bootstrap block)
    bool verifiable = true;
    Locator::PeerBlock peerBlock(cs::Sequence s) const {
        return {s, theirs(s), s == 0 ? std::string{} : theirs(s - 1), verifiable && s > 0 && s <= fork + 1};
    }
};

cs::PublicKey key(uint8_t b) {
    cs::PublicKey k{};
    k.fill(b);
    return k;
}

// a locator against `chains`, the peer answering with `answer` (by default its own chain); runs until
// the search ends (or 200 ticks), 7 s apart
struct Harness {
    Chains chains;
    Locator::Settings settings;
    std::vector<std::vector<cs::Sequence>> requests;
    bool connected = true;
    std::function<std::vector<Locator::PeerBlock>(const std::vector<cs::Sequence>&)> answer;
    std::optional<Locator> locator;
    Locator::Clock::time_point now = Locator::Clock::time_point{} + 1h;

    Locator& make() {
        Locator::Hooks hooks;
        hooks.ourHash = [this](cs::Sequence s) { return chains.ours(s); };
        hooks.sendRequest = [this](const cs::PublicKey&, const std::vector<cs::Sequence>& seqs) {
            if (!connected) {
                return false;
            }
            requests.push_back(seqs);
            return true;
        };
        locator.emplace(std::move(hooks), settings);
        return *locator;
    }

    std::vector<Locator::PeerBlock> honest(const std::vector<cs::Sequence>& seqs) const {
        std::vector<Locator::PeerBlock> out;
        for (auto s : seqs) {
            if (s <= chains.peerHead) {
                out.push_back(chains.peerBlock(s));
            }
        }
        return out;
    }

    Status run(const cs::PublicKey& peer = key(7)) {
        size_t answered = 0;
        for (int i = 0; i < 200 && locator->searching(); ++i) {
            locator->tick(now);
            if (requests.size() > answered) {
                answered = requests.size();
                const auto& seqs = requests.back();
                auto blocks = answer ? answer(seqs) : honest(seqs);
                std::vector<cs::Sequence> got;
                for (const auto& b : blocks) {
                    got.push_back(b.seq);
                }
                if (!answer && !got.empty()) {
                    EXPECT_TRUE(locator->expects(peer, got));
                }
                locator->onReply(peer, blocks, now);
            }
            now += 7s;
        }
        return locator->status();
    }
};

}  // namespace

// a fork 397 blocks under the upper bound: found exactly, in a few requests of a few blocks each
TEST(ForkLocator, FindsTheExactForkPoint) {
    Harness h;
    h.chains = {533, 931, 1500};
    auto& loc = h.make();
    loc.start(key(7), 930, 0, h.now);
    EXPECT_EQ(h.run(), Status::Exact);
    EXPECT_EQ(loc.forkPoint(), 533u);
    EXPECT_EQ(loc.forkPointHash(), Chains::common(533));
    EXPECT_TRUE(loc.verified());
    EXPECT_LE(h.requests.size(), 8u);
    for (const auto& r : h.requests) {
        EXPECT_LE(r.size(), h.settings.probesPerRequest);
        for (auto s : r) {
            EXPECT_LE(s, 931u);  // never above the bound + 1
        }
    }
}

// the bound itself is the fork point (the chains differ right above it), or the fork is at the floor
TEST(ForkLocator, ForkAtTheBoundOrAtTheFloor) {
    {
        Harness h;
        h.chains = {930, 931, 1500};
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        EXPECT_EQ(h.run(), Status::Exact);
        EXPECT_EQ(loc.forkPoint(), 930u);
    }
    {
        Harness h;
        h.chains = {500, 931, 1500};
        auto& loc = h.make();
        loc.start(key(7), 930, 500, h.now);  // the first stored block is 500
        EXPECT_EQ(h.run(), Status::Exact);
        EXPECT_EQ(loc.forkPoint(), 500u);
    }
}

// a fork below the bound of the search: "deeper", at the lowest block searched
TEST(ForkLocator, DeepForkBeyondTheBound) {
    Harness h;
    h.settings.maxDepth = 2000;
    h.chains = {100, 5001, 6000};
    auto& loc = h.make();
    loc.start(key(7), 5000, 0, h.now);
    EXPECT_EQ(loc.floor(), 3000u);
    EXPECT_EQ(h.run(), Status::Deeper);
    EXPECT_EQ(loc.forkPoint(), 3000u);
    EXPECT_EQ(h.requests.size(), 1u);  // the floor is in the first request

    // a pruned node: never below its first stored block
    Harness p;
    p.chains = {100, 5001, 6000};
    auto& pruned = p.make();
    pruned.start(key(7), 5000, 4500, p.now);
    EXPECT_EQ(p.run(), Status::Deeper);
    EXPECT_EQ(pruned.forkPoint(), 4500u);

    // the genesis block is never asked for, even when a peer would answer it with another hash
    Harness g;
    g.chains = {533, 931, 1500};
    g.answer = [&](const std::vector<cs::Sequence>& seqs) {
        auto out = g.honest(seqs);
        for (auto& b : out) {
            EXPECT_NE(b.seq, 0u);
            if (b.seq == 0) {
                b.hash = "other genesis";
            }
        }
        return out;
    };
    auto& genesis = g.make();
    genesis.start(key(7), 930, 0, g.now);
    EXPECT_EQ(genesis.floor(), 1u);
    EXPECT_EQ(g.run(), Status::Exact);
    EXPECT_EQ(genesis.forkPoint(), 533u);

    EXPECT_EQ(cs::forkguard::locateDepth(100), 2000u);
    EXPECT_EQ(cs::forkguard::locateDepth(10), 1000u);
    EXPECT_EQ(cs::forkguard::locateDepth(0), 2000u);
    EXPECT_EQ(cs::forkguard::locateDepth(1000000), 10000u);
}

// a peer that lies cannot make the search say anything that does not hold together
TEST(ForkLocator, LyingPeer) {
    // our own blocks at every height, also above the bound: the chains cannot differ there and match
    // above it
    {
        Harness h;
        h.chains = {533, 931, 1500};
        h.answer = [&](const std::vector<cs::Sequence>& seqs) {
            std::vector<Locator::PeerBlock> out;
            for (auto s : seqs) {
                out.push_back({s, h.chains.ours(s), s ? h.chains.ours(s - 1) : std::string{}});
            }
            return out;
        };
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        EXPECT_EQ(h.run(), Status::Failed);
        EXPECT_NE(loc.why().find("chains differ"), std::string::npos) << loc.why();
    }
    // matches above a difference
    {
        Harness h;
        h.chains = {533, 931, 1500};
        h.answer = [&](const std::vector<cs::Sequence>& seqs) {
            std::vector<Locator::PeerBlock> out;
            for (auto s : seqs) {
                // the lowest height differs, the others are ours
                const bool lie = s != seqs.front();
                out.push_back({s, lie ? h.chains.ours(s) : "X" + std::to_string(s), "Y"});
            }
            return out;
        };
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        EXPECT_EQ(h.run(), Status::Failed);
    }
    // the right answer, but the first block above the fork point is not built on ours
    {
        Harness h;
        h.chains = {533, 931, 1500};
        h.answer = [&](const std::vector<cs::Sequence>& seqs) {
            auto out = h.honest(seqs);
            for (auto& b : out) {
                if (b.seq == 534) {
                    b.previous = "forged";
                }
            }
            return out;
        };
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        EXPECT_EQ(h.run(), Status::Failed);
        // "not built on our block #533", or "#533 and #534 are not chained" when both came together
        EXPECT_NE(loc.why().find("#534"), std::string::npos) << loc.why();
    }
    // consecutive blocks that are not chained
    {
        Harness h;
        h.chains = {533, 931, 1500};
        h.answer = [&](const std::vector<cs::Sequence>& seqs) {
            auto out = h.honest(seqs);
            for (auto& b : out) {
                b.previous = "broken";
            }
            return out;
        };
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        EXPECT_EQ(h.run(), Status::Failed);
    }
    // blocks nobody asked for are not taken; a peer that only sends those makes the search give up
    {
        Harness h;
        h.chains = {533, 931, 1500};
        h.answer = [&](const std::vector<cs::Sequence>&) {
            return std::vector<Locator::PeerBlock>{h.chains.peerBlock(1200)};
        };
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        EXPECT_FALSE(loc.expects(key(7), {1200}));
        h.locator->tick(h.now);
        EXPECT_FALSE(loc.expects(key(7), {1200}));
        EXPECT_FALSE(loc.expects(key(8), {h.requests.back().front()}));  // another peer
        EXPECT_TRUE(loc.expects(key(7), {h.requests.back().front()}));
        EXPECT_EQ(h.run(), Status::Failed);
        EXPECT_NE(loc.why().find("no usable reply"), std::string::npos) << loc.why();
    }
}

// no peer to ask, or a peer that never answers: the search gives up, nothing is decided
TEST(ForkLocator, NoPeer) {
    {
        Harness h;
        h.chains = {533, 931, 1500};
        h.connected = false;
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        EXPECT_EQ(h.run(), Status::Failed);
        EXPECT_NE(loc.why().find("not connected"), std::string::npos) << loc.why();
        EXPECT_EQ(loc.requestsSent(), 0u);
    }
    {
        Harness h;
        h.chains = {533, 931, 1500};
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        for (int i = 0; i < 200 && loc.searching(); ++i) {
            loc.tick(h.now);
            h.now += 7s;
        }
        EXPECT_EQ(loc.status(), Status::Failed);
        EXPECT_NE(loc.why().find("no reply"), std::string::npos) << loc.why();
        EXPECT_EQ(loc.requestsSent(), h.settings.maxAttempts);
    }
    {
        // refusals (duplicate request, no such blocks) count as failed attempts too
        Harness h;
        h.chains = {533, 931, 1500};
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        for (int i = 0; i < 200 && loc.searching(); ++i) {
            loc.tick(h.now);
            loc.onRefused(key(8), h.now);  // another peer: ignored
            loc.onRefused(key(7), h.now);
            h.now += 7s;
        }
        EXPECT_EQ(loc.status(), Status::Failed);
        EXPECT_NE(loc.why().find("refuses"), std::string::npos) << loc.why();
    }
    {
        // a duplicate-request refusal is tried again, more often, then given up as well
        Harness h;
        h.chains = {533, 931, 1500};
        auto& loc = h.make();
        loc.start(key(7), 930, 0, h.now);
        for (size_t i = 0; i < h.settings.maxAttempts + 1; ++i) {
            loc.tick(h.now);
            loc.onRefused(key(7), h.now, Locator::Refusal::Duplicate);
            h.now += 7s;
        }
        EXPECT_TRUE(loc.searching());
        for (int i = 0; i < 200 && loc.searching(); ++i) {
            loc.tick(h.now);
            loc.onRefused(key(7), h.now, Locator::Refusal::Duplicate);
            h.now += 7s;
        }
        EXPECT_EQ(loc.status(), Status::Failed);
        EXPECT_NE(loc.why().find("duplicates"), std::string::npos) << loc.why();
    }
}

// a reply with more blocks than asked for, or one height twice, is not for the search (each block is
// checked once at most)
TEST(ForkLocator, DuplicatedBlocksReplyIsDropped) {
    Harness h;
    h.chains = {533, 931, 1500};
    auto& loc = h.make();
    loc.start(key(7), 930, 0, h.now);
    loc.tick(h.now);
    ASSERT_EQ(h.requests.size(), 1u);
    const auto asked = h.requests.back();
    EXPECT_EQ(loc.inFlightCount(), asked.size());
    EXPECT_TRUE(loc.expects(key(7), asked));
    EXPECT_FALSE(loc.expects(key(7), {asked[0], asked[0]}));
    auto tooMany = asked;
    tooMany.push_back(asked.back());
    EXPECT_FALSE(loc.expects(key(7), tooMany));
    EXPECT_TRUE(loc.expects(key(7), {asked[1], asked[0]}));  // any order
}

// a pruned peer does not store the lowest height asked for: the floor goes up past it instead of
// counting a failed attempt, a limited number of times
TEST(ForkLocator, PrunedPeerRaisesTheFloor) {
    Harness h;
    h.chains = {533, 931, 1500};
    auto& loc = h.make();
    loc.start(key(7), 930, 0, h.now);
    loc.tick(h.now);
    const auto first = h.requests.back();
    loc.onRefused(key(7), h.now, Locator::Refusal::NoSuchBlocks);
    EXPECT_TRUE(loc.searching());
    EXPECT_EQ(loc.floor(), first[1]);
    // then the peer answers: the search ends exactly, above the raised floor
    h.requests.clear();  // the refused request is not answered
    EXPECT_EQ(h.run(), Status::Exact);
    EXPECT_EQ(loc.forkPoint(), 533u);

    // a peer that never has the blocks: given up after maxFloorRaises
    Harness p;
    p.chains = {533, 931, 1500};
    p.settings.maxFloorRaises = 3;
    auto& none = p.make();
    none.start(key(7), 930, 0, p.now);
    for (int i = 0; i < 50 && none.searching(); ++i) {
        none.tick(p.now);
        none.onRefused(key(7), p.now, Locator::Refusal::NoSuchBlocks);
        p.now += 7s;
    }
    EXPECT_EQ(none.status(), Status::Failed);
    EXPECT_NE(none.why().find("does not store"), std::string::npos) << none.why();
}

// one request at a time, at most one per minInterval
TEST(ForkLocator, RateLimited) {
    Harness h;
    h.chains = {533, 931, 1500};
    auto& loc = h.make();
    loc.start(key(7), 930, 0, h.now);
    loc.tick(h.now);
    ASSERT_EQ(h.requests.size(), 1u);
    loc.tick(h.now + 1s);
    EXPECT_EQ(h.requests.size(), 1u);   // waits for the reply
    loc.onReply(key(7), h.honest(h.requests.back()), h.now + 1s);
    loc.tick(h.now + 2s);
    EXPECT_EQ(h.requests.size(), 1u);   // replied, but too early for the next one
    loc.tick(h.now + 6s);
    EXPECT_EQ(h.requests.size(), 2u);
    loc.stop();
    loc.tick(h.now + 60s);
    EXPECT_EQ(h.requests.size(), 2u);
    EXPECT_EQ(loc.status(), Status::Idle);
}

namespace {
struct TempFolder {
    std::filesystem::path path;
    explicit TempFolder(const std::string& name) {
        path = std::filesystem::temp_directory_path() /
               ("csnode-locator-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string str() const {
        return path.string();
    }
};

cs::forkguard::ForkRecord heldRecord() {
    cs::forkguard::ForkRecord r;
    r.reason = "authority";
    r.sinceUnix = 1795000000;
    r.highWater = 931;
    r.ourHead = 931;
    r.forkPoint = 930;
    r.forkPointHash = "C930";
    r.competingSeq = 932;
    r.competingHash = "P932";
    r.peerKey = "key";
    return r;
}
}  // namespace

// a lying peer that reports a deeper fork point: it can send blocks of its own below the real fork,
// chained and consistent, but it cannot make the block after its claimed point verify against our
// confidants there: the point is found but not verified (a hint only), and a bootstrap-flagged block
// after the point stays unverified too
TEST(ForkLocator, LyingPeerReportingADeeperPointIsNotVerified) {
    Harness h;
    h.chains = {533, 931, 1500};
    Chains fake = h.chains;
    fake.fork = 300;  // the liar's chain parts from ours at #300
    h.answer = [&](const std::vector<cs::Sequence>& seqs) {
        std::vector<Locator::PeerBlock> out;
        for (auto s : seqs) {
            auto b = fake.peerBlock(s);
            b.verified = false;  // its forged blocks never verify against our confidants
            out.push_back(b);
        }
        return out;
    };
    auto& loc = h.make();
    loc.start(key(7), 930, 0, h.now);
    EXPECT_EQ(h.run(), Status::Exact);
    EXPECT_EQ(loc.forkPoint(), 300u);
    EXPECT_FALSE(loc.verified());

    // an honest peer whose block after the fork point is a bootstrap block: located, not verified
    Harness b;
    b.chains = {533, 931, 1500};
    b.chains.verifiable = false;
    auto& boot = b.make();
    boot.start(key(7), 930, 0, b.now);
    EXPECT_EQ(b.run(), Status::Exact);
    EXPECT_EQ(boot.forkPoint(), 533u);
    EXPECT_FALSE(boot.verified());
}

namespace {
// several candidate peers, each answering (or not) by its own rule; the request goes to the peer the
// locator names, the reply comes back from it
struct MultiHarness {
    Chains chains;
    Locator::Settings settings;
    std::map<uint8_t, std::function<std::optional<std::vector<Locator::PeerBlock>>(const std::vector<cs::Sequence>&)>> peers;
    std::vector<std::pair<uint8_t, std::vector<cs::Sequence>>> requests;
    std::optional<Locator> locator;
    Locator::Clock::time_point now = Locator::Clock::time_point{} + 1h;

    Locator& make() {
        Locator::Hooks hooks;
        hooks.ourHash = [this](cs::Sequence s) { return chains.ours(s); };
        hooks.sendRequest = [this](const cs::PublicKey& to, const std::vector<cs::Sequence>& seqs) {
            requests.emplace_back(to[0], seqs);
            return true;
        };
        locator.emplace(std::move(hooks), settings);
        return *locator;
    }
    // a peer on the competing branch (the honest one)
    std::vector<Locator::PeerBlock> competing(const std::vector<cs::Sequence>& seqs) const {
        std::vector<Locator::PeerBlock> out;
        for (auto s : seqs) {
            if (s <= chains.peerHead) {
                out.push_back(chains.peerBlock(s));
            }
        }
        return out;
    }
    // a peer on our own side: our blocks
    std::vector<Locator::PeerBlock> sameSide(const std::vector<cs::Sequence>& seqs) const {
        std::vector<Locator::PeerBlock> out;
        for (auto s : seqs) {
            if (s <= chains.ourHead) {
                out.push_back({s, chains.ours(s), s == 0 ? std::string{} : chains.ours(s - 1), false});
            }
        }
        return out;
    }
    Status run() {
        size_t answered = 0;
        for (int i = 0; i < 400 && locator->searching(); ++i) {
            locator->tick(now);
            if (requests.size() > answered) {
                answered = requests.size();
                const auto& [who, seqs] = requests.back();
                const auto it = peers.find(who);
                if (it != peers.end()) {
                    if (auto blocks = it->second(seqs)) {
                        locator->onReply(key(who), *blocks, now);
                    }
                }
            }
            now += 7s;
        }
        return locator->status();
    }
    std::set<uint8_t> asked() const {
        std::set<uint8_t> out;
        for (const auto& r : requests) {
            out.insert(r.first);
        }
        return out;
    }
};
}  // namespace

// the peer of the refused block sits on our own side (it relayed the block): its answers locate
// nothing, and the search goes on with the next candidate, which finds the point
TEST(ForkLocator, RotatesAfterAUselessPeer) {
    MultiHarness h;
    h.chains = {533, 931, 1500};
    h.peers[3] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(h.sameSide(seqs)); };
    h.peers[8] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(h.competing(seqs)); };
    auto& loc = h.make();
    loc.start(std::vector<cs::PublicKey>{key(3), key(8)}, 930, 0, h.now);
    EXPECT_EQ(h.run(), Status::Exact);
    EXPECT_EQ(loc.forkPoint(), 533u);
    EXPECT_TRUE(loc.verified());
    EXPECT_EQ(loc.resultPeer(), key(8));
    EXPECT_EQ(loc.nextHash(), "P534");
    EXPECT_EQ(loc.rotations(), 1u);
    EXPECT_EQ(h.asked(), (std::set<uint8_t>{3, 8}));
    // one request at a time, never closer than minInterval, whoever is asked
    for (size_t i = 0; i < h.requests.size(); ++i) {
        EXPECT_LE(h.requests[i].second.size(), h.settings.probesPerRequest);
    }

    // a peer that never answers, then one that answers nothing usable: each costs one attempt, then
    // the next candidate
    MultiHarness t;
    t.chains = {533, 931, 1500};
    t.peers[4] = [](const std::vector<cs::Sequence>&) { return std::optional<std::vector<Locator::PeerBlock>>{}; };
    t.peers[5] = [](const std::vector<cs::Sequence>&) { return std::optional(std::vector<Locator::PeerBlock>{}); };
    t.peers[8] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(t.competing(seqs)); };
    auto& rot = t.make();
    rot.start(std::vector<cs::PublicKey>{key(4), key(5), key(8)}, 930, 0, t.now);
    EXPECT_EQ(t.run(), Status::Exact);
    EXPECT_EQ(rot.forkPoint(), 533u);
    EXPECT_EQ(rot.resultPeer(), key(8));
    EXPECT_EQ(rot.rotations(), 2u);
    ASSERT_GE(t.requests.size(), 3u);
    EXPECT_EQ(t.requests[0].first, 4);
    EXPECT_EQ(t.requests[1].first, 5);
    EXPECT_EQ(t.requests[2].first, 8);

    // every candidate useless: fails after each one was asked, and a late reply of an earlier
    // candidate is not taken
    MultiHarness f;
    f.chains = {533, 931, 1500};
    f.peers[3] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(f.sameSide(seqs)); };
    f.peers[6] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(f.sameSide(seqs)); };
    auto& none = f.make();
    none.start(std::vector<cs::PublicKey>{key(3), key(6), key(3)}, 930, 0, f.now);
    EXPECT_EQ(none.candidates().size(), 2u);  // duplicates dropped
    EXPECT_EQ(f.run(), Status::Failed);
    EXPECT_EQ(f.asked(), (std::set<uint8_t>{3, 6}));

    MultiHarness l;
    l.chains = {533, 931, 1500};
    auto& late = l.make();
    late.start(std::vector<cs::PublicKey>{key(4), key(8)}, 930, 0, l.now);
    late.tick(l.now);
    ASSERT_EQ(l.requests.size(), 1u);
    const auto first = l.requests.back().second;
    late.tick(l.now + 21s);  // no reply: the next candidate
    EXPECT_EQ(late.peer(), key(8));
    std::vector<cs::Sequence> got;
    for (const auto& b : l.competing(first)) {
        got.push_back(b.seq);
    }
    EXPECT_FALSE(late.expects(key(4), got));
    late.onReply(key(4), l.competing(first), l.now + 22s);
    EXPECT_TRUE(late.searching());
}

// a point is exact only when the peer's next block verifies: an unverified answer is kept as a hint
// while another candidate is asked; a verified answer wins; with no verified answer, the hint
TEST(ForkLocator, ExactOnlyWhenVerifiedAcrossCandidates) {
    MultiHarness h;
    h.chains = {533, 931, 1500};
    Chains fake = h.chains;
    fake.fork = 300;
    const auto liar = [&](const std::vector<cs::Sequence>& seqs) {
        std::vector<Locator::PeerBlock> out;
        for (auto s : seqs) {
            auto b = fake.peerBlock(s);
            b.verified = false;
            out.push_back(b);
        }
        return std::optional(out);
    };
    h.peers[2] = liar;
    h.peers[8] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(h.competing(seqs)); };
    auto& loc = h.make();
    loc.start(std::vector<cs::PublicKey>{key(2), key(8)}, 930, 0, h.now);
    EXPECT_EQ(h.run(), Status::Exact);
    EXPECT_TRUE(loc.verified());
    EXPECT_EQ(loc.forkPoint(), 533u);
    EXPECT_EQ(loc.resultPeer(), key(8));

    // a hostile candidate claiming an unverified fork point ABOVE the true one, then an honest one: the
    // verified answer wins, the hostile hint is dropped
    MultiHarness up;
    up.chains = {533, 931, 1500};
    Chains higher = up.chains;
    higher.fork = 700;
    up.peers[2] = [&](const std::vector<cs::Sequence>& seqs) {
        std::vector<Locator::PeerBlock> out;
        for (auto s : seqs) {
            // ours up to 700 (it claims to share them), its own blocks above, never verifiable
            Locator::PeerBlock b{s, s <= 700 ? up.chains.ours(s) : higher.theirs(s),
                                 s == 0 ? std::string{} : (s - 1 <= 700 ? up.chains.ours(s - 1) : higher.theirs(s - 1)), false};
            out.push_back(b);
        }
        return std::optional(out);
    };
    up.peers[8] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(up.competing(seqs)); };
    auto& hostile = up.make();
    hostile.start(std::vector<cs::PublicKey>{key(2), key(8)}, 930, 0, up.now);
    EXPECT_EQ(up.run(), Status::Exact);
    EXPECT_TRUE(hostile.verified());
    EXPECT_EQ(hostile.forkPoint(), 533u);
    EXPECT_EQ(hostile.resultPeer(), key(8));
    EXPECT_EQ(hostile.nextHash(), "P534");

    // only unverified answers: the hint, not verified (the node keeps it a hint)
    MultiHarness u;
    u.chains = {533, 931, 1500};
    u.peers[2] = liar;
    u.peers[3] = [&](const std::vector<cs::Sequence>& seqs) { return std::optional(u.sameSide(seqs)); };
    auto& hint = u.make();
    hint.start(std::vector<cs::PublicKey>{key(2), key(3)}, 930, 0, u.now);
    EXPECT_EQ(u.run(), Status::Exact);
    EXPECT_FALSE(hint.verified());
    EXPECT_EQ(hint.forkPoint(), 300u);
    EXPECT_EQ(hint.resultPeer(), key(2));
    EXPECT_EQ(u.asked(), (std::set<uint8_t>{2, 3}));
}

// the candidates: neighbours only; first the peers likely on the competing branch (official version,
// a height above ours), then those that sent a refused block
TEST(ForkLocator, PrefersPeersOnTheCompetingBranch) {
    using cs::forkguard::CandidatePeer;
    std::vector<CandidatePeer> peers;
    peers.push_back({key(3), 637, false, true, true});    // community, relayed the refused block
    peers.push_back({key(7), 637, false, false, true});   // community, same height
    peers.push_back({key(2), 1480, true, false, true});   // official, far ahead
    peers.push_back({key(4), 1490, true, false, true});   // official, further ahead
    peers.push_back({key(9), 900, false, false, true});   // community peer announcing more blocks
    peers.push_back({key(6), 2000, true, true, false});   // not a neighbour
    const auto ranked = cs::forkguard::rankCandidates(peers, 637, 5);
    ASSERT_EQ(ranked.size(), 5u);
    EXPECT_EQ(ranked[0], key(4));
    EXPECT_EQ(ranked[1], key(2));
    EXPECT_EQ(ranked[2], key(9));
    EXPECT_EQ(ranked[3], key(3));
    EXPECT_EQ(ranked[4], key(7));
    EXPECT_EQ(cs::forkguard::rankCandidates(peers, 637, 2).size(), 2u);
    EXPECT_TRUE(cs::forkguard::rankCandidates({}, 637, 5).empty());
}

// the record of a located fork point and what --resolve-fork does with it: a verified point is the
// fork point and the cut goes there; an unverified one is a hint, the cut stays at the upper bound
// unless the operator asks for it (--to-located); "deeper" is a hint as well
TEST(ForkLocator, LocatedRecordAndResolvePlan) {
    TempFolder dir("record");
    std::string error;

    auto r = heldRecord();
    r.located = true;
    r.locatedVerified = true;
    r.upperBound = 930;
    r.locatedWith = "key";
    r.locatedPoint = 533;
    r.locatedHash = "C533";
    r.forkPoint = 533;
    r.forkPointHash = "C533";
    r.forkPointExact = true;
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), r));
    {
        std::ifstream in(dir.path / cs::forkguard::kForkFile);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("\"fork_point_status\": \"exact\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"fork_point_upper_bound\": \"930\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"located_verified\": \"true\""), std::string::npos) << text;
    }
    auto back = cs::forkguard::loadForkRecord(dir.str());
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->located);
    EXPECT_TRUE(back->locatedVerified);
    EXPECT_TRUE(back->forkPointExact);
    EXPECT_FALSE(back->deeper);
    EXPECT_EQ(back->upperBound, 930u);
    EXPECT_EQ(back->locatedPoint, 533u);
    EXPECT_EQ(back->locatedWith, "key");
    auto plan = cs::forkguard::planResolve(dir.str(), &error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->top, 533u);  // a verified point: cut exactly there
    EXPECT_EQ(plan->hash, "C533");
    auto overview = cs::forkguard::resolveOverview(*plan);
    EXPECT_NE(overview.find("our head #931"), std::string::npos) << overview;
    EXPECT_NE(overview.find("upper bound #930"), std::string::npos) << overview;
    EXPECT_NE(overview.find("located #533 (verified)"), std::string::npos) << overview;
    EXPECT_NE(overview.find("Cutting at #533: 398 block(s) removed"), std::string::npos) << overview;
    auto line = cs::forkguard::summary(*back);
    EXPECT_NE(line.find("Fork point: block #533 (C533), the last common block, verified (located from #930)"), std::string::npos) << line;
    EXPECT_NE(line.find("--set-bc-top 533"), std::string::npos) << line;

    // not verified: the fork point stays the upper bound; the located point is a hint
    auto hint = heldRecord();
    hint.located = true;
    hint.upperBound = 930;
    hint.locatedWith = "key";
    hint.locatedPoint = 300;
    hint.locatedHash = "C300";
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), hint));
    back = cs::forkguard::loadForkRecord(dir.str());
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(back->locatedVerified);
    EXPECT_FALSE(back->forkPointExact);
    EXPECT_EQ(back->forkPointStatus(), "located (unverified)");
    plan = cs::forkguard::planResolve(dir.str(), &error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->top, 930u);  // no deeper cut without the operator's confirmation
    EXPECT_EQ(plan->hash, "C930");
    EXPECT_FALSE(plan->toLocated);
    overview = cs::forkguard::resolveOverview(*plan);
    EXPECT_NE(overview.find("located #300 (NOT verified)"), std::string::npos) << overview;
    EXPECT_NE(overview.find("Cutting at #930: 1 block(s) removed"), std::string::npos) << overview;
    plan = cs::forkguard::planResolve(dir.str(), &error, true);  // --to-located
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->top, 300u);
    EXPECT_EQ(plan->hash, "C300");
    EXPECT_TRUE(plan->toLocated);
    overview = cs::forkguard::resolveOverview(*plan);
    EXPECT_NE(overview.find("Cutting at #300 (--to-located): 631 block(s) removed"), std::string::npos) << overview;
    line = cs::forkguard::summary(*back);
    EXPECT_NE(line.find("Fork point: block #930 or lower (C930)"), std::string::npos) << line;
    EXPECT_NE(line.find("NOT verified"), std::string::npos) << line;
    EXPECT_NE(line.find("--to-located at #300"), std::string::npos) << line;

    // deeper: a hint as well; --resolve-fork cuts at the upper bound, --to-located has no point
    auto deep = heldRecord();
    deep.located = true;
    deep.deeper = true;
    deep.upperBound = 930;
    deep.deeperThan = 100;
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), deep));
    back = cs::forkguard::loadForkRecord(dir.str());
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->deeper);
    EXPECT_EQ(back->deeperThan, 100u);
    EXPECT_EQ(back->forkPointStatus(), "deeper");
    plan = cs::forkguard::planResolve(dir.str(), &error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->top, 930u);
    EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error, true).has_value());
    EXPECT_NE(error.find("--to-located"), std::string::npos) << error;
    line = cs::forkguard::summary(*back);
    EXPECT_NE(line.find("differs already at #100 (not verified)"), std::string::npos) << line;

    // --to-located without any located point
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), heldRecord()));
    EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error, true).has_value());
}

namespace {
// a BlockChain holding a fork after a depth refusal at 4900 (reference 5000), fork point an upper
// bound, with its last block (the only one the test chain can name) at 4800
struct HeldChain {
    TempFolder dir{"chain"};
    BlockChain chain{csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001"),
                     csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002")};
    cs::forkguard::ForkRecord held;
    std::string hash;
    HeldChain() {
        cs::ConfigHolder::instance().setConfig(Config{});
        chain.test_resetRollbackEpisode();
        chain.setGuardStateDir(dir.str());
        chain.test_setLastSequence(5000);
        EXPECT_TRUE(chain.guardAutoRollback());
        chain.test_setLastSequence(4900);
        EXPECT_FALSE(chain.guardAutoRollback());
        held = *chain.forkHeldRecord();
        csdb::Pool block(csdb::PoolHash{}, 4800);
        block.add_user_field(200, std::string("located"));
        EXPECT_TRUE(block.compose());
        chain.test_setLastBlock(block);
        hash = block.hash().to_string();
    }
    ~HeldChain() {
        chain.test_resetRollbackEpisode();
        cs::forkguard::clearAll(dir.str());
    }
};
}  // namespace

// the node puts the answer in the held record only if it is still the record searched for and our
// block at the point is still the one compared; a verified answer becomes the fork point, an
// unverified one stays a hint; nothing else changes
TEST(ForkLocator, BlockChainTakesTheAnswer) {
    {
        HeldChain h;
        auto& chain = h.chain;
        ASSERT_FALSE(h.held.forkPointExact);
        // another record than the one searched for: ignored
        auto other = h.held;
        other.sinceUnix += 1;
        EXPECT_FALSE(chain.applyLocatedForkPoint(other, 4800, h.hash, false, "peer", true));
        // our block there is not the one compared: ignored
        EXPECT_FALSE(chain.applyLocatedForkPoint(h.held, 4800, "00", false, "peer", true));
        EXPECT_FALSE(chain.forkHeldRecord()->located);

        EXPECT_TRUE(chain.applyLocatedForkPoint(h.held, 4800, h.hash, false, "peer", true, "NEXT"));
        const auto located = chain.forkHeldRecord();
        ASSERT_TRUE(located.has_value());
        EXPECT_TRUE(located->held);
        EXPECT_TRUE(located->located);
        EXPECT_TRUE(located->locatedVerified);
        EXPECT_TRUE(located->forkPointExact);
        EXPECT_EQ(located->forkPoint, 4800u);
        EXPECT_EQ(located->forkPointHash, h.hash);
        EXPECT_EQ(located->upperBound, h.held.forkPoint);
        EXPECT_EQ(located->competingSeq, h.held.competingSeq);  // the refused block stays the one refused
        EXPECT_EQ(located->locatedNextHash, "NEXT");             // the block --resolve-fork will prefer
        EXPECT_EQ(cs::forkguard::loadForkRecord(h.dir.str())->locatedNextHash, "NEXT");
        EXPECT_EQ(chain.getLastSeq(), 4800u);                    // nothing unwound
        EXPECT_FALSE(chain.approvedBranch().has_value());       // nothing approved
        const auto plan = cs::forkguard::planResolve(h.dir.str(), nullptr);
        ASSERT_TRUE(plan.has_value());
        EXPECT_EQ(plan->top, 4800u);  // the verified point cuts exactly
        // once located, a second answer changes nothing
        EXPECT_FALSE(chain.applyLocatedForkPoint(h.held, 4800, h.hash, true, "peer"));
    }
    {
        // not verified (a lying peer's deeper point, a bootstrap block after it): a hint only
        HeldChain h;
        auto& chain = h.chain;
        EXPECT_TRUE(chain.applyLocatedForkPoint(h.held, 4800, h.hash, false, "peer", false, "NEXT"));
        const auto hint = chain.forkHeldRecord();
        ASSERT_TRUE(hint.has_value());
        EXPECT_TRUE(hint->located);
        EXPECT_FALSE(hint->locatedVerified);
        EXPECT_FALSE(hint->forkPointExact);
        EXPECT_EQ(hint->forkPoint, h.held.forkPoint);  // still the upper bound
        EXPECT_EQ(hint->locatedPoint, 4800u);
        EXPECT_TRUE(hint->locatedNextHash.empty());  // nothing to prefer from an unverified answer
        std::string error;
        // no deeper cut without confirmation: the recorded upper bound, or nothing (this test chain
        // has no hash for it)
        auto plan = cs::forkguard::planResolve(h.dir.str(), &error);
        if (plan.has_value()) {
            EXPECT_EQ(plan->top, h.held.forkPoint);
        }
        plan = cs::forkguard::planResolve(h.dir.str(), &error, true);
        ASSERT_TRUE(plan.has_value()) << error;
        EXPECT_EQ(plan->top, 4800u);
    }
}

namespace {
void writeHead(const std::filesystem::path& dir, cs::Sequence seq) {
    std::filesystem::create_directories(dir);
    std::ofstream f(dir / "head.bin", std::ios::binary);
    const uint32_t magic = 0x44414548u;
    f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    const uint64_t s = seq;
    f.write(reinterpret_cast<const char*>(&s), sizeof(s));
    const cs::Bytes hash(32, 0xab);
    const uint16_t hh = static_cast<uint16_t>(hash.size());
    f.write(reinterpret_cast<const char*>(&hh), sizeof(hh));
    f.write(reinterpret_cast<const char*>(hash.data()), hh);
    const uint16_t ph = 0;
    f.write(reinterpret_cast<const char*>(&ph), sizeof(ph));
}
}  // namespace

// after a cut at 950: every checkpoint above it is set aside as <name>.bad (the shutdown checkpoint
// qs/0 at the old head first), with its .prev backup; those at or below it stay
TEST(ForkLocator, ChainCutDropsCheckpointsAboveTheTop) {
    TempFolder qs("qs");
    writeHead(qs.path / "0", 980);
    writeHead(qs.path / "900", 900);
    writeHead(qs.path / "960", 960);
    writeHead(qs.path / "960.prev", 960);
    writeHead(qs.path / "0.bad", 980);
    std::filesystem::create_directories(qs.path / "500");   // legacy, no head.bin: named by its block
    std::filesystem::create_directories(qs.path / "2000");
    std::ofstream(qs.path / "notes.txt") << "not a checkpoint";

    EXPECT_TRUE(cs::hasCheckpointAtOrBelow(qs.path, 950));
    EXPECT_FALSE(cs::hasCheckpointAtOrBelow(qs.path, 400));  // 500 and 900 continue above it
    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path, 950), 4u);
    for (const char* gone : {"0", "960", "960.prev", "2000"}) {
        EXPECT_FALSE(std::filesystem::exists(qs.path / gone)) << gone;
    }
    // set aside, not deleted (an older 0.bad is replaced)
    for (const char* aside : {"0.bad", "960.bad", "960.prev.bad", "2000.bad"}) {
        EXPECT_TRUE(std::filesystem::exists(qs.path / aside)) << aside;
    }
    EXPECT_EQ(cs::readCheckpointHead(qs.path / "0.bad")->sequence, 980u);
    for (const char* kept : {"900", "500", "notes.txt"}) {
        EXPECT_TRUE(std::filesystem::exists(qs.path / kept)) << kept;
    }
    // set-aside checkpoints never count: a cut at 400 would leave a pruned node no checkpoint
    EXPECT_FALSE(cs::hasCheckpointAtOrBelow(qs.path, 400));
    // a cut above every checkpoint removes nothing; a missing folder is no error
    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path, 5000), 0u);
    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path / "none", 10), 0u);
    // a shutdown checkpoint at the new top itself stays
    writeHead(qs.path / "0", 950);
    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path, 950), 0u);
    EXPECT_TRUE(std::filesystem::exists(qs.path / "0"));
}
