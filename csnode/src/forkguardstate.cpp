#include <csnode/forkguardstate.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace cs::forkguard {

namespace fs = std::filesystem;
using boost::property_tree::ptree;

namespace {

// a small file written whole: to a temporary name first, then renamed over the old one
bool writeTree(const std::string& dir, const char* name, const ptree& tree) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path target = fs::path(dir) / name;
    const fs::path tmp = fs::path(dir) / (std::string(name) + ".tmp");
    try {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return false;
        }
        boost::property_tree::write_json(out, tree);
        out.close();
        if (!out) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    catch (const std::exception&) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<ptree> readTree(const std::string& dir, const char* name) {
    if (dir.empty()) {
        return std::nullopt;
    }
    const fs::path path = fs::path(dir) / name;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > 64 * 1024) {
        return std::nullopt;
    }
    try {
        std::ifstream in(path);
        ptree tree;
        boost::property_tree::read_json(in, tree);
        return tree;
    }
    catch (const std::exception&) {
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

std::optional<Episode> loadEpisode(const std::string& dir) {
    const auto tree = readTree(dir, kEpisodeFile);
    if (!tree) {
        return std::nullopt;
    }
    try {
        Episode e;
        e.highWater = tree->get<cs::Sequence>("high_water");
        return e;
    }
    catch (const std::exception&) {
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
    return writeTree(dir, kApprovedFile, tree);
}

std::optional<ApprovedBranch> loadApprovedBranch(const std::string& dir) {
    const auto tree = readTree(dir, kApprovedFile);
    if (!tree) {
        return std::nullopt;
    }
    try {
        ApprovedBranch b;
        b.forkPoint = tree->get<cs::Sequence>("fork_point");
        b.seq = tree->get<cs::Sequence>("block");
        b.hash = tree->get<std::string>("hash");
        if (b.hash.empty() || b.seq != b.forkPoint + 1) {
            return std::nullopt;
        }
        return b;
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
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
    if (branchHashes.size() > kMaxBranchHashes) {
        branchHashes.erase(branchHashes.begin(), branchHashes.begin() + (branchHashes.size() - kMaxBranchHashes));
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

std::optional<ForkRecord> loadForkRecord(const std::string& dir) {
    const auto tree = readTree(dir, kForkFile);
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
        return std::nullopt;
    }
}

void clearForkRecord(const std::string& dir) {
    removeFile(dir, kForkFile);
}

std::optional<ResolvePlan> planResolve(const std::string& dir, std::string* error) {
    const auto record = loadForkRecord(dir);
    if (!record.has_value()) {
        if (error) {
            *error = std::string("no fork record (") + kForkFile + ") in " + dir +
                     ": this node holds no refused fork, nothing to do. To cut the chain at a known block, use --set-bc-top <sequence>";
        }
        return std::nullopt;
    }
    if (record->forkPointHash.empty()) {
        if (error) {
            *error = "the fork record has no hash for block #" + std::to_string(record->forkPoint) +
                     " and cannot be checked against the database: nothing removed. Use --set-bc-top <sequence>";
        }
        return std::nullopt;
    }
    return ResolvePlan{record->forkPoint, record->forkPointHash, *record};
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
       << "); the node keeps refusing that branch and stops reporting it. If the other chain is the right one: stop "
       << "the node and run once node --config-file config.ini --resolve-fork (Docker: docker compose stop && docker "
       << "compose run --rm node resolve-fork && docker compose up -d), then start it as usual; by hand: --set-bc-top "
       << r.forkPoint << " (Docker: docker compose run --rm node set-bc-top " << r.forkPoint << ").";
    return os.str();
}

std::string summary(const ForkRecord& r) {
    std::ostringstream os;
    os << "FORK HELD since " << utcTime(r.sinceUnix) << ": ";
    if (r.reason == "authority") {
        os << "a competing block is not vouched for by this node's confidants";
    }
    else {
        os << "a competing chain asks to unwind beyond the automatic bound (reference head #" << r.highWater << ")";
    }
    os << ". Fork point: block #" << r.forkPoint << (r.forkPointExact ? "" : " or lower");
    if (!r.forkPointHash.empty()) {
        os << " (" << r.forkPointHash << ")";
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
    os << ". The node keeps its chain. What to do: " << remedy(r);
    return os.str();
}

}  // namespace cs::forkguard
