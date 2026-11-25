#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include <p2p_network.h>

// net::LostPeers and net::PingRegistry of the p2p fork (CreditsCommunity/p2p): the peers the routing
// table lost because they stopped answering, pinged again with a backoff so that the two sides of a
// network cut find each other again when it ends (RoutingTable::PingLostAndBootNodes), and the pings
// outstanding, each bound to the endpoint it went to. A ping response carries no nonce (official wire
// format): only one from the endpoint pinged, within the window, counts as an answer.
namespace {

using namespace net;
using Clock = LostPeers::Clock;
using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::seconds;

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

constexpr auto kWindow = seconds(8);

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

TEST(LostPeers, APeerLeavesOnlyWhenItsRecordedEndpointAnswersTheLatestPing) {
    LostPeers lost;
    const auto t0 = Clock::now();
    const auto p = peer(1);
    ASSERT_TRUE(lost.Add(p, minutes(10), t0));

    // not pinged yet: even its own endpoint cannot answer
    EXPECT_FALSE(lost.Answered(p, t0 + seconds(1), kWindow).has_value());

    const auto t = t0 + seconds(30);
    ASSERT_EQ(ids(lost.Due(t)), std::vector<uint32_t>{1});
    // the id claimed from another address or another port (anyone may send a datagram with any id)
    EXPECT_FALSE(lost.Answered(at(p, "10.1.2.3", p.udp_port), t + seconds(1), kWindow).has_value());
    EXPECT_FALSE(lost.Answered(at(p, "127.0.0.1", p.udp_port + 1), t + seconds(1), kWindow).has_value());
    // late
    EXPECT_FALSE(lost.Answered(p, t + kWindow + seconds(1), kWindow).has_value());
    EXPECT_TRUE(lost.Contains(NodeId(1)));
    // still pinged at its recorded endpoint
    const auto t2 = t + seconds(60);
    auto again = lost.Due(t2);
    ASSERT_EQ(ids(again), std::vector<uint32_t>{1});
    EXPECT_EQ(again[0].address, p.address);
    EXPECT_EQ(again[0].udp_port, p.udp_port);

    // its own endpoint within the window: it leaves the list, and when it was lost comes back
    auto back = lost.Answered(p, t2 + seconds(2), kWindow);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, t0);
    EXPECT_FALSE(lost.Contains(NodeId(1)));
    EXPECT_FALSE(lost.Answered(p, t2 + seconds(3), kWindow).has_value());
}

TEST(LostPeers, APeerLostADayAgoIsNoLongerPinged) {
    LostPeers lost;
    const auto t0 = Clock::now();
    ASSERT_TRUE(lost.Add(peer(2), minutes(10), t0));

    EXPECT_FALSE(lost.Due(t0 + hours(23)).empty());
    EXPECT_TRUE(lost.Due(t0 + hours(24)).empty());
    EXPECT_EQ(lost.Size(), 0u);
}

