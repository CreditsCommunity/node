#ifndef COMMUNITYBOOTSTRAP_HPP
#define COMMUNITYBOOTSTRAP_HPP

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <cstdint>
#include <set>
#include <vector>

#include <cscrypto/cscrypto.hpp>
#include <csnode/odatastream.hpp>
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
// - the node itself has seen no block, no round package whose signatures it accepted and no regular
//   bootstrap table for stall_minutes (a jump of its clocks counts as progress), and no neighbour
//   reports a block two above its own, or one above for less than a further stall_minutes
//   (isNeighbourAhead: this node would sync);
// - the table: 4 to 25 keys in ascending order, each an initial confidant or listed, at least one
//   of them listed and not an initial confidant (else it is a regular table), more than half of the
//   list in it (two parts of a split community cannot both restart), and signed by its first key, the
//   starter;
// - the round window (#71) and the stall check (#70) of every unsigned table still apply;
// - a node takes one such table per half round_elapse_time; within that window only a heavier table
//   whose starter key is not above the current one replaces it, and a regular table always replaces
//   it (it goes through the unchanged path, whose weight it does not touch). A regular table, even
//   a project-signed one, is progress: a community table may follow it only after a further
//   stall_minutes without a block.
// Official nodes, and community nodes without the list, drop such a table: one of its keys is not an
// initial confidant and its signature is not the project's.

constexpr size_t kCommunityMinTable = 4;   // Consensus::MinTrustedNodes
constexpr size_t kCommunityMaxTable = 25;  // Consensus::MaxTrustedNodes

using CommunityWeight = uint64_t;

// The bytes a project-signed table and a community table sign: the round, then the table as sent
// (the count and the keys). The round is in them, so a table of another round does not verify
inline cs::Bytes bootstrapSignedBytes(cs::RoundNumber round, const uint8_t* table, size_t tableLen) {
    cs::Bytes buf;
    buf.reserve(sizeof(cs::RoundNumber) + tableLen);
    const auto* ptr = reinterpret_cast<const uint8_t*>(&round);
    buf.insert(buf.end(), ptr, ptr + sizeof(cs::RoundNumber));
    buf.insert(buf.end(), table, table + tableLen);
    return buf;
}

// A community table as its starter sends it: the count, the keys in ascending order, the starter's
// signature over bootstrapSignedBytes, and 4 random bytes as in a regular table. Without them a
// repeat in the same round would be the same message, and the p2p layer drops a broadcast whose
// bytes it has seen (Host::IsDuplicate): a node whose fallback opens after the first one would never
// get one. Receivers read the signature and ignore the bytes after it
inline cs::Bytes makeCommunityTable(cs::RoundNumber round, const std::set<PublicKey>& keys, const cs::PrivateKey& starterKey,
                                    uint32_t random) {
    cs::Bytes bin;
    cs::ODataStream out(bin);
    out << uint8_t(keys.size());
    for (const auto& key : keys) {
        out << key;
    }
    const cs::Bytes signedPart = bootstrapSignedBytes(round, bin.data(), bin.size());
    out << cscrypto::generateSignature(starterKey, signedPart.data(), signedPart.size());
    out << random;
    return bin;
}

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

// The starter of an even set drops its last key (the official rule, for an odd table). Not when that
// would leave a community table without more than half of the list: with a list of 6 and 4 of its
// nodes left, the 3 kept would never be enough. A community table whose last key is its only one
// outside the initial confidants drops it: the rest is a regular table, as the official rule gives
inline bool dropForOddTable(const std::set<PublicKey>& initial, const std::set<PublicKey>& community,
                            const std::vector<PublicKey>& table) {
    if (table.empty() || table.size() % 2 != 0) {
        return false;
    }
    if (!isCommunityTable(initial, community, table)) {
        return true;
    }
    const std::vector<PublicKey> rest(table.begin(), table.end() - 1);
    return isCommunityTable(initial, community, rest)
           || std::none_of(rest.cbegin(), rest.cend(), [&initial](const PublicKey& key) { return initial.count(key) == 0; });
}

