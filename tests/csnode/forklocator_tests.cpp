#define TESTING

#include "gtest/gtest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
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
    Locator::PeerBlock peerBlock(cs::Sequence s) const {
        return {s, theirs(s), s == 0 ? std::string{} : theirs(s - 1)};
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
            loc.onRefused(key(7), h.now, true);
            h.now += 7s;
        }
        EXPECT_TRUE(loc.searching());
        for (int i = 0; i < 200 && loc.searching(); ++i) {
            loc.tick(h.now);
            loc.onRefused(key(7), h.now, true);
            h.now += 7s;
        }
        EXPECT_EQ(loc.status(), Status::Failed);
        EXPECT_NE(loc.why().find("duplicates"), std::string::npos) << loc.why();
    }
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

// the record of a located fork point: exact (and --resolve-fork cuts there), or deeper (and
// --resolve-fork removes nothing); both in fork-held.json and the detail line
TEST(ForkLocator, LocatedRecordAndResolvePlan) {
    TempFolder dir("record");
    auto r = heldRecord();
    r.located = true;
    r.upperBound = 930;
    r.locatedWith = "key";
    r.forkPoint = 533;
    r.forkPointHash = "C533";
    r.forkPointExact = true;
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), r));
    {
        std::ifstream in(dir.path / cs::forkguard::kForkFile);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("\"fork_point_status\": \"exact\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"fork_point_upper_bound\": \"930\""), std::string::npos) << text;
    }
    auto back = cs::forkguard::loadForkRecord(dir.str());
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->located);
    EXPECT_TRUE(back->forkPointExact);
    EXPECT_FALSE(back->deeper);
    EXPECT_EQ(back->upperBound, 930u);
    EXPECT_EQ(back->locatedWith, "key");
    std::string error;
    const auto plan = cs::forkguard::planResolve(dir.str(), &error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->top, 533u);
    EXPECT_EQ(plan->hash, "C533");
    const auto line = cs::forkguard::summary(*back);
    EXPECT_NE(line.find("Fork point: block #533 (C533), the last common block, located from #930"), std::string::npos) << line;
    EXPECT_NE(line.find("--set-bc-top 533"), std::string::npos) << line;

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
    EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error).has_value());
    EXPECT_NE(error.find("below block #100"), std::string::npos) << error;
    EXPECT_NE(error.find("--set-bc-top"), std::string::npos) << error;
    const auto deepLine = cs::forkguard::summary(*back);
    EXPECT_NE(deepLine.find("Fork point: below block #100"), std::string::npos) << deepLine;
}

// the node puts the answer in the held record only if it is still the record searched for and our
// block at the point is still the one compared; nothing else changes
TEST(ForkLocator, BlockChainTakesTheAnswer) {
    cs::ConfigHolder::instance().setConfig(Config{});
    TempFolder dir("chain");
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();
    chain.setGuardStateDir(dir.str());

    // a depth refusal at 4900 (reference 5000): FORK HELD, fork point an upper bound
    chain.test_setLastSequence(5000);
    ASSERT_TRUE(chain.guardAutoRollback());
    chain.test_setLastSequence(4900);
    ASSERT_FALSE(chain.guardAutoRollback());
    const auto held = chain.forkHeldRecord();
    ASSERT_TRUE(held.has_value());
    ASSERT_FALSE(held->forkPointExact);

    // our block at the located point (the last block here, the only one this test chain can name)
    csdb::Pool block(csdb::PoolHash{}, 4800);
    block.add_user_field(200, std::string("located"));
    ASSERT_TRUE(block.compose());
    chain.test_setLastBlock(block);
    const std::string hash = block.hash().to_string();

    // another record than the one searched for: ignored
    auto other = *held;
    other.sinceUnix += 1;
    EXPECT_FALSE(chain.applyLocatedForkPoint(other, 4800, hash, false, "peer"));
    // our block there is not the one compared: ignored
    EXPECT_FALSE(chain.applyLocatedForkPoint(*held, 4800, "00", false, "peer"));
    EXPECT_FALSE(chain.forkHeldRecord()->located);

    EXPECT_TRUE(chain.applyLocatedForkPoint(*held, 4800, hash, false, "peer"));
    const auto located = chain.forkHeldRecord();
    ASSERT_TRUE(located.has_value());
    EXPECT_TRUE(located->held);
    EXPECT_TRUE(located->located);
    EXPECT_TRUE(located->forkPointExact);
    EXPECT_EQ(located->forkPoint, 4800u);
    EXPECT_EQ(located->forkPointHash, hash);
    EXPECT_EQ(located->upperBound, held->forkPoint);
    EXPECT_EQ(located->competingSeq, held->competingSeq);  // the refused block stays the one refused
    EXPECT_EQ(chain.getLastSeq(), 4800u);                  // nothing unwound
    EXPECT_FALSE(chain.approvedBranch().has_value());     // nothing approved
    const auto onDisk = cs::forkguard::loadForkRecord(dir.str());
    ASSERT_TRUE(onDisk.has_value());
    EXPECT_EQ(onDisk->forkPoint, 4800u);
    EXPECT_TRUE(onDisk->located);
    // once located, a second answer changes nothing
    EXPECT_FALSE(chain.applyLocatedForkPoint(*held, 4800, hash, true, "peer"));

    chain.test_resetRollbackEpisode();
    cs::forkguard::clearAll(dir.str());
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

// after a cut at 950: every checkpoint above it goes (the shutdown checkpoint qs/0 at the old head
// first), with its .prev backup; those at or below it stay, and so do quarantined ones
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

    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path, 950), 4u);
    for (const char* gone : {"0", "960", "960.prev", "2000"}) {
        EXPECT_FALSE(std::filesystem::exists(qs.path / gone)) << gone;
    }
    for (const char* kept : {"900", "500", "0.bad", "notes.txt"}) {
        EXPECT_TRUE(std::filesystem::exists(qs.path / kept)) << kept;
    }
    // a cut above every checkpoint removes nothing; a missing folder is no error
    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path, 5000), 0u);
    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path / "none", 10), 0u);
    // a shutdown checkpoint at the new top itself stays
    writeHead(qs.path / "0", 950);
    EXPECT_EQ(cs::dropCheckpointsAbove(qs.path, 950), 0u);
    EXPECT_TRUE(std::filesystem::exists(qs.path / "0"));
}
