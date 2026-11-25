#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include <p2p_network.h>

// net::LostPeers, net::PingRegistry, net::MayTakeOver and KBucket::ReplaceEndpoint of the p2p fork
// (CreditsCommunity/p2p): the peers the routing table lost because they stopped answering, pinged
// again with a backoff so that the two sides of a network cut find each other again when it ends.
//
// Trust model: node ids are unauthenticated (an official peer's ping response carries no nonce or
// signature and cannot be made to), so what is bound is the endpoint. One test per attack the
// security reviews raised: reflection (a response from elsewhere), squatting (an id taken at another
// endpoint), first-writer pinning (an endpoint recorded first for an id), metadata overwrite (a
// verified endpoint taking more than its address and port), and a refreshed loss time.
namespace {

using namespace net;
using Clock = LostPeers::Clock;
using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::seconds;

constexpr auto kWindow = seconds(8);

NodeEntrance peer(uint32_t n) {
    NodeEntrance e;
    e.id = NodeId(n);
    e.address = bi::make_address("127.0.0.1");
    e.udp_port = e.tcp_port = static_cast<uint16_t>(6000 + n % 1000);
    return e;
}

NodeEntrance at(NodeEntrance e, const char* address, uint16_t port) {
    e.address = bi::make_address(address);
    e.udp_port = port;
    return e;
}

std::vector<uint32_t> ids(const std::vector<NodeEntrance>& nodes) {
    std::vector<uint32_t> result;
    for (const auto& n : nodes) {
        result.push_back(static_cast<uint32_t>(n.id.GetLow64()));
    }
    return result;
}

}  // namespace

TEST(LostPeers, APeerIsPingedAgainWithABackoffUpToTheMaximumWait) {
    LostPeers lost;
    const auto t0 = Clock::now();
    ASSERT_TRUE(lost.Add(peer(1), minutes(10), t0));

    EXPECT_TRUE(lost.Due(t0).empty());
    EXPECT_TRUE(lost.Due(t0 + seconds(29)).empty());
    // 30 s after it was lost, then 60, 120 and 180 s (the maximum) after each ping
    auto t = t0 + seconds(30);
    EXPECT_EQ(ids(lost.Due(t)), std::vector<uint32_t>{1});
    EXPECT_TRUE(lost.Due(t + seconds(59)).empty());
    t += seconds(60);
    EXPECT_EQ(ids(lost.Due(t)), std::vector<uint32_t>{1});
    EXPECT_TRUE(lost.Due(t + seconds(119)).empty());
    t += seconds(120);
    EXPECT_EQ(ids(lost.Due(t)), std::vector<uint32_t>{1});
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(lost.Due(t + seconds(179)).empty());
        t += seconds(180);
        EXPECT_EQ(ids(lost.Due(t)), std::vector<uint32_t>{1});
    }
    EXPECT_EQ(LostPeers::Wait(100), LostPeers::kMaxRetry);
}

TEST(LostPeers, AFullListKeepsThePeersThatAnsweredLongest) {
    LostPeers lost;
    const auto t0 = Clock::now();
    for (uint32_t i = 1; i <= LostPeers::kMaxPeers; ++i) {
        ASSERT_TRUE(lost.Add(peer(i), minutes(10) + seconds(i), t0));
    }
    ASSERT_EQ(lost.Size(), LostPeers::kMaxPeers);

    // ids that came and went quickly push out nobody
    for (uint32_t i = 1000; i < 1100; ++i) {
        EXPECT_FALSE(lost.Add(peer(i), seconds(20), t0));
    }
    EXPECT_EQ(lost.Size(), LostPeers::kMaxPeers);
    EXPECT_TRUE(lost.Contains(NodeId(1)));

    // a peer that answered longer than the shortest one takes its place
    EXPECT_TRUE(lost.Add(peer(2000), hours(1), t0));
    EXPECT_EQ(lost.Size(), LostPeers::kMaxPeers);
    EXPECT_FALSE(lost.Contains(NodeId(1)));
    EXPECT_TRUE(lost.Contains(NodeId(2000)));
}

TEST(LostPeers, AtMostAFewPingsACallTheLongestWaitingFirst) {
    LostPeers lost;
    const auto t0 = Clock::now();
    const uint32_t n = LostPeers::kMaxPerCall + 5;
    for (uint32_t i = 1; i <= n; ++i) {
        // peer 1 lost first: due first
        ASSERT_TRUE(lost.Add(peer(i), minutes(10), t0 + seconds(i)));
    }
    const auto t = t0 + minutes(5);
    auto first = ids(lost.Due(t));
    ASSERT_EQ(first.size(), LostPeers::kMaxPerCall);
    for (uint32_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i], i + 1);
    }
    // the rest at the next call, then nobody until their next turn
    EXPECT_EQ(lost.Due(t + seconds(8)).size(), 5u);
    EXPECT_TRUE(lost.Due(t + seconds(16)).empty());
}

