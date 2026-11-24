#pragma once

#include <cstddef>
#include <functional>
#include <vector>

// A node behind a NAT keeps a connection of its own open to the peers of its routing table (see
// Transport::keepReachable): which peers to ping in one iteration. Only the routing table's
// k-buckets are candidates (nodes whose datagrams came from the endpoint recorded, never ids that
// other nodes named), the neighbours are left out (they are pinged anyway), and at most `cap` are
// taken, from `offset` on, so that with more candidates every one gets its turn; `offset` advances.
namespace cs::keepalive {

inline constexpr size_t kMaxPeersPerIteration = 32;

template <class Key>
std::vector<Key> select(const std::vector<Key>& tablePeers, const std::function<bool(const Key&)>& isNeighbour,
                        size_t cap, size_t& offset) {
    std::vector<Key> others;
    others.reserve(tablePeers.size());
    for (const auto& k : tablePeers) {
        if (!isNeighbour(k)) {
            others.push_back(k);
        }
    }
    std::vector<Key> result;
    if (others.empty() || cap == 0) {
        return result;
    }
    const size_t n = others.size() < cap ? others.size() : cap;
    const size_t start = offset % others.size();
    result.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        result.push_back(others[(start + i) % others.size()]);
    }
    offset = (start + n) % others.size();
    return result;
}

}  // namespace cs::keepalive
