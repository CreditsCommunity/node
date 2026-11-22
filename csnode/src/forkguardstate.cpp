#include <csnode/forkguardstate.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <base58.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace cs::forkguard {

namespace fs = std::filesystem;
using boost::property_tree::ptree;

namespace {

// flushes a written file (and, on POSIX, the folder entry of a rename) to the disk
bool syncFile(std::FILE* f) {
    if (std::fflush(f) != 0) {
        return false;
    }
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

void syncDir(const fs::path& dir) {
#ifndef _WIN32
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)dir;
#endif
}

// a small file written as JSON with writeFileAtomic
bool writeTree(const std::string& dir, const char* name, const ptree& tree) {
    std::string text;
    try {
        std::ostringstream out;
        boost::property_tree::write_json(out, tree);
        text = out.str();
    }
    catch (const std::exception&) {
        return false;
    }
    return writeFileAtomic(dir, name, text);
}

// the file's tree; nothing when it does not exist. A file that exists but cannot be read sets
// *corrupt (an empty file, bad JSON, or larger than any record)
std::optional<ptree> readTree(const std::string& dir, const char* name, bool* corrupt) {
    if (corrupt) {
        *corrupt = false;
    }
    if (dir.empty()) {
        return std::nullopt;
    }
    const fs::path path = fs::path(dir) / name;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return std::nullopt;
    }
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > 64 * 1024 || fs::file_size(path, ec) == 0) {
        if (corrupt) {
            *corrupt = true;
        }
        return std::nullopt;
    }
    try {
        std::ifstream in(path);
        ptree tree;
        boost::property_tree::read_json(in, tree);
        return tree;
    }
    catch (const std::exception&) {
        if (corrupt) {
            *corrupt = true;
        }
        return std::nullopt;
    }
}

void removeFile(const std::string& dir, const char* name) {
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::remove(fs::path(dir) / name, ec);
}

}  // namespace

bool writeFileAtomic(const std::string& dir, const char* name, const std::string& text) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path target = fs::path(dir) / name;
    const fs::path tmp = fs::path(dir) / (std::string(name) + ".tmp");
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size() && syncFile(f);
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    syncDir(fs::path(dir));
    return true;
}

std::string stateDirFor(const std::string& dbPath) {
    fs::path p(dbPath);
    // "db/" and "db" name the same folder
    while (!p.empty() && !p.has_filename() && p.has_parent_path() && p != p.parent_path()) {
        p = p.parent_path();
    }
    const fs::path parent = p.parent_path();
    return parent.empty() ? std::string(".") : parent.string();
}

bool saveEpisode(const std::string& dir, const Episode& episode) {
    ptree tree;
    tree.put("high_water", episode.highWater);
    return writeTree(dir, kEpisodeFile, tree);
}

std::optional<Episode> loadEpisode(const std::string& dir, bool* corrupt) {
    const auto tree = readTree(dir, kEpisodeFile, corrupt);
    if (!tree) {
        return std::nullopt;
    }
    try {
        Episode e;
        e.highWater = tree->get<cs::Sequence>("high_water");
        return e;
    }
    catch (const std::exception&) {
        if (corrupt) {
            *corrupt = true;
        }
        return std::nullopt;
    }
}

void clearEpisode(const std::string& dir) {
    removeFile(dir, kEpisodeFile);
}

bool saveApprovedBranch(const std::string& dir, const ApprovedBranch& b) {
    ptree tree;
    tree.put("fork_point", b.forkPoint);
    tree.put("block", b.seq);
    tree.put("hash", b.hash);
    if (!b.abandonedHash.empty()) {
        tree.put("abandoned_hash", b.abandonedHash);
    }
    if (!b.abandonedConfidants.empty()) {
        ptree keys;
        for (const auto& k : b.abandonedConfidants) {
            ptree item;
            item.put("", EncodeBase58(k.data(), k.data() + k.size()));
            keys.push_back(std::make_pair("", item));
        }
        tree.add_child("abandoned_confidants", keys);
    }
    if (!b.peerKey.empty()) {
        tree.put("peer_key", b.peerKey);
    }
    if (b.untilUnix != 0) {
        tree.put("prefer_until", utcTime(b.untilUnix));
        tree.put("prefer_until_unix", b.untilUnix);
        tree.put("expired", b.expired);
    }
    return writeTree(dir, kApprovedFile, tree);
}