TEST(LostPeers, LostAgainStartsOverAndKeepsTheLongerTenure) {
    LostPeers lost;
    const auto t0 = Clock::now();
    ASSERT_TRUE(lost.Add(peer(1), hours(2), t0));
    // pinged a few times
    lost.Due(t0 + minutes(1));
    lost.Due(t0 + minutes(3));
    // the same peer lost again later (it came back without leaving the list, then stopped answering):
    // its schedule starts over
    const auto t1 = t0 + hours(1);
    ASSERT_TRUE(lost.Add(peer(1), seconds(5), t1));
    EXPECT_EQ(lost.Size(), 1u);
    EXPECT_TRUE(lost.Due(t1 + seconds(29)).empty());
    EXPECT_EQ(lost.Due(t1 + seconds(30)).size(), 1u);
    // and with the two hours it had, a full list does not push it out for a short-lived id
    for (uint32_t i = 2; i <= LostPeers::kMaxPeers; ++i) {
        ASSERT_TRUE(lost.Add(peer(i), minutes(30), t1));
    }
    EXPECT_TRUE(lost.Add(peer(5000), hours(1), t1));
    EXPECT_TRUE(lost.Contains(NodeId(1)));
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

TEST(LostPeers, TheIdEvictedAtAnotherEndpointNeitherReplacesTheEntryNorInheritsItsTenure) {
    LostPeers lost;
    const auto t0 = Clock::now();
    const auto real = peer(1);
    const auto squatter = at(real, "10.6.6.6", 9999);
    ASSERT_TRUE(lost.Add(real, hours(5), t0));

    // whoever took the id meanwhile is evicted in turn: the entry stays the real endpoint's
    EXPECT_FALSE(lost.Add(squatter, seconds(40), t0 + minutes(2)));
    EXPECT_EQ(lost.Size(), 1u);
    auto due = lost.Due(t0 + seconds(30));
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].address, real.address);
    EXPECT_EQ(due[0].udp_port, real.udp_port);
    EXPECT_FALSE(lost.Answered(squatter, t0 + seconds(31), kWindow).has_value());

    // and it did not inherit the five hours: a full list pushes it out before the real peer
    LostPeers other;
    ASSERT_TRUE(other.Add(squatter, seconds(40), t0));
    EXPECT_FALSE(other.Add(real, hours(5), t0));  // other endpoint for the same id: the first stays
    for (uint32_t i = 2; i <= LostPeers::kMaxPeers; ++i) {
        ASSERT_TRUE(other.Add(peer(i), minutes(30), t0));
    }
    EXPECT_TRUE(other.Add(peer(7000), minutes(10), t0));
    EXPECT_FALSE(other.Contains(NodeId(1)));
}

// The squatting case of RoutingTable::OnPacketReceived: while a peer is lost, another node claims its
// id from its own endpoint and gets into the k-buckets (anyone may claim any id). The real peer's ping
// response then fails the endpoint check of the k-buckets, so it is verified against the lost entry
// first (LostPeers::Answered), and the k-bucket entry takes the verified endpoint (KBucket::ReplaceEndpoint).
TEST(LostPeers, ASquattedIdIsTakenBackByTheVerifiedEndpoint) {
    LostPeers lost;
    KBucket bucket;
    const auto t0 = Clock::now();
    const auto real = peer(1);
    const auto squatter = at(real, "10.6.6.6", 9999);

    ASSERT_TRUE(lost.Add(real, hours(1), t0));
    bucket.AddNode(squatter);

    // the routing table's endpoint check alone would refuse the real peer
    NodeEntrance held;
    ASSERT_TRUE(bucket.Get(real.id, held));
    EXPECT_FALSE(held.address == real.address && held.udp_port == real.udp_port);

    // the squatter answering a retry ping does not count: it is not the recorded endpoint
    const auto t = t0 + seconds(30);
    ASSERT_EQ(lost.Due(t).size(), 1u);
    EXPECT_FALSE(lost.Answered(squatter, t + seconds(1), kWindow).has_value());
    // nor a replacement with the endpoint already there
    EXPECT_FALSE(bucket.ReplaceEndpoint(squatter));

    // the real peer answers at its recorded endpoint: verified, and the entry is its own again
    ASSERT_TRUE(lost.Answered(real, t + seconds(2), kWindow).has_value());
    EXPECT_TRUE(bucket.ReplaceEndpoint(real));
    ASSERT_TRUE(bucket.Get(real.id, held));
    EXPECT_EQ(held.address, real.address);
    EXPECT_EQ(held.udp_port, real.udp_port);
    EXPECT_EQ(bucket.Size(), 1u);
    EXPECT_FALSE(bucket.ReplaceEndpoint(real));
    // an id that is not in the bucket is not added by it
    EXPECT_FALSE(bucket.ReplaceEndpoint(peer(2)));
    EXPECT_EQ(bucket.Size(), 1u);
}
