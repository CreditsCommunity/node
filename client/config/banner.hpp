#ifndef CLIENT_CONFIG_BANNER_HPP
#define CLIENT_CONFIG_BANNER_HPP

#include <string>
#include <vector>

namespace cs {

// what the start-up banner shows, so that a log says at once which node wrote it
struct BannerInfo {
    std::string version;     // 4.2.532-community.N, from the VERSION file of the package
    int protocol = 0;        // NODE_VERSION, the version the network checks
    std::string build;       // the commit the binary was built from
    bool pruned = false;
    std::string backend;     // berkeleydb, lmdb or rocksdb
    bool serve = false;      // [snapshot] serve
    bool backfill = false;   // [storage] backfill, for a full node
};

// the lines to log at start-up: the logo and "community" when art is set ([params] banner),
// then the version and the settings; plain lines only without art
std::vector<std::string> startupBanner(const BannerInfo& info, bool art);

// the version of the package: the first line of ./VERSION, else 4.2.<protocol>.<minor>
std::string packageVersion(int protocol, int minor);

}  // namespace cs

#endif  // CLIENT_CONFIG_BANNER_HPP
