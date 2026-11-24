#include <gtest/gtest.h>

#include <set>
#include <string>

#include <net/keepalive.hpp>

using cs::keepalive::select;

namespace {
std::vector<std::string> keys(size_t n, const char* prefix = "p") {
    std::vector<std::string> v;
    for (size_t i = 0; i < n; ++i) {
        v.push_back(std::string(prefix) + std::to_string(i));
    }
    return v;
}
}  // namespace

TEST(Keepalive, NeighboursAreLeftOut) {
    const auto table = keys(5);
    const std::set<std::string> neighbours{"p1", "p3"};
    size_t offset = 0;
    const auto picked = select<std::string>(table, [&](const std::string& k) { return neighbours.count(k) > 0; }, 32, offset);
    EXPECT_EQ(picked, (std::vector<std::string>{"p0", "p2", "p4"}));
}

TEST(Keepalive, CappedAndRotating) {
    // thousands of routing table entries: never more than the cap in one iteration, every peer in turn
    const auto table = keys(1000);
    size_t offset = 0;
    std::set<std::string> seen;
    for (int it = 0; it < 32; ++it) {
        const auto picked = select<std::string>(table, [](const std::string&) { return false; },
                                                cs::keepalive::kMaxPeersPerIteration, offset);
        EXPECT_EQ(picked.size(), cs::keepalive::kMaxPeersPerIteration);
        seen.insert(picked.begin(), picked.end());
    }
    EXPECT_EQ(seen.size(), 1000u);
}

TEST(Keepalive, NothingToPing) {
    size_t offset = 7;
    EXPECT_TRUE(select<std::string>({}, [](const std::string&) { return false; }, 32, offset).empty());
    const auto table = keys(3);
    EXPECT_TRUE(select<std::string>(table, [](const std::string&) { return true; }, 32, offset).empty());
    EXPECT_TRUE(select<std::string>(table, [](const std::string&) { return false; }, 0, offset).empty());
}

TEST(Keepalive, OffsetBeyondAShrunkTable) {
    size_t offset = 50;
    const auto picked = select<std::string>(keys(4), [](const std::string&) { return false; }, 2, offset);
    EXPECT_EQ(picked.size(), 2u);
    EXPECT_LT(offset, 4u);
}
