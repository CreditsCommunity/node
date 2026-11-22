#include <csnode/forklocator.hpp>

#include <algorithm>
#include <iterator>
#include <string>

namespace cs::forkguard {

ForkLocator::ForkLocator(Hooks hooks, Settings settings)
: hooks_(std::move(hooks))
, settings_(settings) {
    settings_.probesPerRequest = std::max<size_t>(settings_.probesPerRequest, 2);
    settings_.maxAttempts = std::max<size_t>(settings_.maxAttempts, 1);
}

void ForkLocator::start(const cs::PublicKey& peer, cs::Sequence upper, cs::Sequence floor, Clock::time_point now) {
    start(std::vector<cs::PublicKey>{peer}, upper, floor, now);
}

void ForkLocator::start(const std::vector<cs::PublicKey>& candidates, cs::Sequence upper, cs::Sequence floor,
                        Clock::time_point now) {
    (void)now;
    candidates_.clear();
    for (const auto& c : candidates) {
        if (candidates_.size() >= std::max<size_t>(settings_.maxCandidates, 1)) {
            break;
        }
        if (std::find(candidates_.begin(), candidates_.end(), c) == candidates_.end()) {
            candidates_.push_back(c);
        }
    }
    failures_.assign(candidates_.size(), 0);
    spent_.assign(candidates_.size(), false);
    current_ = 0;
    rotations_ = 0;
    hint_.reset();
    resultPeer_ = {};
    peer_ = candidates_.empty() ? cs::PublicKey{} : candidates_.front();
    status_ = Status::Searching;
    upper_ = upper;
    const cs::Sequence byDepth = upper > settings_.maxDepth ? upper - settings_.maxDepth : 0;
    // never block 0: both chains start from the same genesis block, and a peer may serve block 0 in a
    // form that hashes differently (seen with the official node), which would read as a fork below it
    initialFloor_ = std::min(std::max({floor, byDepth, cs::Sequence{1}}), std::max(upper, cs::Sequence{1}));
    sentAt_ = {};
    lastSent_ = {};
    everSent_ = false;
    requests_ = 0;
    forkPoint_ = 0;
    forkPointHash_.clear();
    nextHash_.clear();
    verified_ = false;
    why_.clear();
    resetPeerSearch();
    if (candidates_.empty()) {
        fail("no peer to ask");
    }
}

void ForkLocator::resetPeerSearch() {
    floor_ = initialFloor_;
    lo_.reset();
    hi_ = upper_ + 1;
    known_.clear();
    inFlight_.clear();
    attempts_ = 0;
    duplicates_ = 0;
    floorRaises_ = 0;
}

void ForkLocator::stop() {
    status_ = Status::Idle;
    inFlight_.clear();
}

void ForkLocator::transientFailure(const std::string& why) {
    ++attempts_;
    if (current_ < failures_.size() && ++failures_[current_] >= settings_.maxAttempts) {
        spent_[current_] = true;
    }
    if (candidates_.size() <= 1) {
        // the only candidate: the search goes on with it, up to maxAttempts in a row
        if (attempts_ >= settings_.maxAttempts) {
            finish(why);
        }
        return;
    }
    rotate(why);
}

void ForkLocator::peerDone(const std::string& why) {
    if (current_ < spent_.size()) {
        spent_[current_] = true;
    }
    rotate(why);
}

void ForkLocator::rotate(const std::string& why) {
    why_ = why;
    inFlight_.clear();
    const size_t n = candidates_.size();
    for (size_t step = 1; step <= n; ++step) {
        const size_t next = (current_ + step) % n;
        if (spent_[next]) {
            continue;
        }
        if (next != current_) {
            current_ = next;
            peer_ = candidates_[next];
            ++rotations_;
            resetPeerSearch();
        }
        return;
    }
    finish(why);
}

void ForkLocator::finish(const std::string& why) {
    inFlight_.clear();
    if (hint_.has_value()) {
        status_ = hint_->status;
        forkPoint_ = hint_->forkPoint;
        forkPointHash_ = hint_->forkPointHash;
        nextHash_ = hint_->nextHash;
        verified_ = false;
        resultPeer_ = hint_->peer;
        return;
    }
    fail(why);
}

std::vector<cs::Sequence> ForkLocator::nextProbes() const {
    std::vector<cs::Sequence> probes;
    if (status_ != Status::Searching) {
        return probes;
    }
    const bool hiKnown = known_.count(hi_) != 0;
    // the heights still open: (lo, hi), or [floor, hi) while no match is known (the floor itself must
    // be seen to match, else the fork is deeper)
    const cs::Sequence a = lo_.has_value() ? *lo_ + 1 : floor_;
    if (hi_ == 0 || a > hi_ - 1) {
        if (!hiKnown) {
            probes.push_back(hi_);  // the first differing block, to check it is built on ours at lo
        }
        return probes;
    }
    const cs::Sequence b = hi_ - 1;
    const cs::Sequence open = b - a + 1;
    const size_t k = settings_.probesPerRequest;
    if (open + (hiKnown ? 0 : 1) <= k) {
        for (cs::Sequence s = a; s <= b; ++s) {
            probes.push_back(s);
        }
        if (!hiKnown) {
            probes.push_back(hi_);
        }
        return probes;
    }
    // k heights spread over the open range, the lowest first (the floor, while no match is known)
    for (size_t i = 0; i < k; ++i) {
        const cs::Sequence s = a + static_cast<cs::Sequence>((static_cast<uint64_t>(open) * i) / k);
        if (probes.empty() || probes.back() != s) {
            probes.push_back(s);
        }
    }
    return probes;
}

void ForkLocator::tick(Clock::time_point now) {
    if (status_ != Status::Searching) {
        return;
    }
    if (!inFlight_.empty()) {
        if (now - sentAt_ < settings_.replyTimeout) {
            return;
        }
        inFlight_.clear();
        transientFailure("no reply from the peer");
        if (status_ != Status::Searching) {
            return;
        }
    }
    if (everSent_ && now - lastSent_ < settings_.minInterval) {
        return;
    }
    const auto probes = nextProbes();
    if (probes.empty()) {
        evaluate();
        return;
    }
    everSent_ = true;
    lastSent_ = now;
    if (!hooks_.sendRequest || !hooks_.sendRequest(peer_, probes)) {
        transientFailure("the peer is not connected");
        return;
    }
    inFlight_ = probes;
    sentAt_ = now;
    ++requests_;
}

bool ForkLocator::expects(const cs::PublicKey& sender, const std::vector<cs::Sequence>& replySeqs) const {
    if (status_ != Status::Searching || inFlight_.empty() || sender != peer_ || replySeqs.empty() ||
        replySeqs.size() > inFlight_.size()) {
        return false;
    }
    std::vector<cs::Sequence> seen;
    seen.reserve(replySeqs.size());
    for (const auto s : replySeqs) {
        if (std::find(inFlight_.begin(), inFlight_.end(), s) == inFlight_.end() ||
            std::find(seen.begin(), seen.end(), s) != seen.end()) {
            return false;
        }
        seen.push_back(s);
    }
    return true;
}

void ForkLocator::onReply(const cs::PublicKey& sender, const std::vector<PeerBlock>& blocks, Clock::time_point now) {
    (void)now;
    if (status_ != Status::Searching || inFlight_.empty() || sender != peer_) {
        return;
    }
    size_t usable = 0;
    for (const auto& b : blocks) {
        if (b.hash.empty() || std::find(inFlight_.begin(), inFlight_.end(), b.seq) == inFlight_.end()) {
            continue;
        }
        known_[b.seq] = b;
        ++usable;
    }
    inFlight_.clear();
    duplicates_ = 0;
    if (usable == 0) {
        transientFailure("no usable reply from the peer");
        return;
    }
    attempts_ = 0;
    evaluate();
}

void ForkLocator::onRefused(const cs::PublicKey& sender, Clock::time_point now, Refusal why) {
    (void)now;
    if (status_ != Status::Searching || inFlight_.empty() || sender != peer_) {
        return;
    }
    const std::vector<cs::Sequence> asked = std::move(inFlight_);
    inFlight_.clear();
    if (why == Refusal::Duplicate) {
        if (++duplicates_ >= settings_.maxDuplicates) {
            peerDone("the peer refuses the requests as duplicates");
        }
        return;
    }
    if (why == Refusal::NoSuchBlocks && !lo_.has_value() && !asked.empty() && asked.front() >= floor_) {
        // the peer does not store the lowest height asked for: search above it (the heights between it
        // and the next one asked for were never asked, so skip to that one)
        if (++floorRaises_ > settings_.maxFloorRaises) {
            peerDone("the peer does not store the blocks to search");
            return;
        }
        floor_ = asked.size() > 1 ? asked[1] : asked.front() + 1;
        if (floor_ >= hi_) {
            peerDone("the peer does not store the blocks to search");
        }
        return;
    }
    transientFailure("the peer refuses the requests");
}

bool ForkLocator::matches(cs::Sequence seq) const {
    const auto it = known_.find(seq);
    if (it == known_.end() || !hooks_.ourHash) {
        return false;
    }
    const std::string ours = hooks_.ourHash(seq);
    return !ours.empty() && ours == it->second.hash;
}

void ForkLocator::fail(const std::string& why) {
    status_ = Status::Failed;
    why_ = why;
    inFlight_.clear();
}

void ForkLocator::evaluate() {
    if (status_ != Status::Searching) {
        return;
    }
    // the peer's blocks hold together: two consecutive heights are chained
    for (auto it = known_.begin(); it != known_.end(); ++it) {
        const auto next = std::next(it);
        if (next != known_.end() && next->first == it->first + 1 && next->second.previous != it->second.hash) {
            peerDone("the peer's blocks #" + std::to_string(it->first) + " and #" + std::to_string(next->first) + " are not chained");
            return;
        }
    }
    // one chain shares a prefix with ours: every match lies below every difference
    std::optional<cs::Sequence> lo = lo_;
    cs::Sequence hi = hi_;
    for (const auto& [seq, block] : known_) {
        if (matches(seq)) {
            if (!lo.has_value() || seq > *lo) {
                lo = seq;
            }
        }
        else if (seq < hi) {
            hi = seq;
        }
    }
    if (lo.has_value() && *lo >= hi) {
        peerDone("the peer's block #" + std::to_string(*lo) + " is ours, but the chains differ at #" + std::to_string(hi) +
                 " below it");
        return;
    }
    lo_ = lo;
    hi_ = hi;
    if (!lo_.has_value() && hi_ <= floor_) {
        // a hint (a peer can claim it): another candidate may still locate the point
        if (!hint_.has_value()) {
            hint_ = Hint{Status::Deeper, floor_, std::string{}, std::string{}, peer_};
        }
        peerDone("the peer's block already differs at the floor #" + std::to_string(floor_));
        return;
    }
    if (lo_.has_value() && hi_ == *lo_ + 1) {
        const auto first = known_.find(hi_);
        if (first == known_.end()) {
            return;  // the next request asks for it
        }
        const std::string ours = hooks_.ourHash ? hooks_.ourHash(*lo_) : std::string{};
        if (ours.empty() || first->second.previous != ours) {
            peerDone("the peer's block #" + std::to_string(hi_) + " is not built on our block #" + std::to_string(*lo_));
            return;
        }
        if (!first->second.verified) {
            // a hint only: kept (over a "deeper" one), and another candidate may verify the point
            if (!hint_.has_value() || hint_->status != Status::Exact) {
                hint_ = Hint{Status::Exact, *lo_, ours, first->second.hash, peer_};
            }
            peerDone("the peer's block #" + std::to_string(hi_) + " after the located point is not verified");
            return;
        }
        status_ = Status::Exact;
        forkPoint_ = *lo_;
        forkPointHash_ = ours;
        nextHash_ = first->second.hash;
        verified_ = true;
        resultPeer_ = peer_;
        inFlight_.clear();
    }
}

std::vector<cs::PublicKey> rankCandidates(const std::vector<CandidatePeer>& peers, cs::Sequence ourHead, size_t max) {
    std::vector<std::pair<int, const CandidatePeer*>> scored;
    for (const auto& p : peers) {
        if (!p.neighbour) {
            continue;
        }
        bool seen = false;
        for (auto& s : scored) {
            if (s.second->key == p.key) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        const int score = (p.official ? 4 : 0) + (p.announced > ourHead + 1 ? 2 : 0) + (p.sentRefused ? 1 : 0);
        scored.emplace_back(score, &p);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return a.second->announced > b.second->announced;
    });
    std::vector<cs::PublicKey> out;
    for (const auto& s : scored) {
        if (out.size() >= max) {
            break;
        }
        out.push_back(s.second->key);
    }
    return out;
}

cs::Sequence locateDepth(uint64_t maxAutoRollback) {
    if (maxAutoRollback == 0) {
        return 2000;
    }
    if (maxAutoRollback >= 500) {
        return 10000;
    }
    return static_cast<cs::Sequence>(std::max<uint64_t>(maxAutoRollback * 20, 1000));
}

}  // namespace cs::forkguard