// The table a starter sends from the keys at the highest sequence it sees (its own included), after
// the odd rule. A set with a listed key outside the initial confidants that is not a community table
// (not more than half of the list) cannot start anything as it is; it must not hold back the
// regular restart either, which the initial confidants among it would make without the list: the
// table is then theirs, under the official odd rule, and a node outside them waits as before
inline std::set<PublicKey> bootstrapTableOf(const std::set<PublicKey>& initial, const std::set<PublicKey>& community,
                                            const std::set<PublicKey>& seen) {
    const auto oddTable = [&initial](const std::set<PublicKey>& listed, std::set<PublicKey> keys) {
        if (dropForOddTable(initial, listed, std::vector<PublicKey>(keys.cbegin(), keys.cend()))) {
            keys.erase(std::prev(keys.end()));
        }
        return keys;
    };
    std::set<PublicKey> table = oddTable(community, seen);
    const std::vector<PublicKey> asSent(table.cbegin(), table.cend());
    const bool outsideInitial = std::any_of(asSent.cbegin(), asSent.cend(), [&initial](const PublicKey& key) {
        return initial.count(key) == 0;
    });
    if (!outsideInitial || isCommunityTable(initial, community, asSent)) {
        return table;
    }
    std::set<PublicKey> regular;
    for (const auto& key : seen) {
        if (initial.count(key) > 0) {
            regular.insert(key);
        }
    }
    return oddTable({}, regular);
}

// A neighbour ahead of this node: it syncs from it, the network is not stalled. Two blocks ahead
// always; one block ahead only for a further stall_minutes (sync takes one block in seconds, so a
// neighbour that stays one block ahead cannot give it, and must not keep the fallback closed for ever)
inline bool isNeighbourAhead(Sequence neighbourSeq, Sequence lastSeq, uint64_t msSinceProgress, uint64_t stallMs) {
    if (neighbourSeq >= lastSeq + 2) {
        return true;
    }
    return neighbourSeq == lastSeq + 1 && msSinceProgress < 2 * stallMs;
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
    // a block written, a round package with accepted signatures, a regular bootstrap table: the network is alive.
    // The node's start counts too, so that it has watched for stall_minutes before it may act
    void onProgress(uint64_t nowMs) {
        lastProgressMs_ = nowMs;
        taken_ = false;
        lastWeight_ = 0;
        lastTakenMs_ = 0;
    }

    // the clocks whenever the fallback is considered: a jump of either over maxGapMs since the last
    // look (the host suspended and resumed, the wall clock set) is progress, so that the node watches
    // again for stall_minutes before it acts on heights it saw before the jump
    void onClock(uint64_t steadyMs, uint64_t wallMs, uint64_t maxGapMs) {
        const bool jump = checked_ && (gap(lastCheckSteadyMs_, steadyMs) > maxGapMs || gap(lastCheckWallMs_, wallMs) > maxGapMs);
        checked_ = true;
        lastCheckSteadyMs_ = steadyMs;
        lastCheckWallMs_ = wallMs;
        if (jump) {
            onProgress(steadyMs);
        }
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

    // a community table of that weight and starter (its first key) may be started or taken now: the
    // first one in this stall, any once half a round_elapse_time has passed since the last (a starter
    // repeats its table every round_elapse_time), and within that half only a heavier one whose
    // starter key is not above the last one's. The starter of a table is its lowest key, and among
    // the nodes left the lowest key starts anyway: a node with a lower key that saw more nodes
    // replaces the table at once, but a listed node with a higher key cannot displace a running table
    // by padding its own with keys of stopped nodes (heavier, and unable to decide a round)
    bool mayTake(CommunityWeight weight, const PublicKey& starter, uint64_t nowMs, uint64_t roundElapseMs) const {
        if (!taken_) {
            return true;
        }
        if (nowMs >= lastTakenMs_ && nowMs - lastTakenMs_ >= roundElapseMs / 2) {
            return true;
        }
        return weight > lastWeight_ && !(lastStarter_ < starter);
    }

    void onTaken(CommunityWeight weight, const PublicKey& starter, uint64_t nowMs) {
        taken_ = true;
        lastWeight_ = weight;
        lastStarter_ = starter;
        lastTakenMs_ = nowMs;
    }

private:
    static uint64_t gap(uint64_t a, uint64_t b) {
        return a > b ? a - b : b - a;
    }

    uint64_t lastProgressMs_ = 0;
    Sequence lastSeq_ = 0;
    bool taken_ = false;
    CommunityWeight lastWeight_ = 0;
    PublicKey lastStarter_{};
    uint64_t lastTakenMs_ = 0;
    bool checked_ = false;
    uint64_t lastCheckSteadyMs_ = 0;
    uint64_t lastCheckWallMs_ = 0;
};

// the fallback may act on this node: a list configured, the node stalled for stall_minutes, and no
// neighbour ahead of it
inline bool isCommunityFallbackOpen(bool configured, bool stalled, bool neighbourAhead) {
    return configured && stalled && !neighbourAhead;
}

}  // namespace cs

#endif  // COMMUNITYBOOTSTRAP_HPP
