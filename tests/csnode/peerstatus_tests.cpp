#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <base58.h>

#include <csnode/forkguardstate.hpp>
#include <csnode/peerstatus.hpp>

using namespace cs::peerstatus;

namespace {

cs::PublicKey key(uint8_t n) {
    cs::PublicKey k{};
    k.fill(0);
    k[0] = n;
    return k;
}

// keys 1-9 official, 11-19 community, 21-29 unknown (not in the map); 100 is this node
void setup(Tracker& t) {
    t.setSelf(key(100), Kind::Community);
    std::map<cs::PublicKey, Kind> kinds;
    for (uint8_t i = 1; i < 10; ++i) {
        kinds[key(i)] = Kind::Official;
        kinds[key(10 + i)] = Kind::Community;
    }
    t.setKinds(kinds);
}

NeighbourCounts neighbours(size_t official, size_t community, size_t unknown = 0) {
    return NeighbourCounts{official + community + unknown, official, community, unknown};
}

bool hasLine(const std::vector<Line>& lines, const std::string& prefix) {
    for (const auto& l : lines) {
        if (l.text.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(PeerStatus, ClassifiesAnnouncedVersions) {
    EXPECT_EQ(classify(0, 0), Kind::Unknown);     // nothing announced yet
    EXPECT_EQ(classify(532, 0), Kind::Official);  // official 4.2.532
    EXPECT_EQ(classify(532, 15), Kind::Community);
    EXPECT_STREQ(kindName(Kind::Official), "official");
    EXPECT_STREQ(kindName(Kind::Community), "community");
    EXPECT_STREQ(kindName(Kind::Unknown), "unknown");
}

TEST(PeerStatus, Percent) {
    EXPECT_EQ(percent(0, 0), 0u);
    EXPECT_EQ(percent(1, 3), 33u);
    EXPECT_EQ(percent(2, 3), 67u);
    EXPECT_EQ(percent(3, 4), 75u);
    EXPECT_EQ(percent(5, 5), 100u);
}

TEST(PeerStatus, SeatsCountOnlyRealTrustedAndClassify) {
    Tracker t;
    setup(t);
    // confidants official 1, community 11, unknown 21, self; only bits 0, 1 and 3 signed
    t.addBlock(10, {key(1), key(11), key(21), key(100)}, 0b1011);
    const auto c = t.seats();
    EXPECT_EQ(c.rounds, 1u);
    EXPECT_EQ(c.seats, 3u);
    EXPECT_EQ(c.official, 1u);
    EXPECT_EQ(c.community, 2u);  // 11 and this node
    EXPECT_EQ(c.unknown, 0u);
    EXPECT_EQ(c.selfTrusted, 1u);
    t.addBlock(11, {key(1), key(21), key(100)}, 0b011);
    const auto d = t.seats();
    EXPECT_EQ(d.rounds, 2u);
    EXPECT_EQ(d.unknown, 1u);
    EXPECT_EQ(d.selfTrusted, 1u);
    EXPECT_EQ(d.first, 10u);
    EXPECT_EQ(d.last, 11u);
    EXPECT_EQ(t.seats(1).rounds, 1u);
    EXPECT_EQ(t.seats(1).first, 11u);
}

TEST(PeerStatus, SlidingWindowAndRollback) {
    Settings s;
    s.window = 5;
    Tracker t(s);
    setup(t);
    for (cs::Sequence q = 1; q <= 8; ++q) {
        t.addBlock(q, {key(1)}, 1);
    }
    auto c = t.seats();
    EXPECT_EQ(c.rounds, 5u);
    EXPECT_EQ(c.first, 4u);
    EXPECT_EQ(c.last, 8u);
    t.removeBlock(7);  // 7 and 8 removed
    EXPECT_EQ(t.seats().last, 6u);
    t.addBlock(5, {key(11)}, 1);  // stored again after a rollback: replaces 5 and 6
    c = t.seats();
    EXPECT_EQ(c.last, 5u);
    EXPECT_EQ(c.rounds, 2u);
    EXPECT_EQ(c.community, 1u);
}

TEST(PeerStatus, KindsLearnedLaterReclassifyTheWindow) {
    Tracker t;
    t.setSelf(key(100), Kind::Community);
    t.addBlock(1, {key(1), key(11)}, 0b11);
    EXPECT_EQ(t.seats().unknown, 2u);
    t.setKinds({{key(1), Kind::Official}, {key(11), Kind::Community}});
    EXPECT_EQ(t.seats().official, 1u);
    EXPECT_EQ(t.seats().community, 1u);
    EXPECT_EQ(t.kindOf(key(1)), Kind::Official);
    EXPECT_EQ(t.kindOf(key(100)), Kind::Community);
    EXPECT_EQ(t.kindOf(key(50)), Kind::Unknown);
}

TEST(PeerStatus, NeighbourCounts) {
    Tracker t;
    setup(t);
    const auto n = t.countNeighbours({key(1), key(2), key(11), key(25)});
    EXPECT_EQ(n.total, 4u);
    EXPECT_EQ(n.official, 2u);
    EXPECT_EQ(n.community, 1u);
    EXPECT_EQ(n.unknown, 1u);
}

TEST(PeerStatus, PeersLine) {
    Tracker t;
    setup(t);
    EXPECT_EQ(t.peersLine(neighbours(11, 5)), "PEERS: 16 neighbours (11 official, 5 community); no stored blocks yet");
    // 4 rounds, 4 seats each: 3 official + 1 community, this node in 2 of them
    for (cs::Sequence q = 1; q <= 4; ++q) {
        if (q % 2 == 0) {
            t.addBlock(q, {key(1), key(2), key(3), key(100)}, 0xF);
        }
        else {
            t.addBlock(q, {key(1), key(2), key(3), key(11)}, 0xF);
        }
    }
    EXPECT_EQ(t.peersLine(neighbours(11, 5)),
              "PEERS: 16 neighbours (11 official, 5 community); last 4 rounds: trusted seats 75% official, 25% community; "
              "this node trusted in 50%");
    t.addBlock(5, {key(21), key(22), key(23), key(24)}, 0xF);
    EXPECT_EQ(t.peersLine(neighbours(1, 1, 2)),
              "PEERS: 4 neighbours (1 official, 1 community, 2 unknown); last 5 rounds: trusted seats 60% official, 20% "
              "community, 20% unknown; this node trusted in 40%");
}

TEST(PeerStatus, PeersLineAtStartThenEveryTenMinutes) {
    Tracker t;
    setup(t);
    auto lines = t.tick(1000, neighbours(3, 1));
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(hasLine(lines, "PEERS: 4 neighbours"));
    EXPECT_FALSE(lines[0].warning);
    EXPECT_TRUE(t.tick(1030, neighbours(3, 1)).empty());
    EXPECT_TRUE(t.tick(1599, neighbours(3, 1)).empty());
    EXPECT_TRUE(hasLine(t.tick(1600, neighbours(3, 1)), "PEERS:"));
}

TEST(PeerStatus, GoneByNeighboursThrottledAndBack) {
    Tracker t;
    setup(t);
    t.tick(0, neighbours(2, 2));  // the last official neighbour
    EXPECT_FALSE(t.officialGone());
    EXPECT_FALSE(hasLine(t.tick(300, neighbours(0, 4)), "OFFICIAL"));  // 5 minutes only
    EXPECT_FALSE(hasLine(t.tick(599, neighbours(0, 4)), "OFFICIAL"));
    EXPECT_FALSE(t.officialGone());
    auto lines = t.tick(600, neighbours(0, 4));  // 10 minutes without an official neighbour
    EXPECT_TRUE(t.officialGone());
    ASSERT_TRUE(hasLine(lines, "OFFICIAL NODES GONE: no official neighbour for 10 min"));
    for (const auto& l : lines) {
        if (l.text.rfind("OFFICIAL", 0) == 0) {
            EXPECT_TRUE(l.warning);
            EXPECT_NE(l.text.find("community nodes keep the network going: 4 community of 4 neighbours"), std::string::npos);
        }
    }
    // held: nothing for 10 minutes, then a reminder
    EXPECT_FALSE(hasLine(t.tick(930, neighbours(0, 4)), "OFFICIAL"));
    EXPECT_FALSE(hasLine(t.tick(1199, neighbours(0, 4)), "OFFICIAL"));
    EXPECT_TRUE(hasLine(t.tick(1200, neighbours(0, 4)), "OFFICIAL NODES GONE (still, for 10 min)"));
    // back
    lines = t.tick(1230, neighbours(1, 3));
    EXPECT_FALSE(t.officialGone());
    EXPECT_TRUE(hasLine(lines, "OFFICIAL NODES BACK after 10 min: 1 official of 4 neighbours"));
    EXPECT_TRUE(t.tick(1260, neighbours(1, 3)).empty());
}

TEST(PeerStatus, NeverSeenSinceTheStart) {
    Tracker t;
    setup(t);
    t.tick(0, neighbours(0, 6));
    const auto lines = t.tick(600, neighbours(0, 6));
    EXPECT_TRUE(hasLine(lines, "OFFICIAL NODES GONE: no official neighbour seen since the start (10 min)"));
}

TEST(PeerStatus, FlappingIsThrottled) {
    Settings s;
    s.goneRounds = 2;
    Tracker t(s);
    setup(t);
    t.tick(0, neighbours(2, 2));  // official neighbours all along: only the seats change
    t.addBlock(1, {key(11)}, 1);
    t.addBlock(2, {key(12)}, 1);
    EXPECT_TRUE(hasLine(t.tick(10, neighbours(2, 2)), "OFFICIAL NODES GONE"));
    t.addBlock(3, {key(1)}, 1);
    EXPECT_TRUE(hasLine(t.tick(20, neighbours(2, 2)), "OFFICIAL NODES BACK"));
    // gone again within 10 minutes of the last GONE alert: the state changes, the line waits
    t.addBlock(4, {key(11)}, 1);
    t.addBlock(5, {key(12)}, 1);
    EXPECT_FALSE(hasLine(t.tick(30, neighbours(2, 2)), "OFFICIAL"));
    EXPECT_TRUE(t.officialGone());
    // and back: no BACK line for a GONE that was not said
    t.addBlock(6, {key(1)}, 1);
    EXPECT_FALSE(hasLine(t.tick(40, neighbours(2, 2)), "OFFICIAL"));
    EXPECT_FALSE(t.officialGone());
    // gone again 10 minutes after the last alert: said again
    t.addBlock(7, {key(11)}, 1);
    t.addBlock(8, {key(12)}, 1);
    EXPECT_TRUE(hasLine(t.tick(610, neighbours(2, 2)), "OFFICIAL NODES GONE: no official node among the trusted of the last 2 rounds"));
}

TEST(PeerStatus, GoneBySeats) {
    Settings s;
    s.goneRounds = 10;
    s.communityOnlyRounds = 20;
    Tracker t(s);
    setup(t);
    t.tick(0, neighbours(3, 3));
    for (cs::Sequence q = 1; q <= 9; ++q) {
        t.addBlock(q, {key(11), key(12), key(100)}, 0b111);
    }
    EXPECT_FALSE(hasLine(t.tick(30, neighbours(3, 3)), "OFFICIAL"));  // 9 rounds only
    t.addBlock(10, {key(11), key(12), key(100)}, 0b111);
    const auto lines = t.tick(60, neighbours(3, 3));
    EXPECT_TRUE(t.officialGone());
    EXPECT_TRUE(hasLine(lines, "OFFICIAL NODES GONE: no official node among the trusted of the last 10 rounds"));
    // one official seat in the last 10 rounds: back
    t.addBlock(11, {key(1), key(12), key(100)}, 0b111);
    EXPECT_TRUE(hasLine(t.tick(90, neighbours(3, 3)), "OFFICIAL NODES BACK"));
    EXPECT_FALSE(t.officialGone());
}

TEST(PeerStatus, UnknownSeatsDoNotRaiseGone) {
    Settings s;
    s.goneRounds = 4;
    Tracker t(s);
    setup(t);
    t.tick(0, neighbours(1, 1));
    for (cs::Sequence q = 1; q <= 4; ++q) {
        t.addBlock(q, {key(21), key(22), key(23), key(11)}, 0xF);  // 3 of 4 unknown
    }
    t.tick(30, neighbours(1, 1));
    EXPECT_FALSE(t.officialGone());
}

TEST(PeerStatus, CommunityOnlyWarning) {
    Settings s;
    s.goneRounds = 1000;  // only the community-only state here
    s.communityOnlyRounds = 5;
    Tracker t(s);
    setup(t);
    t.tick(0, neighbours(1, 1));
    for (cs::Sequence q = 1; q <= 5; ++q) {
        t.addBlock(q, {key(11), key(100)}, 0b11);
    }
    auto lines = t.tick(30, neighbours(1, 1));
    EXPECT_TRUE(t.communityOnly());
    ASSERT_TRUE(hasLine(lines, "TRUSTED SEATS COMMUNITY-ONLY: every trusted seat of the last 5 rounds (blocks #1-#5)"));
    EXPECT_FALSE(hasLine(t.tick(60, neighbours(1, 1)), "TRUSTED SEATS"));
    EXPECT_TRUE(hasLine(t.tick(630, neighbours(1, 1)), "TRUSTED SEATS COMMUNITY-ONLY"));
    t.addBlock(6, {key(1), key(100)}, 0b11);
    EXPECT_TRUE(hasLine(t.tick(660, neighbours(1, 1)), "TRUSTED SEATS MIXED again"));
    EXPECT_FALSE(t.communityOnly());
}

TEST(PeerStatus, KindByBase58Key) {
    Tracker t;
    setup(t);
    const auto k = key(1);
    std::string name = EncodeBase58(k.data(), k.data() + k.size());
    EXPECT_EQ(t.kindOfBase58(name), Kind::Official);
    EXPECT_EQ(t.kindOfBase58("nope"), Kind::Unknown);
}

TEST(PeerStatus, StatusFileIsWrittenAtomically) {
    Tracker t;
    setup(t);
    t.addBlock(1, {key(1), key(11)}, 0b11);
    t.tick(0, neighbours(0, 2));
    t.tick(600, neighbours(0, 2));
    const auto dir = std::filesystem::temp_directory_path() / "peerstatus_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ASSERT_TRUE(cs::forkguard::writeFileAtomic(dir.string(), kStatusFile, t.statusJson(1795000000)));
    EXPECT_FALSE(std::filesystem::exists(dir / (std::string(kStatusFile) + ".tmp")));
    std::string text;
    {
        // closed before the folder is removed (Windows keeps an open file)
        std::ifstream in(dir / kStatusFile);
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    EXPECT_NE(text.find("\"official_gone\": \"true\""), std::string::npos);
    EXPECT_NE(text.find("\"summary\": \"PEERS: 2 neighbours (0 official, 2 community)"), std::string::npos);
    EXPECT_NE(text.find("\"seats_official_pct\": \"50\""), std::string::npos);
    EXPECT_NE(text.find("\"last_alert\": \"OFFICIAL NODES GONE"), std::string::npos);
    EXPECT_FALSE(cs::forkguard::writeFileAtomic("", kStatusFile, "x"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(PeerStatus, ForkHeldSummaryNamesTheOtherSide) {
    cs::forkguard::ForkRecord r;
    r.forkPoint = 10;
    r.ourHead = 12;
    r.peerKey = "abc";
    EXPECT_EQ(cs::forkguard::summary(r).find("the other side is"), std::string::npos);
    EXPECT_NE(cs::forkguard::summary(r, "an official node (4.2.532)").find("from abc; the other side is an official node (4.2.532)."),
              std::string::npos);
}