// reflection: only the recorded endpoint, answering the latest ping within the window, verifies
TEST(LostPeers, OnlyTheRecordedEndpointAnsweringTheLatestPingVerifies) {
    LostPeers lost;
    const auto t0 = Clock::now();
    const auto p = peer(1);
    ASSERT_TRUE(lost.Add(p, minutes(10), t0));

    // not pinged yet: even its own endpoint does not verify
    EXPECT_FALSE(lost.Verified(p, t0 + seconds(1), kWindow).has_value());

    const auto t = t0 + seconds(30);
    ASSERT_EQ(ids(lost.Due(t)), std::vector<uint32_t>{1});
    // the id claimed from another address or another port (anyone may send a datagram with any id)
    EXPECT_FALSE(lost.Verified(at(p, "10.1.2.3", p.udp_port), t + seconds(1), kWindow).has_value());
    EXPECT_FALSE(lost.Verified(at(p, "127.0.0.1", p.udp_port + 1), t + seconds(1), kWindow).has_value());
    // late
    EXPECT_FALSE(lost.Verified(p, t + kWindow + seconds(1), kWindow).has_value());

    // its own endpoint within the window: verified (when it was lost); it leaves only once back
    auto v = lost.Verified(p, t + seconds(2), kWindow);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, t0);
    EXPECT_TRUE(lost.Contains(NodeId(1)));
    // removing another endpoint of the id removes nothing
    lost.Remove(at(p, "10.1.2.3", p.udp_port));
    EXPECT_TRUE(lost.Contains(NodeId(1)));
    lost.Remove(p);
    EXPECT_FALSE(lost.Contains(NodeId(1)));
}

// first-writer pinning: an endpoint recorded first for an id neither blocks nor replaces another one
TEST(LostPeers, EachEndpointOfAnIdHasItsOwnEntryNoneCanPinTheId) {
    LostPeers lost;
    const auto t0 = Clock::now();
    const auto real = peer(1);
    const auto squatter = at(real, "10.6.6.6", 9999);

    // the squatter's endpoint is recorded first, then the real one: both kept, both pinged
    ASSERT_TRUE(lost.Add(squatter, minutes(1), t0));
    ASSERT_TRUE(lost.Add(real, hours(5), t0 + seconds(10)));
    EXPECT_EQ(lost.Size(), 2u);
    const auto t = t0 + minutes(1);
    auto due = lost.Due(t);
    ASSERT_EQ(due.size(), 2u);

    // the squatter's answer verifies only its own entry, the real peer's its own
    EXPECT_TRUE(lost.Verified(squatter, t + seconds(1), kWindow).has_value());
    auto v = lost.Verified(real, t + seconds(1), kWindow);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, t0 + seconds(10));

    // at most kMaxEndpointsPerId endpoints for one id: more of them push out only the shortest tenure
    // of that id, never the real peer's five hours, and never other ids
    ASSERT_TRUE(lost.Add(peer(2), seconds(30), t0));
    for (uint16_t port = 1; port <= 20; ++port) {
        lost.Add(at(real, "10.6.6.7", port), minutes(2), t0);
    }
    size_t ofId1 = 0;
    std::vector<NodeEntrance> all;
    lost.Get(all);
    for (const auto& e : all) {
        ofId1 += e.id == real.id ? 1 : 0;
    }
    EXPECT_EQ(ofId1, LostPeers::kMaxEndpointsPerId);
    EXPECT_TRUE(lost.Contains(NodeId(2)));
    ASSERT_FALSE(all.empty());
    // the longest-answering first (the hosts file takes the first endpoint of an id)
    EXPECT_EQ(all.front().address, real.address);
    EXPECT_EQ(all.front().udp_port, real.udp_port);
}

// a refreshed loss time: re-adding an entry keeps its loss time and schedule
TEST(LostPeers, ReAddingAnEntryNeitherRefreshesItsLossNorItsSchedule) {
    LostPeers lost;
    const auto t0 = Clock::now();
    ASSERT_TRUE(lost.Add(peer(1), minutes(10), t0));
    lost.Due(t0 + seconds(30));
    ASSERT_TRUE(lost.Add(peer(1), hours(2), t0 + hours(20)));
    EXPECT_EQ(lost.Size(), 1u);
    // forgotten 24 h after the first loss
    EXPECT_FALSE(lost.Due(t0 + hours(23)).empty());
    EXPECT_TRUE(lost.Due(t0 + hours(24)).empty());
    EXPECT_EQ(lost.Size(), 0u);
}

