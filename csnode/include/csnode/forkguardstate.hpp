#pragma once

#include <chrono>
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

// a small file in dir written whole: to a temporary name first, flushed to the disk, then renamed over
// the old one (the folder entry flushed too on POSIX), so a crash leaves either the old file or the new
// one. False (nothing replaced) on any failure or an empty dir.
bool writeFileAtomic(const std::string& dir, const char* name, const std::string& text);

struct Episode {
    cs::Sequence highWater = 0;
};

// resolve-approved.json, written by --resolve-fork when the fork point was exact: the one block the
// operator chose, the competing block right after the fork point. If the node stores its own block at
// that height again before the recorded one arrives, the recorded block itself (that very hash, built
// on the fork point) makes the node give its own block up, never going to or below the fork point.
// Nothing else bypasses the usual checks. The approval ends once the node stores a block at that
// height and goes on, or a competing block that passes the usual checks is offered there.
//
// The branch the operator chose is also preferred, once, until the node is past that height: while
// the preference lasts (untilUnix, kPreferenceSeconds after the cut) the node stores at forkPoint + 1
// only that block, refuses any other one there (e.g. the abandoned branch, which neighbours that still
// hold it would otherwise sync back on top of the cut) and asks the peers that have the chosen block
// first. If the chosen block does not arrive in time, the preference ends with a loud log line, but
// the abandoned block itself (abandonedHash, our block at forkPoint + 1 before the cut) stays refused
// there until the operator acts again (a new cut replaces the file). It only refuses blocks: it never
// lets a block through any check it would otherwise fail.
inline constexpr const char* kApprovedFile = "resolve-approved.json";
inline constexpr int64_t kPreferenceSeconds = 30 * 60;
struct ApprovedBranch {
    cs::Sequence forkPoint = 0;
    cs::Sequence seq = 0;   // forkPoint + 1
    std::string hash;       // the recorded competing block (the chosen one)
    std::string abandonedHash;  // our block at seq before the cut (empty: not known)
    std::string peerKey;        // base58: a peer that had the chosen block (empty: not known)
    int64_t untilUnix = 0;      // the preference lasts until then (0: no preference, approval only)
    bool expired = false;       // the preference timed out: only abandonedHash is still refused

    // the preference for the chosen block is on (until expireIfDue turns it off)
    bool preferring() const {
        return untilUnix != 0 && !expired;
    }
    // the preference times out at untilUnix: true (once) when it just did
    bool expireIfDue(int64_t nowUnix) {
        if (!preferring() || nowUnix < untilUnix) {
            return false;
        }
        expired = true;
        return true;
    }
};
// what the node does with a block at a height the approved branch covers
enum class BranchVerdict {
    None,     // not concerned: the usual checks decide
    Chosen,   // the chosen block itself (it still passes the usual checks)
    Refuse    // another block at seq while preferring, or the abandoned block
};
// a block `hash` at `seq`
BranchVerdict judgeBlock(const ApprovedBranch& branch, cs::Sequence seq, const std::string& hash);
// a block at seq + 1 (built on `previous`): refused when it is not built on the chosen block while
// preferring, or built on the abandoned block
BranchVerdict judgeChild(const ApprovedBranch& branch, cs::Sequence seq, const std::string& previous);
bool saveApprovedBranch(const std::string& dir, const ApprovedBranch& branch);
std::optional<ApprovedBranch> loadApprovedBranch(const std::string& dir, bool* corrupt = nullptr);
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
    // the search for the last common block (ForkLocator) while FORK HELD: located is true once it has
    // an answer, found from the upper bound forkPoint had before (upperBound) with the peer locatedWith.
    // - verified (locatedVerified): the peer's block right after the located point is built on our
    //   block there, and its signatures and round confirmations verify against the confidants of our
    //   block there, as a synced block's would. Only then is the point the fork point: forkPoint and
    //   forkPointHash are set to it and forkPointExact turns true (the competing block is still the
    //   one first refused, so no approval follows from it).
    // - not verified (a bootstrap block, or confirmations that do not verify): a hint only. forkPoint
    //   stays the upper bound; locatedPoint/locatedHash keep the hint, and --resolve-fork cuts there
    //   only with --to-located.
    // - deeper: the peer's block at deeperThan, the lowest one searched, already differs from ours; a
    //   hint as well (a peer can claim it), forkPoint stays the upper bound
    // locatedNextHash: the peer's verified block right after the located point (the competing
    // branch's first block, the one --resolve-fork then prefers); ourNextHash: our block right after the
    // fork point when the record was written or the point located (the branch a cut abandons)
    bool located = false;
    bool locatedVerified = false;
    std::string locatedNextHash;
    std::string ourNextHash;
    cs::Sequence locatedPoint = 0;
    std::string locatedHash;
    bool deeper = false;
    cs::Sequence upperBound = 0;
    cs::Sequence deeperThan = 0;
    std::string locatedWith;
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

    // "exact", "located (unverified)", "deeper" (below deeperThan, unverified) or "upper bound"
    std::string forkPointStatus() const {
        if (forkPointExact) {
            return "exact";
        }
        if (located && !deeper && !locatedHash.empty()) {
            return "located (unverified)";
        }
        return deeper ? "deeper" : "upper bound";
    }
    bool hasDismissal() const {
        return !branchHashes.empty();
    }
    // the competing block (hash, previous hash) belongs to a dismissed branch
    bool inDismissedBranch(const std::string& hash, const std::string& previous) const;
    // remember one more block hash of a dismissed branch (the oldest go beyond kMaxBranchHashes, never
    // dismissedHash)
    void addBranchHash(const std::string& hash);
};

