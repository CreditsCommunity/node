#include <gtest/gtest.h>

#include <csnode/roundlag.hpp>

using cs::roundlag::Monitor;
using cs::roundlag::Settings;

namespace {
// one lag episode a minute from `from`, at rounds from `round`, ticking every 5 s; returns the resets
size_t lagFor(Monitor& m, int64_t from, int64_t seconds, uint64_t round, std::vector<std::string>* lines = nullptr) {
    size_t resets = 0;
    for (int64_t t = from; t < from + seconds; t += 5) {
        if ((t - from) % 60 == 0) {
            m.onLag(t, round++);
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

TEST(RoundLag, OneEpisodePerLocalRound) {
    Monitor m;
    EXPECT_TRUE(m.onLag(1000, 50));
    EXPECT_FALSE(m.onLag(1000, 50));
    EXPECT_FALSE(m.onLag(1001, 50));
    EXPECT_TRUE(m.onLag(1002, 51));
    EXPECT_EQ(m.episodesInWindow(1002), 2u);
}

TEST(RoundLag, EpisodesLeaveTheWindow) {
    Monitor m;
    for (uint64_t r = 0; r < 4; ++r) {
        m.onLag(1000 + static_cast<int64_t>(r), 100 + r);
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
            m.onLag(1000 + t, static_cast<uint64_t>(t));
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
    m.onLag(1000, 1);
    m.onLag(1001, 2);
    EXPECT_FALSE(m.tick(1001).reset);
    EXPECT_TRUE(m.lagging());
    m.onLag(1050, 3);
    const auto tick = m.tick(1062);
    EXPECT_TRUE(tick.reset);
    EXPECT_TRUE(m.persistent());
}
