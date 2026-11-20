#include <csnode/peerstatus.hpp>

#include <sstream>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <base58.h>

#include <csnode/forkguardstate.hpp>

namespace cs::peerstatus {

Kind classify(uint16_t version, uint16_t minorVersion) {
    if (version == 0) {
        return Kind::Unknown;
    }
    return minorVersion == 0 ? Kind::Official : Kind::Community;
}

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Official:
            return "official";
        case Kind::Community:
            return "community";
        default:
            return "unknown";
    }
}

unsigned percent(size_t part, size_t total) {
    if (total == 0) {
        return 0;
    }
    return static_cast<unsigned>((part * 200 + total) / (2 * total));
}

Tracker::Tracker(Settings settings)
: settings_(settings) {
}

void Tracker::setSelf(const cs::PublicKey& key, Kind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_ = key;
    selfKind_ = kind;
}

void Tracker::setKinds(std::map<cs::PublicKey, Kind> kinds) {
    std::map<std::string, Kind> byName;
    for (const auto& [key, kind] : kinds) {
        byName.emplace(EncodeBase58(key.data(), key.data() + key.size()), kind);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    kinds_ = std::move(kinds);
    kindsBase58_ = std::move(byName);
}

Kind Tracker::kindOf(const cs::PublicKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == self_) {
        return selfKind_;
    }
    const auto it = kinds_.find(key);
    return it == kinds_.end() ? Kind::Unknown : it->second;
}

Kind Tracker::kindOfBase58(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = kindsBase58_.find(key);
    return it == kindsBase58_.end() ? Kind::Unknown : it->second;
}

void Tracker::addBlock(cs::Sequence seq, const std::vector<cs::PublicKey>& confidants, uint64_t realTrusted) {
    Block b;
    b.seq = seq;
    for (size_t i = 0; i < confidants.size() && i < 64; ++i) {
        if ((realTrusted >> i) & 1U) {
            b.trusted.push_back(confidants[i]);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& k : b.trusted) {
        if (k == self_) {
            b.self = true;
            break;
        }
    }
    // a block stored again after a rollback replaces the old one and anything above it
    while (!blocks_.empty() && blocks_.back().seq >= seq) {
        blocks_.pop_back();
    }
    blocks_.push_back(std::move(b));
    while (blocks_.size() > settings_.window) {
        blocks_.pop_front();
    }
}

void Tracker::removeBlock(cs::Sequence seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!blocks_.empty() && blocks_.back().seq >= seq) {
        blocks_.pop_back();
    }
}

SeatCounts Tracker::seats(size_t rounds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seatsLocked(rounds);
}

SeatCounts Tracker::seatsLocked(size_t rounds) const {
    SeatCounts c;
    const size_t n = (rounds == 0 || rounds > blocks_.size()) ? blocks_.size() : rounds;
    for (size_t i = blocks_.size() - n; i < blocks_.size(); ++i) {
        const auto& b = blocks_[i];
        if (c.rounds == 0) {
            c.first = b.seq;
        }
        c.last = b.seq;
        ++c.rounds;
        if (b.self) {
            ++c.selfTrusted;
        }
        for (const auto& k : b.trusted) {
            ++c.seats;
            Kind kind = Kind::Unknown;
            if (k == self_) {
                kind = selfKind_;
            }
            else if (const auto it = kinds_.find(k); it != kinds_.end()) {
                kind = it->second;
            }
            switch (kind) {
                case Kind::Official:
                    ++c.official;
                    break;
                case Kind::Community:
                    ++c.community;
                    break;
                default:
                    ++c.unknown;
                    break;
            }
        }
    }
    return c;
}

NeighbourCounts Tracker::countNeighbours(const std::vector<cs::PublicKey>& neighbours) const {
    NeighbourCounts n;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& k : neighbours) {
        ++n.total;
        const auto it = kinds_.find(k);
        switch (it == kinds_.end() ? Kind::Unknown : it->second) {
            case Kind::Official:
                ++n.official;
                break;
            case Kind::Community:
                ++n.community;
                break;
            default:
                ++n.unknown;
                break;
        }
    }
    return n;
}

std::string Tracker::peersLine(const NeighbourCounts& neighbours) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peersLineLocked(neighbours);
}

std::string Tracker::peersLineLocked(const NeighbourCounts& n) const {
    std::ostringstream os;
    os << "PEERS: " << n.total << " neighbours (" << n.official << " official, " << n.community << " community";
    if (n.unknown != 0) {
        os << ", " << n.unknown << " unknown";
    }
    os << "); ";
    const auto c = seatsLocked(0);
    if (c.rounds == 0 || c.seats == 0) {
        os << "no stored blocks yet";
        return os.str();
    }
    os << "last " << c.rounds << " rounds: trusted seats " << percent(c.official, c.seats) << "% official, "
       << percent(c.community, c.seats) << "% community";
    if (c.unknown != 0) {
        os << ", " << percent(c.unknown, c.seats) << "% unknown";
    }
    os << "; this node trusted in " << percent(c.selfTrusted, c.rounds) << "%";
    return os.str();
}

