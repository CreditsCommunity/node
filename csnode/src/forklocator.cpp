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
    (void)now;
    status_ = Status::Searching;
    peer_ = peer;
    upper_ = upper;
    const cs::Sequence byDepth = upper > settings_.maxDepth ? upper - settings_.maxDepth : 0;
    floor_ = std::min(std::max(floor, byDepth), upper);
    lo_.reset();
    hi_ = upper + 1;
    known_.clear();
    inFlight_.clear();
    sentAt_ = {};
    lastSent_ = {};
    everSent_ = false;
    attempts_ = 0;
    duplicates_ = 0;
    requests_ = 0;
    forkPoint_ = 0;
    forkPointHash_.clear();
    verified_ = false;
    why_.clear();
}

void ForkLocator::stop() {
    status_ = Status::Idle;
    inFlight_.clear();
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
        if (++attempts_ >= settings_.maxAttempts) {
            fail("no reply from the peer");
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
        if (++attempts_ >= settings_.maxAttempts) {
            fail("the peer is not connected");
        }
        return;
    }
    inFlight_ = probes;
    sentAt_ = now;
    ++requests_;
}

bool ForkLocator::expects(const cs::PublicKey& sender, const std::vector<cs::Sequence>& replySeqs) const {
    if (status_ != Status::Searching || inFlight_.empty() || sender != peer_ || replySeqs.empty()) {
        return false;
    }
    for (const auto s : replySeqs) {
        if (std::find(inFlight_.begin(), inFlight_.end(), s) == inFlight_.end()) {
            return false;
        }
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
        if (++attempts_ >= settings_.maxAttempts) {
            fail("no usable reply from the peer");
        }
        return;
    }
    attempts_ = 0;
    evaluate();
}

void ForkLocator::onRefused(const cs::PublicKey& sender, Clock::time_point now, bool duplicate) {
    (void)now;
    if (status_ != Status::Searching || inFlight_.empty() || sender != peer_) {
        return;
    }
    inFlight_.clear();
    if (duplicate) {
        if (++duplicates_ >= settings_.maxDuplicates) {
            fail("the peer refuses the requests as duplicates");
        }
        return;
    }
    if (++attempts_ >= settings_.maxAttempts) {
        fail("the peer refuses the requests");
    }
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
            fail("the peer's blocks #" + std::to_string(it->first) + " and #" + std::to_string(next->first) + " are not chained");
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
        fail("the peer's block #" + std::to_string(*lo) + " is ours, but the chains differ at #" + std::to_string(hi) +
             " below it");
        return;
    }
    lo_ = lo;
    hi_ = hi;
    if (!lo_.has_value() && hi_ <= floor_) {
        status_ = Status::Deeper;
        forkPoint_ = floor_;
        return;
    }
    if (lo_.has_value() && hi_ == *lo_ + 1) {
        const auto first = known_.find(hi_);
        if (first == known_.end()) {
            return;  // the next request asks for it
        }
        const std::string ours = hooks_.ourHash ? hooks_.ourHash(*lo_) : std::string{};
        if (ours.empty() || first->second.previous != ours) {
            fail("the peer's block #" + std::to_string(hi_) + " is not built on our block #" + std::to_string(*lo_));
            return;
        }
        status_ = Status::Exact;
        forkPoint_ = *lo_;
        forkPointHash_ = ours;
        verified_ = first->second.verified;
    }
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
