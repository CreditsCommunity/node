#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <lib/system/common.hpp>

namespace cs::forkguard {

// While FORK HELD with only an upper bound of the fork point ("block #N or lower"), the node asks the
// peer of the refused block for some of its blocks at heights at or below that bound, with the usual
// BlockRequest, and compares their hashes with its own: the last block both chains share is the
// highest height where they match. A k-ary search (a few heights per request) from the bound down to
// a floor (`Settings::maxDepth` blocks below it, never below the first stored block).
//
// Read-only and local: it stores nothing, unwinds nothing and changes nothing the node accepts. The
// peer's blocks serve only to locate the point: a block's hash is computed from its content, a
// "match" is a block identical to ours, and the answer is checked to hold together (matches strictly
// below mismatches, consecutive blocks chained, the first differing block of the peer built on our
// block at the fork point). A peer that lies can at worst make the search fail or move the point
// within [floor, bound]. So the answer is taken as the fork point only when the peer's block right
// after it verifies as a synced block would after our block there (PeerBlock::verified: signatures,
// and round confirmations by the confidants of our block at the point); otherwise it stays a hint
// that --resolve-fork follows only on the operator's explicit request (--to-located). The cut still
// checks that our stored block at the point is the recorded one.
//
// One request at a time, at most one per Settings::minInterval (the peer refuses a second request
// within 5 s of its reply as a duplicate). Single-threaded: the node calls it from its processor
// thread. The network and our chain are reached only through Hooks.
class ForkLocator {
public:
    using Clock = std::chrono::steady_clock;

    struct Settings {
        cs::Sequence maxDepth = 2000;          // blocks searched below the upper bound
        size_t probesPerRequest = 8;           // heights asked for in one request, at least 2
        Clock::duration minInterval = std::chrono::seconds(6);
        Clock::duration replyTimeout = std::chrono::seconds(20);
        size_t maxAttempts = 8;                // requests in a row without a usable reply before giving up
        size_t maxDuplicates = 50;             // requests in a row refused as duplicates before giving up
    };

    struct Hooks {
        // our stored block hash at a height (hex), empty when not stored
        std::function<std::string(cs::Sequence)> ourHash;
        // sends a BlockRequest for these heights to the peer; false when it cannot (not a neighbour)
        std::function<bool(const cs::PublicKey&, const std::vector<cs::Sequence>&)> sendRequest;
    };

    // a block of the peer, as far as the search needs it (hashes in hex). verified: the node checked
    // it as a synced block after OUR block at seq - 1: built on it (previous is our hash there), its
    // signatures verify against its confidants and its round confirmations against the confidants of
    // our block at seq - 1 (never so for a bootstrap block, which carries no confirmations)
    struct PeerBlock {
        cs::Sequence seq = 0;
        std::string hash;
        std::string previous;
        bool verified = false;
    };

    enum class Status {
        Idle,       // never started, or stopped
        Searching,
        Exact,      // forkPoint is the last common block, forkPointHash our hash there; verified() says
                    // whether the peer's next block was verified against our confidants there (only
                    // then may the node take it as the fork point; otherwise it is a hint)
        Deeper,     // the chains already differ at forkPoint (the floor): the fork is below it
        Failed      // why() says why; the node may start again later
    };

    ForkLocator(Hooks hooks, Settings settings);

    // starts a search: the chains are known to differ above `upper` (the block at upper + 1 is not
    // shared); the search goes down to `floor` at most (raised to upper - maxDepth, and to block 1:
    // the genesis block is shared by definition and never asked for)
    void start(const cs::PublicKey& peer, cs::Sequence upper, cs::Sequence floor, Clock::time_point now);
    void stop();

    // sends the next request when one is due, or gives up after Settings::maxAttempts
    void tick(Clock::time_point now);

    // a request of this search to `sender` waits for its reply, and every block of the reply is one
    // of the heights it asked for: the reply is for the search
    bool expects(const cs::PublicKey& sender, const std::vector<cs::Sequence>& replySeqs) const;
    // the reply to the request in flight (blocks not asked for are ignored)
    void onReply(const cs::PublicKey& sender, const std::vector<PeerBlock>& blocks, Clock::time_point now);
    // the peer said it will not answer the request in flight: no such blocks (a failed attempt), or a
    // duplicate request (it answered another request of this node a moment ago, e.g. of the forward
    // sync: tried again, up to Settings::maxDuplicates times in a row)
    void onRefused(const cs::PublicKey& sender, Clock::time_point now, bool duplicate = false);

    Status status() const {
        return status_;
    }
    bool searching() const {
        return status_ == Status::Searching;
    }
    const cs::PublicKey& peer() const {
        return peer_;
    }
    cs::Sequence upper() const {
        return upper_;
    }
    cs::Sequence floor() const {
        return floor_;
    }
    cs::Sequence forkPoint() const {
        return forkPoint_;
    }
    const std::string& forkPointHash() const {
        return forkPointHash_;
    }
    bool verified() const {
        return verified_;
    }
    const std::string& why() const {
        return why_;
    }
    size_t requestsSent() const {
        return requests_;
    }

    // the heights the next request asks for (exposed for tests)
    std::vector<cs::Sequence> nextProbes() const;

private:
    bool matches(cs::Sequence seq) const;
    void fail(const std::string& why);
    void evaluate();

    Hooks hooks_;
    Settings settings_;
    Status status_ = Status::Idle;
    cs::PublicKey peer_{};
    cs::Sequence upper_ = 0;
    cs::Sequence floor_ = 0;
    // highest height known to match (none yet: floor_ not confirmed) and lowest known to differ
    std::optional<cs::Sequence> lo_;
    cs::Sequence hi_ = 0;
    std::map<cs::Sequence, PeerBlock> known_;   // the peer's blocks received
    std::vector<cs::Sequence> inFlight_;        // the heights of the request waiting for its reply
    Clock::time_point sentAt_{};
    Clock::time_point lastSent_{};
    bool everSent_ = false;
    size_t attempts_ = 0;
    size_t duplicates_ = 0;
    size_t requests_ = 0;
    cs::Sequence forkPoint_ = 0;
    std::string forkPointHash_;
    bool verified_ = false;
    std::string why_;
};

// the depth the search covers below the upper bound for a max_auto_rollback setting: 20 times it,
// between 1000 and 10000 blocks; 2000 when max_auto_rollback is 0 (no depth bound)
cs::Sequence locateDepth(uint64_t maxAutoRollback);

}  // namespace cs::forkguard
