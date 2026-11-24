#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

// Round lag that does not go away. A node that keeps missing round packages sees messages of rounds
// ahead of its own ("detect round lag") and catches up by SYNC, round after round; after a network
// outage this went on for hours on a node behind a NAT until it was restarted (incident 2026-11-24).
// Log, status file and the decision to rebuild the network sessions only: nothing here is sent,
// signed, hashed or validated.
//
// - an episode is a local round in which the node saw a message two or more rounds ahead;
// - the node lags while the last `window` seconds hold at least `episodes` episodes, and the lag
//   persists once it has lagged for `persistAfter` seconds;
// - while it persists: a ROUND LAG warning that says what the node does about it, every `warnEvery`
//   seconds, and a network reset (connections and peer table rebuilt, as a restart would) at most every
//   `resetEvery` seconds; ROUND LAG over when it stops;
// - round-lag.json next to the db folder (like fork-held.json): the Docker healthcheck reports the
//   container unhealthy while "persistent" is "true".
namespace cs::roundlag {

inline constexpr const char* kStatusFile = "round-lag.json";

struct Settings {
    int64_t window = 600;        // seconds of episodes counted
    size_t episodes = 5;         // that many in the window: the node lags
    int64_t persistAfter = 600;  // lagging this long: the lag persists
    int64_t warnEvery = 300;     // the warning while it persists
    int64_t resetEvery = 900;    // at most one network reset in this long
};

struct Tick {
    bool reset = false;          // rebuild the network sessions now
    bool warning = false;        // `text` is a warning (else information)
    std::string text;            // a line to log, empty for none
    bool statusChanged = false;  // the status file is to be written again
};

class Monitor {
public:
    explicit Monitor(Settings settings = Settings{});

    // a message two or more rounds ahead seen at local round `round`; nowSeconds: a monotonic clock.
    // Returns true for the first of that round (a new episode)
    bool onLag(int64_t nowSeconds, uint64_t round);

    // every few seconds: what to log and whether to reset the network sessions
    Tick tick(int64_t nowSeconds);

    bool lagging() const;
    bool persistent() const;
    size_t resets() const;
    size_t episodesInWindow(int64_t nowSeconds) const;

    // the status file's JSON text; wallUnix: the time now, nowSeconds: the monotonic clock
    std::string statusJson(int64_t wallUnix, int64_t nowSeconds) const;

private:
    void pruneLocked(int64_t nowSeconds) const;

    Settings settings_;
    mutable std::mutex mutex_;
    mutable std::deque<std::pair<int64_t, uint64_t>> episodes_;  // (time, local round)
    uint64_t lastRound_ = 0;
    bool haveRound_ = false;
    int64_t lagSince_ = 0;       // monotonic, 0: not lagging
    bool persistent_ = false;
    int64_t lastWarning_ = 0;
    int64_t lastReset_ = 0;
    size_t resets_ = 0;          // network resets in this persistent lag
    size_t totalResets_ = 0;     // since the start
};

}  // namespace cs::roundlag
