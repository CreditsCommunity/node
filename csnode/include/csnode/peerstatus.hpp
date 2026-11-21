#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <lib/system/common.hpp>

// Who keeps the network going: the official nodes (4.2.532, minor version 0 in the handshake) or the
// community releases (minor version N > 0). Log and status only: the node reads what peers announce
// (host_data) and the confidants of the blocks it stores; nothing here is sent, signed, hashed or
// validated.
//
// - every 10 minutes (and once at the start) a PEERS line: the neighbours by kind, the trusted seats
//   of the last rounds by kind, how often this node was among them;
// - OFFICIAL NODES GONE when no official neighbour was seen for 10 minutes, or no official node was
//   among the trusted of the last 200 rounds; OFFICIAL NODES BACK when they return;
// - a warning when the trusted seats of the last 1000 rounds were all community nodes;
// - peer-status.json next to the db folder (like fork-held.json), for the Docker healthcheck and
//   operators.
namespace cs::peerstatus {

inline constexpr const char* kStatusFile = "peer-status.json";

enum class Kind : uint8_t {
    Unknown,
    Official,
    Community
};

// from the announced host_data: version 0 means nothing announced yet
Kind classify(uint16_t version, uint16_t minorVersion);
const char* kindName(Kind kind);

struct Settings {
    size_t window = 1000;               // rounds in the PEERS line
    int64_t goneSeconds = 600;          // no official neighbour for this long: gone
    size_t goneRounds = 200;            // no official among the trusted of this many rounds: gone
    size_t communityOnlyRounds = 1000;  // all trusted seats community for this many rounds: warning
    int64_t summaryEvery = 600;         // the PEERS line
    int64_t repeatEvery = 600;          // an alert while its state holds
};

struct NeighbourCounts {
    size_t total = 0;
    size_t official = 0;
    size_t community = 0;
    size_t unknown = 0;
};

struct SeatCounts {
    size_t rounds = 0;
    size_t seats = 0;
    size_t official = 0;
    size_t community = 0;
    size_t unknown = 0;
    size_t selfTrusted = 0;  // rounds with this node among the trusted
    cs::Sequence first = 0;
    cs::Sequence last = 0;
};

struct Line {
    bool warning = false;
    std::string text;
};

// integer percent, rounded; 0 for an empty total
unsigned percent(size_t part, size_t total);

class Tracker {
public:
    explicit Tracker(Settings settings = Settings{});

    // this node: its key and its own kind (a community build)
    void setSelf(const cs::PublicKey& key, Kind kind);
    // what the node knows of the peers' announced versions (replaces the previous map)
    void setKinds(std::map<cs::PublicKey, Kind> kinds);
    Kind kindOf(const cs::PublicKey& key) const;
    // by the base58 key (the fork record's peer_key); Unknown when not known
    Kind kindOfBase58(const std::string& key) const;

    // a stored block: the confidants and the realTrusted mask (bit i: confidant i signed)
    void addBlock(cs::Sequence seq, const std::vector<cs::PublicKey>& confidants, uint64_t realTrusted);
    // a block removed from the top: forget it and anything above
    void removeBlock(cs::Sequence seq);

    // the trusted seats of the last `rounds` stored blocks (all of the window for 0)
    SeatCounts seats(size_t rounds = 0) const;
    NeighbourCounts countNeighbours(const std::vector<cs::PublicKey>& neighbours) const;

    // every few seconds: the neighbours now; returns the lines to log (the PEERS line when due,
    // alerts on transitions and at most every repeatEvery while they hold); nowSeconds: a monotonic clock
    std::vector<Line> tick(int64_t nowSeconds, const NeighbourCounts& neighbours);

    std::string peersLine(const NeighbourCounts& neighbours) const;

    bool officialGone() const;
    bool communityOnly() const;

    // the status file's JSON text (as of the last tick)
    std::string statusJson(int64_t wallUnix) const;

private:
    struct Block {
        cs::Sequence seq = 0;
        std::vector<cs::PublicKey> trusted;
        bool self = false;
    };

    SeatCounts seatsLocked(size_t rounds) const;
    std::string peersLineLocked(const NeighbourCounts& n) const;

    Settings settings_;
    mutable std::mutex mutex_;
    cs::PublicKey self_{};
    Kind selfKind_ = Kind::Community;
    std::map<cs::PublicKey, Kind> kinds_;
    std::map<std::string, Kind> kindsBase58_;
    std::deque<Block> blocks_;

    // "long ago" for the throttles: the first alert or summary is always due
    static constexpr int64_t kNever = (std::numeric_limits<int64_t>::min)() / 2;

    bool started_ = false;
    int64_t startedAt_ = 0;
    int64_t lastSummary_ = kNever;
    int64_t lastOfficialNeighbour_ = 0;  // last tick with an official neighbour (startedAt_ before)
    bool officialNeighbourSeen_ = false;
    NeighbourCounts lastNeighbours_{};

    bool gone_ = false;
    std::string goneReason_;
    int64_t goneSince_ = 0;
    bool goneAlerted_ = false;
    int64_t lastGoneAlert_ = kNever;
    int64_t lastBackAlert_ = kNever;

    bool communityOnly_ = false;
    bool communityOnlyAlerted_ = false;
    int64_t lastCommunityOnlyAlert_ = kNever;

    std::string lastAlert_;
};

}  // namespace cs::peerstatus
