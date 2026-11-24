#include <csnode/roundlag.hpp>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <sstream>

namespace cs::roundlag {

namespace {
std::string utc(int64_t unixSeconds) {
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

std::string minutes(int64_t seconds) {
    return std::to_string((seconds + 30) / 60) + " min";
}
}  // namespace

bool quorumAhead(uint64_t ownLastSeq, const std::vector<uint64_t>& neighbourLastSeqs, size_t* ahead) {
    size_t count = 0;
    for (const auto seq : neighbourLastSeqs) {
        if (seq >= ownLastSeq + 2) {
            ++count;
        }
    }
    if (ahead != nullptr) {
        *ahead = count;
    }
    return count >= 3 && 2 * count > neighbourLastSeqs.size();
}

Monitor::Monitor(Settings settings)
: settings_(settings) {
}

void Monitor::setSettings(const Settings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
}

void Monitor::pruneLocked(int64_t nowSeconds) const {
    for (auto it = episodes_.begin(); it != episodes_.end();) {
        it = nowSeconds - it->second.time > settings_.window ? episodes_.erase(it) : std::next(it);
    }
}

size_t Monitor::confirmedLocked() const {
    size_t n = 0;
    for (const auto& [round, e] : episodes_) {
        n += e.confirmed ? 1 : 0;
    }
    return n;
}

bool Monitor::onLag(int64_t nowSeconds, uint64_t round, const std::string& sender, bool confirmed) {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(nowSeconds);
    auto res = episodes_.try_emplace(round);
    auto& e = res.first->second;
    if (res.second) {
        e.time = nowSeconds;
    }
    // a bounded set: the senders of one round are at most the network
    if (e.senders.size() < 64) {
        e.senders.insert(sender);
    }
    e.confirmed = e.confirmed || confirmed;
    return res.second;
}

Settings settingsFromText(const char* text) {
    Settings s;
    if (text == nullptr) {
        return s;
    }
    long long v[5] = {0, 0, 0, 0, 0};
    if (std::sscanf(text, "%lld,%lld,%lld,%lld,%lld", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5) {
        return s;
    }
    for (const auto x : v) {
        if (x <= 0) {
            return Settings{};
        }
    }
    s.window = v[0];
    s.episodes = static_cast<size_t>(v[1]);
    s.persistAfter = v[2];
    s.warnEvery = v[3];
    s.resetEvery = v[4];
    return s;
}

Tick Monitor::tick(int64_t nowSeconds, bool resetAllowed) {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(nowSeconds);
    while (!resetTimes_.empty() && nowSeconds - resetTimes_.front() >= 3600) {
        resetTimes_.pop_front();
    }
    Tick result;
    const size_t count = confirmedLocked();
    const bool lagNow = count >= settings_.episodes;

    if (!lagNow) {
        if (persistent_) {
            std::ostringstream os;
            os << "ROUND LAG over: " << count << " lag episode(s) in the last " << minutes(settings_.window) << " after "
               << minutes(nowSeconds - lagSince_) << " of lag";
            if (resets_ > 0) {
                os << " and " << resets_ << " network reset(s)";
            }
            result.text = os.str();
            result.statusChanged = true;
        }
        else if (lagSince_ != 0) {
            result.statusChanged = true;
        }
        lagSince_ = 0;
        persistent_ = false;
        resets_ = 0;
        return result;
    }

    if (lagSince_ == 0) {
        lagSince_ = nowSeconds;
        result.statusChanged = true;
    }
    if (nowSeconds - lagSince_ < settings_.persistAfter) {
        return result;
    }

    if (!persistent_) {
        persistent_ = true;
        result.statusChanged = true;
    }

    // the first reset of a persistent lag at once; then resetEvery, doubled after each reset of this
    // lag up to resetMaxInterval; never more than resetsPerHour in the last hour
    int64_t interval = settings_.resetEvery;
    for (size_t i = 1; i < resets_ && interval < settings_.resetMaxInterval; ++i) {
        interval *= 2;
    }
    interval = std::min(interval, settings_.resetMaxInterval);
    const bool resetDue = (resets_ == 0 ? (lastReset_ == 0 || nowSeconds - lastReset_ >= settings_.resetEvery)
                                        : nowSeconds - lastReset_ >= interval) &&
                          resetTimes_.size() < settings_.resetsPerHour && resetAllowed;
    const bool warnDue = lastWarning_ == 0 || nowSeconds - lastWarning_ >= settings_.warnEvery;
    if (!resetAllowed) {
        if (!warnDue) {
            return result;
        }
        std::ostringstream held;
        held << "ROUND LAG: " << count << " confirmed lag episodes in the last " << minutes(settings_.window) << ", for "
             << minutes(nowSeconds - lagSince_) << ", while the node holds a fork (FORK HELD): no network reset, "
             << "its neighbours are on the chain it refuses";
        lastWarning_ = nowSeconds;
        result.warning = true;
        result.text = held.str();
        return result;
    }
    if (!resetDue && !warnDue) {
        return result;
    }

    std::ostringstream os;
    os << "ROUND LAG: " << count << " confirmed lag episodes in the last " << minutes(settings_.window) << ", for "
       << minutes(nowSeconds - lagSince_) << " (round packages arrive late or not at all, blocks come by SYNC): ";
    if (resetDue) {
        lastReset_ = nowSeconds;
        ++resets_;
        ++totalResets_;
        resetTimes_.push_back(nowSeconds);
        result.reset = true;
        os << "rebuilding the network sessions, as a restart would (connections dropped, peers looked up again; reset "
           << resets_ << "; reason: " << count << " rounds confirmed behind by a quorum of neighbours)";
    }
    else {
        int64_t next = settings_.resetEvery;
        for (size_t i = 1; i < resets_ && next < settings_.resetMaxInterval; ++i) {
            next *= 2;
        }
        next = std::min(next, settings_.resetMaxInterval) - (nowSeconds - lastReset_);
        os << "the network sessions were rebuilt " << minutes(nowSeconds - lastReset_) << " ago (" << resets_
           << " reset(s))";
        if (next > 0) {
            os << ", next in " << minutes(next) << " at the earliest";
        }
        os << "; if this goes on, restart the node and check its network";
    }
    lastWarning_ = nowSeconds;
    result.warning = true;
    result.text = os.str();
    result.statusChanged = true;
    return result;
}

bool Monitor::lagging() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lagSince_ != 0;
}

bool Monitor::persistent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return persistent_;
}

size_t Monitor::resets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resets_;
}