std::vector<Line> Tracker::tick(int64_t now, const NeighbourCounts& neighbours) {
    std::vector<Line> out;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
        started_ = true;
        startedAt_ = now;
        lastOfficialNeighbour_ = now;
    }
    lastNeighbours_ = neighbours;
    if (neighbours.official != 0) {
        lastOfficialNeighbour_ = now;
        officialNeighbourSeen_ = true;
    }

    // gone: by the neighbours (none official for goneSeconds) or by the trusted seats (none official
    // in the last goneRounds, with at least half of the seats classified: unknown keys may be official)
    const bool byNeighbours = now - lastOfficialNeighbour_ >= settings_.goneSeconds;
    const auto recent = seatsLocked(settings_.goneRounds);
    const bool bySeats = recent.rounds >= settings_.goneRounds && recent.seats != 0 && recent.official == 0 &&
                         2 * (recent.official + recent.community) >= recent.seats;
    const bool gone = byNeighbours || bySeats;

    const auto minutes = [](int64_t s) {
        return std::to_string(s / 60) + " min";
    };
    const auto all = seatsLocked(0);
    const auto composition = [&]() {
        std::ostringstream os;
        os << "community nodes keep the network going: " << neighbours.community << " community of " << neighbours.total
           << " neighbours";
        if (all.seats != 0) {
            os << ", " << percent(all.community, all.seats) << "% of the trusted seats of the last " << all.rounds << " rounds";
        }
        return os.str();
    };

    if (gone) {
        std::string reason;
        if (byNeighbours) {
            reason = officialNeighbourSeen_ ? "no official neighbour for " + minutes(now - lastOfficialNeighbour_)
                                            : "no official neighbour seen since the start (" + minutes(now - startedAt_) + ")";
        }
        if (bySeats) {
            if (!reason.empty()) {
                reason += " and ";
            }
            reason += "no official node among the trusted of the last " + std::to_string(recent.rounds) + " rounds";
        }
        if (!gone_) {
            gone_ = true;
            goneSince_ = now;
            goneAlerted_ = false;
        }
        goneReason_ = reason;
        // once on the transition, then at most every repeatEvery (also across a quick back and forth)
        if (now - lastGoneAlert_ >= settings_.repeatEvery) {
            std::string text = "OFFICIAL NODES GONE";
            if (goneAlerted_) {
                text += " (still, for " + minutes(now - goneSince_) + ")";
            }
            text += ": " + reason + "; " + composition();
            out.push_back(Line{true, text});
            lastAlert_ = text;
            goneAlerted_ = true;
            lastGoneAlert_ = now;
        }
    }
    else if (gone_) {
        gone_ = false;
        goneReason_.clear();
        // a quick return after a gone alert is still said, but not more than once per repeatEvery
        if (goneAlerted_ && now - lastBackAlert_ >= settings_.repeatEvery) {
            std::ostringstream os;
            os << "OFFICIAL NODES BACK after " << minutes(now - goneSince_) << ": " << neighbours.official
               << " official of " << neighbours.total << " neighbours, " << percent(recent.official, recent.seats)
               << "% of the trusted seats of the last " << recent.rounds << " rounds";
            out.push_back(Line{false, os.str()});
            lastAlert_ = os.str();
            lastBackAlert_ = now;
        }
        goneAlerted_ = false;
    }

    const auto longRun = seatsLocked(settings_.communityOnlyRounds);
    const bool communityOnly = longRun.rounds >= settings_.communityOnlyRounds && longRun.seats != 0 &&
                               longRun.community == longRun.seats;
    if (communityOnly) {
        communityOnly_ = true;
        if (now - lastCommunityOnlyAlert_ >= settings_.repeatEvery) {
            std::ostringstream os;
            os << "TRUSTED SEATS COMMUNITY-ONLY: every trusted seat of the last " << longRun.rounds << " rounds (blocks #"
               << longRun.first << "-#" << longRun.last << ") was a community node; no official node takes part in consensus";
            out.push_back(Line{true, os.str()});
            lastAlert_ = os.str();
            communityOnlyAlerted_ = true;
            lastCommunityOnlyAlert_ = now;
        }
    }
    else if (communityOnly_) {
        communityOnly_ = false;
        if (communityOnlyAlerted_) {
            out.push_back(Line{false, "TRUSTED SEATS MIXED again: official or unknown nodes among the trusted of the last " +
                                          std::to_string(longRun.rounds) + " rounds"});
        }
        communityOnlyAlerted_ = false;
    }

    // the PEERS line first: at the first tick, then every summaryEvery
    if (now - lastSummary_ >= settings_.summaryEvery) {
        out.insert(out.begin(), Line{false, peersLineLocked(neighbours)});
        lastSummary_ = now;
    }
    return out;
}

bool Tracker::officialGone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gone_;
}

bool Tracker::communityOnly() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return communityOnly_;
}

std::string Tracker::statusJson(int64_t wallUnix) const {
    using boost::property_tree::ptree;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto c = seatsLocked(0);
    const auto& n = lastNeighbours_;
    ptree tree;
    tree.put("updated", cs::forkguard::utcTime(wallUnix));
    tree.put("updated_unix", wallUnix);
    tree.put("summary", peersLineLocked(n));
    tree.put("neighbours", n.total);
    tree.put("neighbours_official", n.official);
    tree.put("neighbours_community", n.community);
    tree.put("neighbours_unknown", n.unknown);
    tree.put("window_rounds", c.rounds);
    tree.put("window_first_block", c.first);
    tree.put("window_last_block", c.last);
    tree.put("seats_official_pct", percent(c.official, c.seats));
    tree.put("seats_community_pct", percent(c.community, c.seats));
    tree.put("seats_unknown_pct", percent(c.unknown, c.seats));
    tree.put("self_trusted_pct", percent(c.selfTrusted, c.rounds));
    tree.put("official_gone", gone_);
    tree.put("official_gone_reason", goneReason_);
    tree.put("community_only", communityOnly_);
    tree.put("last_alert", lastAlert_);
    std::ostringstream os;
    boost::property_tree::write_json(os, tree);
    return os.str();
}

}  // namespace cs::peerstatus