std::optional<ApprovedBranch> loadApprovedBranch(const std::string& dir, bool* corrupt, int64_t nowUnix) {
    const auto tree = readTree(dir, kApprovedFile, corrupt);
    if (!tree) {
        return std::nullopt;
    }
    try {
        ApprovedBranch b;
        b.forkPoint = tree->get<cs::Sequence>("fork_point");
        b.seq = tree->get<cs::Sequence>("block");
        b.hash = tree->get<std::string>("hash");
        b.abandonedHash = tree->get<std::string>("abandoned_hash", "");
        b.peerKey = tree->get<std::string>("peer_key", "");
        b.untilUnix = tree->get<int64_t>("prefer_until_unix", 0);
        b.expired = tree->get<bool>("expired", false);
        if (const auto keys = tree->get_child_optional("abandoned_confidants")) {
            for (const auto& item : *keys) {
                cs::Bytes key;
                cs::PublicKey k{};
                if (!DecodeBase58(item.second.get_value<std::string>(), key) || key.size() != k.size()) {
                    throw std::runtime_error("bad key");
                }
                std::copy(key.begin(), key.end(), k.begin());
                b.abandonedConfidants.push_back(k);
            }
        }
        // a deadline this node would not have written: no further than the preference lasts
        const int64_t now = nowUnix != 0 ? nowUnix : static_cast<int64_t>(std::time(nullptr));
        if (b.untilUnix > now + kPreferenceSeconds) {
            b.untilUnix = now + kPreferenceSeconds;
        }
        if (b.hash.empty() || b.seq != b.forkPoint + 1 || b.abandonedHash == b.hash || b.untilUnix < 0 ||
            (b.untilUnix == 0 && (!b.abandonedHash.empty() || b.expired || !b.abandonedConfidants.empty()))) {
            if (corrupt) {
                *corrupt = true;
            }
            return std::nullopt;
        }
        return b;
    }
    catch (const std::exception&) {
        if (corrupt) {
            *corrupt = true;
        }
        return std::nullopt;
    }
}

BranchVerdict judgeBlock(const ApprovedBranch& b, cs::Sequence seq, const std::string& hash) {
    if (seq != b.seq || hash.empty()) {
        return BranchVerdict::None;
    }
    if (hash == b.hash) {
        return BranchVerdict::Chosen;
    }
    if (b.preferring() || (!b.abandonedHash.empty() && hash == b.abandonedHash)) {
        return BranchVerdict::Refuse;
    }
    return BranchVerdict::None;
}

BranchVerdict judgeChild(const ApprovedBranch& b, cs::Sequence seq, const std::string& previous) {
    if (seq != b.seq + 1 || previous.empty()) {
        return BranchVerdict::None;
    }
    if (previous == b.hash) {
        return BranchVerdict::Chosen;
    }
    if (b.preferring() || (!b.abandonedHash.empty() && previous == b.abandonedHash)) {
        return BranchVerdict::Refuse;
    }
    return BranchVerdict::None;
}

void clearApprovedBranch(const std::string& dir) {
    removeFile(dir, kApprovedFile);
}

bool ForkRecord::inDismissedBranch(const std::string& hash, const std::string& previous) const {
    for (const auto& h : branchHashes) {
        if ((!hash.empty() && h == hash) || (!previous.empty() && h == previous)) {
            return true;
        }
    }
    return false;
}

void ForkRecord::addBranchHash(const std::string& hash) {
    if (hash.empty()) {
        return;
    }
    for (const auto& h : branchHashes) {
        if (h == hash) {
            return;
        }
    }
    branchHashes.push_back(hash);
    // the oldest go first, but never the dismissed block itself: forged blocks cannot push it out
    while (branchHashes.size() > kMaxBranchHashes) {
        auto victim = branchHashes.begin();
        if (*victim == dismissedHash && branchHashes.size() > 1) {
            ++victim;
        }
        branchHashes.erase(victim);
    }
}