size_t Monitor::episodesInWindow(int64_t nowSeconds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(nowSeconds);
    return confirmedLocked();
}

size_t Monitor::seenInWindow(int64_t nowSeconds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(nowSeconds);
    return episodes_.size();
}

std::string Monitor::statusJson(int64_t wallUnix, int64_t nowSeconds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(nowSeconds);
    std::ostringstream os;
    os << "{\n";
    os << "    \"lagging\": \"" << (lagSince_ != 0 ? "true" : "false") << "\",\n";
    os << "    \"persistent\": \"" << (persistent_ ? "true" : "false") << "\",\n";
    if (lagSince_ != 0) {
        os << "    \"since\": \"" << utc(wallUnix - (nowSeconds - lagSince_)) << "\",\n";
    }
    os << "    \"episodes\": \"" << confirmedLocked() << "\",\n";
    os << "    \"unconfirmed\": \"" << episodes_.size() - confirmedLocked() << "\",\n";
    os << "    \"window_minutes\": \"" << settings_.window / 60 << "\",\n";
    os << "    \"resets\": \"" << resets_ << "\",\n";
    os << "    \"resets_total\": \"" << totalResets_ << "\"";
    if (lastReset_ != 0) {
        os << ",\n    \"last_reset\": \"" << utc(wallUnix - (nowSeconds - lastReset_)) << "\"";
    }
    os << "\n}\n";
    return os.str();
}

}  // namespace cs::roundlag