// squatting: a verified endpoint takes an id back from an entry at another endpoint only if that
// entry has stopped answering; a live, answering entry is never evicted for a returning one
TEST(LostPeers, AVerifiedEndpointTakesBackOnlyAnIdWhoseEntryStoppedAnswering) {
    // never answered a ping of ours (one forged datagram is enough to get in): taken back
    EXPECT_TRUE(MayTakeOver(false, std::nullopt));
    // answered, and has now missed a ping: taken back
    EXPECT_TRUE(MayTakeOver(true, uint8_t{1}));
    // answered, and its latest ping is not overdue: it stays
    EXPECT_FALSE(MayTakeOver(true, std::nullopt));
    EXPECT_FALSE(MayTakeOver(true, uint8_t{0}));
}

// metadata overwrite: the entry takes the verified address and UDP port, the TCP port it announces,
// and user data only when the response carried some
TEST(LostPeers, ATakenBackEntryTakesOnlyTheVerifiedEndpoint) {
    const auto real = peer(1);
    auto squatter = at(real, "10.6.6.6", 9999);
    squatter.tcp_port = 4444;
    squatter.user_data = 0xBAD;

    KBucket bucket;
    bucket.AddNode(squatter);
    // nothing for the same endpoint, nor for an id not there
    EXPECT_FALSE(bucket.ReplaceEndpoint(squatter, true));
    EXPECT_FALSE(bucket.ReplaceEndpoint(peer(2), true));
    EXPECT_EQ(bucket.Size(), 1u);

    auto answer = real;
    answer.tcp_port = 6001;
    answer.user_data = 0x1234;
    ASSERT_TRUE(bucket.ReplaceEndpoint(answer, false));
    NodeEntrance held;
    ASSERT_TRUE(bucket.Get(real.id, held));
    EXPECT_EQ(held.address, real.address);
    EXPECT_EQ(held.udp_port, real.udp_port);
    EXPECT_EQ(held.tcp_port, 6001);
    EXPECT_EQ(held.user_data, 0xBADu);  // no user data in the response: not taken

    KBucket other;
    other.AddNode(squatter);
    ASSERT_TRUE(other.ReplaceEndpoint(answer, true));
    ASSERT_TRUE(other.Get(real.id, held));
    EXPECT_EQ(held.user_data, 0x1234u);
}

TEST(PingRegistry, OnlyTheEndpointPingedAnswersWithinTheWindow) {
    PingRegistry pings(kWindow);
    const auto t0 = Clock::now();
    const auto p = peer(1);
    pings.Sent(p, 0, t0);

    // a response claiming the id from elsewhere: no answer, and the ping stays outstanding
    EXPECT_FALSE(pings.Answer(at(p, "10.1.2.3", p.udp_port), t0 + seconds(1)));
    EXPECT_FALSE(pings.Answer(at(p, "127.0.0.1", p.udp_port + 1), t0 + seconds(1)));
    ASSERT_TRUE(pings.Unanswered(p.id).has_value());
    // late
    EXPECT_FALSE(pings.Answer(p, t0 + kWindow + seconds(1)));
    // an id never pinged
    EXPECT_FALSE(pings.Answer(peer(2), t0 + seconds(1)));

    EXPECT_TRUE(pings.Answer(p, t0 + seconds(2)));
    EXPECT_FALSE(pings.Unanswered(p.id).has_value());
    // answered once only
    EXPECT_FALSE(pings.Answer(p, t0 + seconds(3)));
}

TEST(PingRegistry, OnePingPerIdTheLatestEndpointAndTimeCount) {
    PingRegistry pings(kWindow);
    const auto t0 = Clock::now();
    const auto old = peer(1);
    const auto moved = at(old, "127.0.0.2", 7001);

    pings.Sent(old, 0, t0);
    pings.Sent(old, 0, t0 + seconds(8));        // resent: one more unanswered
    EXPECT_EQ(*pings.Unanswered(old.id), 1);
    pings.Sent(moved, 0, t0 + seconds(16));     // pinged at another endpoint: that one counts now
    EXPECT_EQ(*pings.Unanswered(old.id), 2);
    EXPECT_FALSE(pings.Answer(old, t0 + seconds(17)));
    // the window runs from the latest ping
    EXPECT_TRUE(pings.Answer(moved, t0 + seconds(16) + kWindow));

    // a ping for a replacer starts at the eviction count
    pings.Sent(peer(3), 3, t0);
    EXPECT_EQ(*pings.Unanswered(NodeId(3)), 3);
    pings.Erase(NodeId(3));
    EXPECT_FALSE(pings.Unanswered(NodeId(3)).has_value());
}
