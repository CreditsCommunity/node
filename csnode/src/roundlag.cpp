#include <csnode/roundlag.hpp>

#include <ctime>
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

Monitor::Monitor(Settings settings)
: settings_(settings) {
}

void Monitor::pruneLocked(int64_t nowSeconds) const {
    while (!episodes_.empty() && nowSeconds - episodes_.front().first > settings_.window) {
        episodes_.pop_front();
    }
}

bool Monitor::onLag(int64_t nowSeconds, uint64_t round) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (haveRound_ && round == lastRound_) {
        return false;
    }
    haveRound_ = true;
    lastRound_ = round;
    episodes_.emplace_back(nowSeconds, round);
    pruneLocked(nowSeconds);
    return true;
}

Tick Monitor::tick(int64_t nowSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(nowSeconds);
    Tick result;
    const size_t count = episodes_.size();
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

    const bool resetDue = lastReset_ == 0 || nowSeconds - lastReset_ >= settings_.resetEvery;
    const bool warnDue = lastWarning_ == 0 || nowSeconds - lastWarning_ >= settings_.warnEvery;
    if (!resetDue && !warnDue) {
        return result;
    }

    std::ostringstream os;
    os << "ROUND LAG: " << count << " lag episodes in the last " << minutes(settings_.window) << ", for "
       << minutes(nowSeconds - lagSince_) << " (round packages arrive late or not at all, blocks come by SYNC): ";
    if (resetDue) {
        lastReset_ = nowSeconds;
        ++resets_;
        ++totalResets_;
        result.reset = true;
        os << "rebuilding the network sessions, as a restart would (connections dropped, peers looked up again; reset "
           << resets_ << ")";
    }
    else {
        os << "the network sessions were rebuilt " << minutes(nowSeconds - lastReset_) << " ago (" << resets_
           << " reset(s)), next in " << minutes(settings_.resetEvery - (nowSeconds - lastReset_))
           << "; if this goes on, restart the node and check its network";
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
    os << "    \"episodes\": \"" << episodes_.size() << "\",\n";
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