bool saveForkRecord(const std::string& dir, const ForkRecord& r) {
    ptree tree;
    tree.put("held", r.held);
    tree.put("reason", r.reason);
    tree.put("since", utcTime(r.sinceUnix));
    tree.put("since_unix", r.sinceUnix);
    tree.put("high_water", r.highWater);
    tree.put("our_head", r.ourHead);
    tree.put("our_head_hash", r.ourHeadHash);
    tree.put("fork_point", r.forkPoint);
    tree.put("fork_point_hash", r.forkPointHash);
    tree.put("fork_point_exact", r.forkPointExact);
    tree.put("fork_point_status", r.forkPointStatus());
    if (r.located) {
        tree.put("fork_point_located", true);
        tree.put("fork_point_upper_bound", r.upperBound);
        tree.put("fork_point_located_with", r.locatedWith);
        tree.put("located_verified", r.locatedVerified);
        if (!r.locatedNextHash.empty()) {
            tree.put("located_next_hash", r.locatedNextHash);
        }
        if (!r.locatedHash.empty()) {
            tree.put("located_point", r.locatedPoint);
            tree.put("located_hash", r.locatedHash);
        }
        if (r.deeper) {
            tree.put("fork_point_below", r.deeperThan);
        }
    }
    if (!r.ourNextHash.empty()) {
        tree.put("our_next_hash", r.ourNextHash);
    }
    tree.put("competing_block", r.competingSeq);
    tree.put("competing_hash", r.competingHash);
    tree.put("peer_key", r.peerKey);
    tree.put("peer_address", r.peerAddress);
    tree.put("peer_height", r.peerHeight);
    if (r.hasDismissal()) {
        tree.put("dismissed", utcTime(r.dismissedUnix));
        tree.put("dismissed_unix", r.dismissedUnix);
        tree.put("dismissed_block", r.dismissedSeq);
        tree.put("dismissed_hash", r.dismissedHash);
        ptree hashes;
        for (const auto& h : r.branchHashes) {
            ptree item;
            item.put("", h);
            hashes.push_back(std::make_pair("", item));
        }
        tree.add_child("dismissed_branch_hashes", hashes);
    }
    return writeTree(dir, kForkFile, tree);
}

std::optional<ForkRecord> loadForkRecord(const std::string& dir, bool* corrupt) {
    const auto tree = readTree(dir, kForkFile, corrupt);
    if (!tree) {
        return std::nullopt;
    }
    try {
        ForkRecord r;
        r.held = tree->get<bool>("held", true);
        r.reason = tree->get<std::string>("reason", "");
        r.sinceUnix = tree->get<int64_t>("since_unix", 0);
        r.highWater = tree->get<cs::Sequence>("high_water");
        r.ourHead = tree->get<cs::Sequence>("our_head");
        r.ourHeadHash = tree->get<std::string>("our_head_hash", "");
        r.forkPoint = tree->get<cs::Sequence>("fork_point");
        r.forkPointHash = tree->get<std::string>("fork_point_hash", "");
        r.forkPointExact = tree->get<bool>("fork_point_exact", false);
        r.located = tree->get<bool>("fork_point_located", false);
        r.upperBound = tree->get<cs::Sequence>("fork_point_upper_bound", 0);
        r.locatedWith = tree->get<std::string>("fork_point_located_with", "");
        r.locatedVerified = r.located && tree->get<bool>("located_verified", false);
        r.locatedPoint = tree->get<cs::Sequence>("located_point", 0);
        r.locatedHash = tree->get<std::string>("located_hash", "");
        r.locatedNextHash = tree->get<std::string>("located_next_hash", "");
        r.ourNextHash = tree->get<std::string>("our_next_hash", "");
        if (const auto below = tree->get_optional<cs::Sequence>("fork_point_below")) {
            r.deeper = r.located && !r.locatedVerified;
            r.deeperThan = *below;
        }
        r.competingSeq = tree->get<cs::Sequence>("competing_block", 0);
        r.competingHash = tree->get<std::string>("competing_hash", "");
        r.peerKey = tree->get<std::string>("peer_key", "");
        r.peerAddress = tree->get<std::string>("peer_address", "");
        r.peerHeight = tree->get<cs::Sequence>("peer_height", 0);
        r.dismissedUnix = tree->get<int64_t>("dismissed_unix", 0);
        r.dismissedSeq = tree->get<cs::Sequence>("dismissed_block", 0);
        r.dismissedHash = tree->get<std::string>("dismissed_hash", "");
        if (const auto hashes = tree->get_child_optional("dismissed_branch_hashes")) {
            for (const auto& item : *hashes) {
                r.addBranchHash(item.second.get_value<std::string>());
            }
        }
        return r;
    }
    catch (const std::exception&) {
        if (corrupt) {
            *corrupt = true;
        }
        return std::nullopt;
    }
}

