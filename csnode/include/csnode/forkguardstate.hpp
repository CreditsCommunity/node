#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <lib/system/common.hpp>

// Local files of the bound on automatic chain reorganisations, next to the block database (the
// parent folder of --db-path; the data folder in Docker). Purely local bookkeeping: nothing here is
// sent to other nodes or changes what a block contains.
//
// - reorg-episode.json: the fixed reference of the current automatic-rollback episode (the head the
//   node had when the episode began). Kept across restarts, so a restart does not give a fresh full
//   depth. Removed when the episode ends (the head is back above the reference) or the operator
//   truncates the chain.
// - fork-held.json: written when the node refuses to unwind for a competing chain (beyond the depth
//   bound, or a competing block not vouched for by the node's confidants). While its "held" is true
//   the node reports FORK HELD. The operator either keeps this chain (the dismiss-fork file: the
//   record then keeps the dismissed branch, still refused, and "held" turns false) or follows the
//   other one (--resolve-fork / --set-bc-top, which remove the file). When the episode ends on its
//   own, the file goes unless it holds a dismissal.
namespace cs::forkguard {

inline constexpr const char* kEpisodeFile = "reorg-episode.json";
inline constexpr const char* kForkFile = "fork-held.json";
// created by the operator (any content) to dismiss the held fork and keep this node's chain; the
// running node picks it up within a round, acts, logs and deletes it
inline constexpr const char* kDismissTrigger = "dismiss-fork";
// at most this many block hashes of dismissed branches are remembered
inline constexpr size_t kMaxBranchHashes = 256;

// the folder of these files for a database path: its parent folder ("." for a relative "db")
std::string stateDirFor(const std::string& dbPath);

struct Episode {
    cs::Sequence highWater = 0;
};

// resolve-approved.json, written by --resolve-fork when the fork point was exact: the one block the
// operator chose, the competing block right after the fork point. If the node stores its own block at
// that height again before the recorded one arrives, the recorded block itself (that very hash, built
// on the fork point) makes the node give its own block up, never going to or below the fork point.
// Nothing else bypasses the usual checks. The approval ends once the node stores a block at that
// height and goes on, or a competing block that passes the usual checks is offered there.
inline constexpr const char* kApprovedFile = "resolve-approved.json";
struct ApprovedBranch {
    cs::Sequence forkPoint = 0;
    cs::Sequence seq = 0;   // forkPoint + 1
    std::string hash;       // the recorded competing block
};
bool saveApprovedBranch(const std::string& dir, const ApprovedBranch& branch);
std::optional<ApprovedBranch> loadApprovedBranch(const std::string& dir);
void clearApprovedBranch(const std::string& dir);

struct ForkRecord {
    // FORK HELD: the node refuses a competing chain and the operator has not decided yet. False once
    // the operator dismissed it (the record then keeps the dismissal) or the episode ended.
    bool held = true;
    std::string reason;            // "depth" (the bound) or "authority" (not vouched for)
    int64_t sinceUnix = 0;         // first refusal of the episode, UTC seconds
    cs::Sequence highWater = 0;    // the episode's reference head
    cs::Sequence ourHead = 0;      // our head when the node refused
    std::string ourHeadHash;
    // the highest of our blocks the competing chain can share: exact when the competing block names
    // it as its previous block, otherwise the competing chain diverges at or below it
    cs::Sequence forkPoint = 0;
    std::string forkPointHash;
    bool forkPointExact = false;
    cs::Sequence competingSeq = 0; // the refused block (0: not known, e.g. a bad-hash reply)
    std::string competingHash;
    std::string peerKey;           // base58, empty when not known
    std::string peerAddress;       // ip:port, empty when not known
    cs::Sequence peerHeight = 0;   // the height the peer announced, 0 when not known
    // the operator kept this node's chain (dismiss-fork): the first known block of the dismissed
    // branch after the fork point, and the hashes of that branch seen since. A competing block that is
    // one of them, or continues one of them, is still refused but raises FORK HELD no more.
    int64_t dismissedUnix = 0;
    cs::Sequence dismissedSeq = 0;
    std::string dismissedHash;
    std::vector<std::string> branchHashes;

    bool hasDismissal() const {
        return !branchHashes.empty();
    }
    // the competing block (hash, previous hash) belongs to a dismissed branch
    bool inDismissedBranch(const std::string& hash, const std::string& previous) const;
    // remember one more block hash of a dismissed branch (the oldest go beyond kMaxBranchHashes)
    void addBranchHash(const std::string& hash);
};

// An empty dir turns persistence off (the functions do nothing and load returns nothing).
bool saveEpisode(const std::string& dir, const Episode& episode);
std::optional<Episode> loadEpisode(const std::string& dir);
void clearEpisode(const std::string& dir);

bool saveForkRecord(const std::string& dir, const ForkRecord& record);
std::optional<ForkRecord> loadForkRecord(const std::string& dir);
void clearForkRecord(const std::string& dir);

// --resolve-fork: the fork point to cut the chain at and the hash the stored block there must have,
// from the record in dir; nothing (and *error says why) when there is no record or it cannot be
// checked against the database
struct ResolvePlan {
    cs::Sequence top = 0;
    std::string hash;
    ForkRecord record;
};
std::optional<ResolvePlan> planResolve(const std::string& dir, std::string* error);

// "2026-11-19 10:04:05 UTC"
std::string utcTime(int64_t unixSeconds);

// the operator's two choices for a held fork: keep this chain (the dismiss-fork file) or follow the
// other one (--resolve-fork, or --set-bc-top <seq> by hand), native and Docker
std::string remedy(const ForkRecord& record);

// the periodic FORK HELD detail line: since when, where the chains diverge, the refused peer and the
// remedy
std::string summary(const ForkRecord& record);

}  // namespace cs::forkguard
