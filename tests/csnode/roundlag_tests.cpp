#include <gtest/gtest.h>

#include <csnode/roundlag.hpp>

using cs::roundlag::Monitor;
using cs::roundlag::Settings;

namespace {
// one confirmed lag episode a minute from `from`, at rounds from `round`, ticking every 5 s;
// returns the resets
size_t lagFor(Monitor& m, int64_t from, int64_t seconds, uint64_t round, std::vector<std::string>* lines = nullptr,
              bool confirmed = true) {
    size_t resets = 0;
    for (int64_t t = from; t < from + seconds; t += 5) {
        if ((t - from) % 60 == 0) {
            m.onLag(t, round++, "peer-a", confirmed);
        }
        const auto tick = m.tick(t);
        if (tick.reset) {
            ++resets;
        }
        if (lines != nullptr && !tick.text.empty()) {
            lines->push_back(tick.text);
        }
    }
    return resets;
}
}  // namespace

TEST(RoundLag, QuorumAhead) {
    size_t ahead = 0;
    // 16 neighbours, 9 two blocks ahead of 100: a majority, at least 3
    std::vector<uint64_t> seqs(16, 100);
    for (size_t i = 0; i < 9; ++i) {
        seqs[i] = 102;
    }
    EXPECT_TRUE(cs::roundlag::quorumAhead(100, seqs, &ahead));
    EXPECT_EQ(ahead, 9u);
    // one block ahead is the normal state of a node during a round
    EXPECT_FALSE(cs::roundlag::quorumAhead(101, seqs));
    // half is not a majority
    seqs[8] = 100;
    EXPECT_FALSE(cs::roundlag::quorumAhead(100, seqs, &ahead));
    EXPECT_EQ(ahead, 8u);
    // two neighbours are never a quorum, even all of them
    EXPECT_FALSE(cs::roundlag::quorumAhead(100, {500, 500}));
    EXPECT_TRUE(cs::roundlag::quorumAhead(100, {500, 500, 500}));
    EXPECT_FALSE(cs::roundlag::quorumAhead(100, {}));
}

TEST(RoundLag, OneEpisodePerLocalRound) {
    Monitor m;
    EXPECT_TRUE(m.onLag(1000, 50, "a", true));
    EXPECT_FALSE(m.onLag(1000, 50, "b", true));
    EXPECT_FALSE(m.onLag(1001, 50, "a", false));
    EXPECT_TRUE(m.onLag(1002, 51, "a", true));
    EXPECT_EQ(m.episodesInWindow(1002), 2u);
}

TEST(RoundLag, UnconfirmedEpisodesDoNotCount) {
    Monitor m;
    m.onLag(1000, 50, "a", false);
    m.onLag(1001, 51, "a", false);
    EXPECT_EQ(m.episodesInWindow(1001), 0u);
    EXPECT_EQ(m.seenInWindow(1001), 2u);
    // confirmed later in the same round (a quorum ahead by then): it counts
    m.onLag(1002, 51, "b", true);
    EXPECT_EQ(m.episodesInWindow(1002), 1u);
}

TEST(RoundLag, InflatedRoundsFromOnePeerNeverReset) {
    // one peer sends messages with high round numbers in every round, for hours; the neighbours do
    // not announce blocks ahead of ours (no quorum): no lag, no warning, no reset
    Monitor m;
    std::vector<std::string> lines;
    uint64_t round = 1000;
    size_t resets = 0;
    for (int64_t t = 1000; t < 1000 + 6 * 3600; t += 1) {
        m.onLag(t, round++, "attacker", false);
        if (t % 5 == 0) {
            const auto tick = m.tick(t);
            resets += tick.reset ? 1 : 0;
            if (!tick.text.empty()) {
                lines.push_back(tick.text);
            }
        }
    }
    EXPECT_EQ(resets, 0u);
    EXPECT_FALSE(m.lagging());
    EXPECT_FALSE(m.persistent());
    EXPECT_TRUE(lines.empty());
    EXPECT_NE(m.statusJson(1764000000, 1000 + 6 * 3600).find("\"persistent\": \"false\""), std::string::npos);
}

TEST(RoundLag, EpisodesLeaveTheWindow) {
    Monitor m;
    for (uint64_t r = 0; r < 4; ++r) {
        m.onLag(1000 + static_cast<int64_t>(r), 100 + r, "a", true);
    }
    EXPECT_EQ(m.episodesInWindow(1100), 4u);
    EXPECT_EQ(m.episodesInWindow(1700), 0u);
}

TEST(RoundLag, AFewLagsAreNotALag) {
    // a healthy mainnet node sees ~20 episodes a day: never a lag
    Monitor m;
    std::vector<std::string> lines;
    for (int64_t t = 0; t < 86400; t += 5) {
        if (t % 4320 == 0) {
            m.onLag(1000 + t, static_cast<uint64_t>(t), "a", true);
        }
        const auto tick = m.tick(1000 + t);
        EXPECT_FALSE(tick.reset);
        if (!tick.text.empty()) {
            lines.push_back(tick.text);
        }
    }
    EXPECT_FALSE(m.lagging());
    EXPECT_TRUE(lines.empty());
}