void clearForkRecord(const std::string& dir) {
    removeFile(dir, kForkFile);
}

void clearAll(const std::string& dir) {
    removeFile(dir, kEpisodeFile);
    removeFile(dir, kForkFile);
    removeFile(dir, kApprovedFile);
}

std::optional<ResolvePlan> planResolve(const std::string& dir, std::string* error, bool toLocated) {
    bool corrupt = false;
    const auto record = loadForkRecord(dir, &corrupt);
    if (corrupt) {
        if (error) {
            *error = std::string("the fork record (") + kForkFile + ") in " + dir +
                     " cannot be read: nothing removed. Use --set-bc-top <sequence> after checking with other nodes";
        }
        return std::nullopt;
    }
    if (!record.has_value()) {
        if (error) {
            *error = std::string("no fork record (") + kForkFile + ") in " + dir +
                     ": this node holds no refused fork, nothing to do. To cut the chain at a known block, use --set-bc-top <sequence>";
        }
        return std::nullopt;
    }
    if (!record->held) {
        if (error) {
            *error = record->hasDismissal()
                ? "the fork was dismissed on " + utcTime(record->dismissedUnix) +
                  ": nothing held, nothing removed. To cut the chain at a known block, use --set-bc-top <sequence>"
                : std::string("nothing held, nothing removed. To cut the chain at a known block, use --set-bc-top <sequence>");
        }
        return std::nullopt;
    }
    if (toLocated) {
        if (!record->located || record->deeper || record->locatedHash.empty()) {
            if (error) {
                *error = "--to-located: the node has located no last common block (fork point status: " +
                         record->forkPointStatus() + "): nothing removed. Run --resolve-fork without it to cut at "
                         "the recorded fork point, or --set-bc-top <sequence>";
            }
            return std::nullopt;
        }
        return ResolvePlan{record->locatedPoint, record->locatedHash, *record, true};
    }
    if (record->forkPointHash.empty()) {
        if (error) {
            *error = "the fork record has no hash for block #" + std::to_string(record->forkPoint) +
                     " and cannot be checked against the database: nothing removed. Use --set-bc-top <sequence>";
        }
        return std::nullopt;
    }
    return ResolvePlan{record->forkPoint, record->forkPointHash, *record, false};
}

std::optional<PreferredBranch> preferenceFor(const ResolvePlan& plan) {
    const auto& r = plan.record;
    if (plan.toLocated) {
        // the operator's choice of the located point: preferred only when it is the verified one
        if (r.locatedVerified && r.forkPointExact && r.forkPoint == plan.top && !r.locatedNextHash.empty()) {
            return PreferredBranch{r.locatedNextHash, r.locatedWith};
        }
        return std::nullopt;
    }
    if (!r.forkPointExact) {
        return std::nullopt;  // an upper bound
    }
    if (!r.located && r.competingSeq == plan.top + 1 && !r.competingHash.empty()) {
        return PreferredBranch{r.competingHash, r.peerKey};  // the refused block names the fork point
    }
    if (r.located && r.locatedVerified && !r.locatedNextHash.empty()) {
        return PreferredBranch{r.locatedNextHash, r.locatedWith};
    }
    return std::nullopt;
}

