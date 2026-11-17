#ifndef COMMUNITYBOOTSTRAP_HPP
#define COMMUNITYBOOTSTRAP_HPP

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include <lib/system/common.hpp>

namespace cs {

// Community bootstrap: a liveness fallback, off by default ([community_bootstrap] in config.ini).
//
// Only the initial confidants (initialConfidants_) start a bootstrap, and a table with any other key
// needs the project's signature. If the initial confidants that are still running are too few, the
// network stays stalled whatever the other nodes do. The fallback lets a node that lists a set of
// community keys start and take, after a long stall only, a bootstrap table of keys from the initial
// confidants and that list, signed by its starter instead of the project.
//
// What keeps it out of normal operation:
// - the node itself has seen no block, no round of a round package and no regular bootstrap table
//   for stall_minutes, and no neighbour reports a block above its own (that node would sync);
// - the table: 4 to 25 keys in ascending order, each an initial confidant or listed, at least one
//   of them listed and not an initial confidant (else it is a regular table), more than half of the
//   list in it (two parts of a split community cannot both restart), and signed by its first key, the
//   starter;
// - the round window (#71) and the stall check (#70) of every unsigned table still apply;
// - a node takes one such table per half round_elapse_time, or a heavier one at once, and a regular
//   table always replaces it (it goes through the unchanged path, whose weight it does not touch).
// Official nodes, and community nodes without the list, drop such a table: one of its keys is not an
// initial confidant and its signature is not the project's.

constexpr size_t kCommunityMinTable = 4;   // Consensus::MinTrustedNodes
constexpr size_t kCommunityMaxTable = 25;  // Consensus::MaxTrustedNodes

using CommunityWeight = uint64_t;

// the starter's table is a std::set, sent in order: each key above the previous one
inline bool isAscending(const std::vector<PublicKey>& table) {
    for (size_t i = 1; i < table.size(); ++i) {
        if (!(table[i - 1] < table[i])) {
            return false;
        }
    }
    return true;
}

inline bool isCommunityTable(const std::set<PublicKey>& initial, const std::set<PublicKey>& community,
                             const std::vector<PublicKey>& table) {
    if (community.empty() || table.size() < kCommunityMinTable || table.size() > kCommunityMaxTable || !isAscending(table)) {
        return false;
    }
    size_t listed = 0;
    bool outsideInitial = false;
    for (const auto& key : table) {
        const bool isInitial = initial.count(key) > 0;
        const bool isListed = community.count(key) > 0;
        if (!isInitial && !isListed) {
            return false;
        }
        outsideInitial = outsideInitial || !isInitial;
        listed += isListed ? 1 : 0;
    }
    return outsideInitial && listed * 2 > community.size();
}

// More keys weigh more; among tables of as many keys, the one with the keys earlier in the sorted
// union of the two sets. Only compared between community tables: the regular weight is not involved
constexpr size_t kCommunityWeightPositions = 58;

inline CommunityWeight communityTableWeight(const std::set<PublicKey>& initial, const std::set<PublicKey>& community,
                                            const std::vector<PublicKey>& table) {
    std::set<PublicKey> all(initial);
    all.insert(community.begin(), community.end());
    CommunityWeight weight = CommunityWeight(table.size()) << kCommunityWeightPositions;
    size_t index = 0;
    for (const auto& key : all) {
        if (index >= kCommunityWeightPositions) {
            break;
        }
        for (const auto& member : table) {
            if (member == key) {
                weight += CommunityWeight(1) << (kCommunityWeightPositions - 1 - index);
                break;
            }
        }
        ++index;
    }
    return weight;
}

// The fallback's state on one node; times in ms of a steady clock
class CommunityBootstrapGate {
public:
    // a block written, a round of a round package, a regular bootstrap table: the network is alive.
    // The node's start counts too, so that it has watched for stall_minutes before it may act
    void onProgress(uint64_t nowMs) {
        lastProgressMs_ = nowMs;
        taken_ = false;
        lastWeight_ = 0;
        lastTakenMs_ = 0;
    }

    // the node's last block, read whenever the fallback is considered: a new one is progress
    void onLastSequence(Sequence seq, uint64_t nowMs) {
        if (seq != lastSeq_) {
            lastSeq_ = seq;
            onProgress(nowMs);
        }
    }

    uint64_t msSinceProgress(uint64_t nowMs) const {
        return nowMs > lastProgressMs_ ? nowMs - lastProgressMs_ : 0;
    }

    bool isStalled(uint64_t nowMs, uint64_t stallMs) const {
        return stallMs > 0 && msSinceProgress(nowMs) >= stallMs;
    }

    // a community table of that weight may be started or taken now: the first one in this stall, one
    // heavier than the last, or any once half a round_elapse_time has passed since the last (a starter
    // repeats its table every round_elapse_time)
    bool mayTake(CommunityWeight weight, uint64_t nowMs, uint64_t roundElapseMs) const {
        if (!taken_ || weight > lastWeight_) {
            return true;
        }
        return nowMs >= lastTakenMs_ && nowMs - lastTakenMs_ >= roundElapseMs / 2;
    }

    void onTaken(CommunityWeight weight, uint64_t nowMs) {
        taken_ = true;
        lastWeight_ = weight;
        lastTakenMs_ = nowMs;
    }

private:
    uint64_t lastProgressMs_ = 0;
    Sequence lastSeq_ = 0;
    bool taken_ = false;
    CommunityWeight lastWeight_ = 0;
    uint64_t lastTakenMs_ = 0;
};

// the fallback may act on this node: a list configured, the node stalled for stall_minutes, and no
// neighbour ahead of it
inline bool isCommunityFallbackOpen(bool configured, bool stalled, bool neighbourAhead) {
    return configured && stalled && !neighbourAhead;
}

}  // namespace cs

#endif  // COMMUNITYBOOTSTRAP_HPP