// An empty dir turns persistence off (the functions do nothing and load returns nothing).
bool saveEpisode(const std::string& dir, const Episode& episode);
// *corrupt (if given) is set when the file exists but cannot be read
std::optional<Episode> loadEpisode(const std::string& dir, bool* corrupt = nullptr);
void clearEpisode(const std::string& dir);

bool saveForkRecord(const std::string& dir, const ForkRecord& record);
std::optional<ForkRecord> loadForkRecord(const std::string& dir, bool* corrupt = nullptr);
void clearForkRecord(const std::string& dir);
// all three files: the database they describe was replaced (wiped, restored from a snapshot)
void clearAll(const std::string& dir);

// --resolve-fork: the fork point to cut the chain at and the hash the stored block there must have,
// from the record in dir; nothing (and *error says why) when there is no record or it cannot be
// checked against the database
// toLocated (--resolve-fork --to-located): the located but unverified last common block instead of
// the recorded fork point
struct ResolvePlan {
    cs::Sequence top = 0;
    std::string hash;
    ForkRecord record;
    bool toLocated = false;
};
std::optional<ResolvePlan> planResolve(const std::string& dir, std::string* error, bool toLocated = false);

// what --resolve-fork shows before it cuts: our head, the upper bound, the located point and whether
// it is verified, where the cut goes and how many blocks it removes
std::string resolveOverview(const ResolvePlan& plan);

// "2026-11-19 10:04:05 UTC"
std::string utcTime(int64_t unixSeconds);

// the operator's two choices for a held fork: keep this chain (the dismiss-fork file) or follow the
// other one (--resolve-fork, or --set-bc-top <seq> by hand), native and Docker
std::string remedy(const ForkRecord& record);

// the periodic FORK HELD detail line: since when, where the chains diverge, the refused peer and the
// remedy. peerSide (optional): what the refused peer announces, e.g. "official 4.2.532"
std::string summary(const ForkRecord& record, const std::string& peerSide = std::string());

// A line for an event that may repeat many times a minute (a node holding a fork refuses every block
// its peers offer of the other chain, and asks for them again): the first event gets a line at once,
// later ones are counted and reported with the next line, at most one per `period`. onEvent returns
// the number of events the line due now reports (this one included), or 0 when no line is due.
class RepeatedLine {
public:
    explicit RepeatedLine(std::chrono::steady_clock::duration period = std::chrono::minutes(1))
    : period_(period) {}

    uint64_t onEvent(std::chrono::steady_clock::time_point now) {
        ++pending_;
        if (logged_ && now - last_ < period_) {
            return 0;
        }
        const uint64_t count = pending_;
        pending_ = 0;
        last_ = now;
        logged_ = true;
        return count;
    }

private:
    std::chrono::steady_clock::duration period_;
    std::chrono::steady_clock::time_point last_{};
    bool logged_ = false;
    uint64_t pending_ = 0;
};

}  // namespace cs::forkguard