std::string resolveOverview(const ResolvePlan& plan) {
    const auto& r = plan.record;
    std::ostringstream os;
    os << "our head #" << r.ourHead << "; fork point #" << r.forkPoint << " (" << r.forkPointStatus() << ")";
    if (r.located) {
        os << ", upper bound #" << r.upperBound;
        if (r.deeper) {
            os << ", a peer's block differs already at #" << r.deeperThan << " (not verified)";
        }
        else if (!r.locatedHash.empty()) {
            os << ", located #" << r.locatedPoint << (r.locatedVerified ? " (verified)" : " (NOT verified)");
        }
    }
    else {
        os << ", not located";
    }
    const cs::Sequence removed = r.ourHead > plan.top ? r.ourHead - plan.top : 0;
    os << ". Cutting at #" << plan.top << (plan.toLocated ? " (--to-located)" : "") << ": " << removed
       << " block(s) removed";
    return os.str();
}

std::string utcTime(int64_t unixSeconds) {
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

std::string remedy(const ForkRecord& r) {
    std::ostringstream os;
    os << "first check the head and the block at #" << r.forkPoint << " with other nodes or the monitor. If this node's "
       << "chain is the right one: create the file " << kDismissTrigger << " next to the db folder (native: touch "
       << kDismissTrigger << " in the node folder; Docker: docker compose exec node touch /data/" << kDismissTrigger
       << "); the running node picks it up within a few seconds, keeps refusing that branch and stops reporting it. If the other chain is the right one: stop "
       << "the node and run once node --config-file config.ini --resolve-fork (Docker: docker compose stop && docker "
       << "compose run --rm node resolve-fork && docker compose up -d), then start it as usual; by hand: --set-bc-top "
       << r.forkPoint << " (Docker: docker compose run --rm node set-bc-top " << r.forkPoint << ").";
    return os.str();
}

std::string summary(const ForkRecord& r, const std::string& peerSide) {
    std::ostringstream os;
    os << "FORK HELD since " << utcTime(r.sinceUnix) << ": ";
    if (r.reason == "authority") {
        os << "a competing block is not vouched for by this node's confidants";
    }
    else if (r.reason == "unreadable") {
        os << "the fork record " << kForkFile << " could not be read; the fork point is not known (check the chain "
           << "with other nodes)";
    }
    else {
        os << "a competing chain asks to unwind beyond the automatic bound (reference head #" << r.highWater << ")";
    }
    os << ". Fork point: block #" << r.forkPoint << (r.forkPointExact ? "" : " or lower");
    if (!r.forkPointHash.empty()) {
        os << " (" << r.forkPointHash << ")";
    }
    if (r.located && r.locatedVerified) {
        os << ", the last common block, verified (located from #" << r.upperBound << ")";
    }
    else if (r.located && r.deeper) {
        os << "; a peer's block differs already at #" << r.deeperThan << " (not verified): the fork may be deeper, "
           << "find the last common block with other nodes";
    }
    else if (r.located && !r.locatedHash.empty()) {
        os << "; a peer's blocks place the last common block at #" << r.locatedPoint
           << ", NOT verified: --resolve-fork cuts at #" << r.forkPoint << ", --resolve-fork --to-located at #"
           << r.locatedPoint;
    }
    os << "; our head #" << r.ourHead;
    if (r.competingSeq != 0) {
        os << ", refused block #" << r.competingSeq;
    }
    if (!r.peerKey.empty() || !r.peerAddress.empty()) {
        os << " from " << (r.peerKey.empty() ? std::string("?") : r.peerKey);
        if (!r.peerAddress.empty()) {
            os << " (" << r.peerAddress << ")";
        }
    }
    if (r.peerHeight != 0) {
        os << ", peer height #" << r.peerHeight;
    }
    if (!peerSide.empty()) {
        os << "; the other side is " << peerSide;
    }
    os << ". The node keeps its chain. What to do: " << remedy(r);
    return os.str();
}

}  // namespace cs::forkguard