TEST(RoundLag, PersistentLagWarnsAndResets) {
    // the incident: an episode every minute or so for hours
    Monitor m;
    std::vector<std::string> lines;
    // 5 episodes needed in the window: lagging from the 5th minute, persistent 10 min later
    EXPECT_EQ(lagFor(m, 1000, 14 * 60, 10, &lines), 0u);
    EXPECT_TRUE(m.lagging());
    EXPECT_FALSE(m.persistent());
    EXPECT_TRUE(lines.empty());

    lines.clear();
    EXPECT_EQ(lagFor(m, 1000 + 14 * 60, 2 * 60, 30, &lines), 1u);
    EXPECT_TRUE(m.persistent());
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("ROUND LAG:"), std::string::npos);
    EXPECT_NE(lines[0].find("rebuilding the network sessions"), std::string::npos);
    EXPECT_NE(lines[0].find("reason:"), std::string::npos);

    // still lagging: a warning every 5 min, the next reset 15 min after the first
    lines.clear();
    EXPECT_EQ(lagFor(m, 1000 + 16 * 60, 14 * 60, 50, &lines), 1u);
    EXPECT_EQ(m.resets(), 2u);
    EXPECT_GE(lines.size(), 3u);
    bool sawNotYet = false;
    for (const auto& l : lines) {
        sawNotYet = sawNotYet || l.find("were rebuilt") != std::string::npos;
    }
    EXPECT_TRUE(sawNotYet);
}

TEST(RoundLag, ResetsBackOffAndAreCapped) {
    // a lag that never ends: resets at once, then 15 min later, the third held back to 60 min after the
    // first (never more than 2 in an hour, its own interval was 30 min), then +60, +120, +240 and every
    // 240 min (resetMaxInterval)
    Monitor m;
    std::vector<int64_t> at;
    uint64_t round = 10;
    for (int64_t t = 1000; t < 1000 + 12 * 3600; t += 5) {
        if ((t - 1000) % 60 == 0) {
            m.onLag(t, round++, "peer-a", true);
        }
        if (m.tick(t).reset) {
            at.push_back(t);
        }
    }
    ASSERT_GE(at.size(), 6u);
    EXPECT_EQ(at[1] - at[0], 900);
    EXPECT_EQ(at[2] - at[0], 3600);
    EXPECT_EQ(at[3] - at[2], 3600);
    EXPECT_EQ(at[4] - at[3], 7200);
    EXPECT_EQ(at[5] - at[4], 14400);
    for (size_t i = 0; i + 2 < at.size(); ++i) {
        EXPECT_GE(at[i + 2] - at[i], 3600) << "more than 2 resets in an hour";
    }
    EXPECT_LE(at.size(), 8u);
}

TEST(RoundLag, ResetCapPerHourWithShortSettings) {
    Settings s;
    s.episodes = 1;
    s.persistAfter = 0;
    s.resetEvery = 60;
    s.resetMaxInterval = 60;
    s.resetsPerHour = 2;
    Monitor m(s);
    size_t resets = 0;
    uint64_t round = 1;
    for (int64_t t = 1000; t < 1000 + 3600; t += 5) {
        m.onLag(t, round++, "a", true);
        resets += m.tick(t).reset ? 1 : 0;
    }
    EXPECT_EQ(resets, 2u);
}

TEST(RoundLag, LagOverIsReported) {
    Monitor m;
    lagFor(m, 1000, 20 * 60, 10);
    ASSERT_TRUE(m.persistent());
    // no episode for 10 min: the window empties
    std::vector<std::string> lines;
    for (int64_t t = 1000 + 20 * 60; t < 1000 + 31 * 60; t += 5) {
        const auto tick = m.tick(t);
        EXPECT_FALSE(tick.reset);
        if (!tick.text.empty()) {
            lines.push_back(tick.text);
        }
    }
    EXPECT_FALSE(m.lagging());
    EXPECT_FALSE(m.persistent());
    // the episodes still in the window may bring one more warning first; then one line that it is over
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.back().find("ROUND LAG over"), std::string::npos);
    size_t over = 0;
    for (const auto& l : lines) {
        over += l.find("ROUND LAG over") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(over, 1u);
    EXPECT_EQ(m.resets(), 0u);
}

TEST(RoundLag, StatusJson) {
    Monitor m;
    auto json = m.statusJson(1764000000, 1000);
    EXPECT_NE(json.find("\"lagging\": \"false\""), std::string::npos);
    EXPECT_NE(json.find("\"persistent\": \"false\""), std::string::npos);
    EXPECT_EQ(json.find("\"since\""), std::string::npos);

    lagFor(m, 1000, 16 * 60, 10);
    json = m.statusJson(1764000000, 1000 + 16 * 60);
    EXPECT_NE(json.find("\"persistent\": \"true\""), std::string::npos);
    EXPECT_NE(json.find("\"since\": \""), std::string::npos);
    EXPECT_NE(json.find(" UTC\""), std::string::npos);
    EXPECT_NE(json.find("\"resets\": \"1\""), std::string::npos);
    EXPECT_NE(json.find("\"window_minutes\": \"10\""), std::string::npos);
}

TEST(RoundLag, Settings) {
    Settings s;
    s.episodes = 2;
    s.persistAfter = 60;
    s.window = 120;
    Monitor m(s);
    m.onLag(1000, 1, "a", true);
    m.onLag(1001, 2, "a", true);
    EXPECT_FALSE(m.tick(1001).reset);
    EXPECT_TRUE(m.lagging());
    m.onLag(1050, 3, "a", true);
    const auto tick = m.tick(1062);
    EXPECT_TRUE(tick.reset);
    EXPECT_TRUE(m.persistent());
}
