#include <gtest/gtest.h>

#include <string>

#include <banner.hpp>

namespace {
cs::BannerInfo info(bool pruned) {
    cs::BannerInfo i;
    i.version = "4.2.532-community.16";
    i.protocol = 532;
    i.build = "7de684740d7585fbc167cfdd44819a496ee26912";
    i.pruned = pruned;
    i.backend = "lmdb";
    i.serve = true;
    i.backfill = true;
    return i;
}
}  // namespace

TEST(StartupBanner, LogoThenVersionAndSettings) {
    const auto lines = cs::startupBanner(info(false), true);
    ASSERT_EQ(lines.size(), 10u);
    EXPECT_EQ(lines[0], "   ____ ____  _____ ____ ___ _____ ____");
    EXPECT_EQ(lines[5], "              c o m m u n i t y");
    EXPECT_EQ(lines[7], "  node 4.2.532-community.16  (protocol 532, build 7de68474)");
    EXPECT_EQ(lines[8], "  storage: full, lmdb    snapshots: serve on    backfill: on");
    EXPECT_EQ(lines[9], "  https://github.com/CreditsCommunity/node");
}

// banner=off: the same lines without the logo; a pruned node shows no backfill
TEST(StartupBanner, PlainLinesForAPrunedNode) {
    const auto lines = cs::startupBanner(info(true), false);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "  node 4.2.532-community.16  (protocol 532, build 7de68474)");
    EXPECT_EQ(lines[1], "  storage: pruned, lmdb    snapshots: serve on");
}

TEST(StartupBanner, NoBuildWhenUnknown) {
    auto i = info(true);
    i.build.clear();
    EXPECT_EQ(cs::startupBanner(i, false)[0], "  node 4.2.532-community.16  (protocol 532)");
}
