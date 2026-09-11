#include "rules.hpp"
#include "process.hpp"
namespace pc {
void Rules::apply(const Json &rows, const Json &cfg, Logger &log) {
    std::set<std::string> live;
    for (const auto &p : rows) {
        if (p.at("creationTime") == "0")
            continue;
        for (const auto &kind : {"priority", "affinity"}) {
            std::string section = std::string(kind) + "Rules";
            for (const auto &r : cfg.at(section)) {
                auto a = wide(p.at("name")), b = wide(r.at("name"));
                if (CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) != CSTR_EQUAL)
                    continue;
                auto key = p.at("pid").dump() + ":" + p.at("creationTime").get<std::string>() + ":" + kind;
                live.insert(key);
                if (applied_.contains(key))
                    continue;
                applied_.insert(key);
                try {
                    change(p.at("pid"), decimal(p.at("creationTime")), kind, r.at("value"));
                    log.log("INFO", "Rule", key + " applied");
                } catch (const Error &e) {
                    log.log("WARN", "Rule", key + " failed Win32=" + std::to_string(e.code) + " " + e.what());
                } catch (const std::exception &e) {
                    log.log("WARN", "Rule", key + " failed " + e.what());
                }
            }
        }
    }
    applied_ = std::move(live);
}
} // namespace pc
