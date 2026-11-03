#include "banner.hpp"

#include <fstream>

namespace cs {

std::vector<std::string> startupBanner(const BannerInfo& info, bool art) {
    std::vector<std::string> lines;
    if (art) {
        lines = {
            "   ____ ____  _____ ____ ___ _____ ____",
            "  / ___|  _ \\| ____|  _ \\_ _|_   _/ ___|",
            " | |   | |_) |  _| | | | | |  | | \\___ \\",
            " | |___|  _ <| |___| |_| | |  | |  ___) |",
            "  \\____|_| \\_\\_____|____/___| |_| |____/",
            "              c o m m u n i t y",
            "",
        };
    }
    std::string build = info.build.substr(0, 8);
    lines.push_back("  node " + info.version + "  (protocol " + std::to_string(info.protocol) +
                    (build.empty() ? "" : ", build " + build) + ")");
    std::string storage = "  storage: " + std::string(info.pruned ? "pruned" : "full") + ", " + info.backend +
                          "    snapshots: serve " + (info.serve ? "on" : "off");
    if (!info.pruned) {
        storage += "    backfill: " + std::string(info.backfill ? "on" : "off");
    }
    lines.push_back(storage);
    lines.push_back("  https://github.com/CreditsCommunity/node");
    return lines;
}

std::string packageVersion(int protocol, int minor) {
    std::string version;
    std::ifstream file("VERSION");
    if (!file || !std::getline(file, version) || version.empty() || version.size() > 64) {
        return "4.2." + std::to_string(protocol) + "." + std::to_string(minor);
    }
    while (!version.empty() && (version.back() == '\r' || version.back() == ' ')) {
        version.pop_back();
    }
    for (auto& c : version) {
        if (c < 0x20 || c >= 0x7f) {
            c = '_';
        }
    }
    return version;
}

}  // namespace cs
