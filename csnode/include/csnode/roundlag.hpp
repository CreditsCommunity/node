#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Round lag that does not go away. A node that keeps missing round packages sees messages of rounds
// ahead of its own ("detect round lag") and catches up by SYNC, round after round; after a network
// outage this went on for hours on a node behind a NAT until it was restarted (incident 2026-11-24).
// Log, status file and the decision to rebuild the network sessions only: nothing here is sent,
// signed, hashed or validated.
//
// - an episode is a local round in which the node saw a message two or more rounds ahead. The round
//   in a packet header is the sender's word only, so an episode counts only when it is confirmed: at
//   that moment a quorum of distinct neighbours (at least 3, and more than half of them) announced a
//   last block at least two above the node's own. One peer, or a few, sending inflated rounds never
//   make the node lag here, let alone reset its network;
// - the node lags while the last `window` seconds hold at least `episodes` confirmed episodes, and
//   the lag persists once it has lagged for `persistAfter` seconds;
// - while it persists: a ROUND LAG warning that says what the node does about it, every `warnEvery`
//   seconds, and a network reset (connections and peer table rebuilt, as a restart would), the first
//   one at once, then after `resetEvery` seconds, doubling up to `resetMaxInterval`, and never more
//   than `resetsPerHour` in an hour; every reset is logged with its reason; ROUND LAG over when it stops;
// - round-lag.json next to the db folder (like fork-held.json): the Docker healthcheck reports the
//   container unhealthy while "persistent" is "true".
namespace cs::roundlag {

inline constexpr const char* kStatusFile = "round-lag.json";

struct Settings {
    int64_t window = 600;        // seconds of episodes counted
    size_t episodes = 5;         // that many in the window: the node lags
    int64_t persistAfter = 600;  // lagging this long: the lag persists
    int64_t warnEvery = 300;     // the warning while it persists
    int64_t resetEvery = 900;    // after a reset, the next one at the earliest; doubles after each
    int64_t resetMaxInterval = 4 * 3600;  // the doubling stops there
    size_t resetsPerHour = 2;    // never more in the last hour
};

// whether the node is really behind, from what it can check itself: its own last block against
// the last blocks its neighbours announced (distinct peers)
bool quorumAhead(uint64_t ownLastSeq, const std::vector<uint64_t>& neighbourLastSeqs, size_t* ahead = nullptr);

struct Tick {
    bool reset = false;          // rebuild the network sessions now
    bool warning = false;        // `text` is a warning (else information)
    std::string text;            // a line to log, empty for none
    bool statusChanged = false;  // the status file is to be written again
};

class Monitor {
public:
    explicit Monitor(Settings settings = Settings{});

    // a message two or more rounds ahead seen at local round `round` from `sender` (any stable id of
    // the peer); confirmed: quorumAhead held at that moment. nowSeconds: a monotonic clock.
    // Returns true for the first message of that round (the one to log)
    bool onLag(int64_t nowSeconds, uint64_t round, const std::string& sender, bool confirmed);

    // every few seconds: what to log and whether to reset the network sessions
    Tick tick(int64_t nowSeconds);

    bool lagging() const;
    bool persistent() const;
    size_t resets() const;
    // confirmed episodes in the window (the ones that count)
    size_t episodesInWindow(int64_t nowSeconds) const;
    // every episode in the window, confirmed or not
    size_t seenInWindow(int64_t nowSeconds) const;

    // the status file's JSON text; wallUnix: the time now, nowSeconds: the monotonic clock
    std::string statusJson(int64_t wallUnix, int64_t nowSeconds) const;

private:
    void pruneLocked(int64_t nowSeconds) const;

    Settings settings_;
    mutable std::mutex mutex_;
    struct Episode {
        int64_t time = 0;
        std::set<std::string> senders;
        bool confirmed = false;
    };
    size_t confirmedLocked() const;
    mutable std::map<uint64_t, Episode> episodes_;  // by local round
    std::deque<int64_t> resetTimes_;                // the resets of the last hour
    int64_t lagSince_ = 0;       // monotonic, 0: not lagging
    bool persistent_ = false;
    int64_t lastWarning_ = 0;
    int64_t lastReset_ = 0;
    size_t resets_ = 0;          // network resets in this persistent lag
    size_t totalResets_ = 0;     // since the start
};

}  // namespace cs::roundlag
