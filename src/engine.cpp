#include "engine.hpp"
#include "process.hpp"
#include <map>
namespace pc {
Engine::Engine(const std::filesystem::path &d) : log_(d), store_(d), config_(defaults()) {
    log_.log("INFO", "Lifecycle", "Core startup");
    try {
        config_ = store_.load();
        log_.log("INFO", "Config", "Configuration loaded");
    } catch (const std::exception &e) {
        configHealthy_ = false;
        log_.log("ERROR", "Config", e.what());
    }
    monitor_ = std::thread([this] { monitor(); });
}
Engine::~Engine() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (monitor_.joinable())
        monitor_.join();
    log_.log("INFO", "Lifecycle", "Core shutdown");
}
void Engine::monitor() {
    std::map<DWORD, std::string> old;
    std::unique_lock lock(mutex_);
    while (!stop_) {
        dirty_ = false;
        lock.unlock();
        Json rows;
        bool ok = false;
        try {
            rows = enumerate();
            ok = true;
        } catch (const std::exception &e) {
            log_.log("ERROR", "Monitoring", e.what());
        }
        lock.lock();
        if (ok) {
            std::map<DWORD, std::string> current;
            for (auto &p : rows) {
                DWORD pid = p.at("pid");
                std::string id = p.at("creationTime");
                current[pid] = id;
                if (!old.contains(pid) || old[pid] != id) {
                    log_.log("INFO", "Process",
                             "New PID=" + std::to_string(pid) + " creation=" + id + " " +
                                 p.at("name").get<std::string>());
                    if (p.at("accessError") != 0)
                        log_.log("WARN", "Access",
                                 "PID=" + std::to_string(pid) + " Win32=" + p.at("accessError").dump());
                }
            }
            for (auto &[pid, id] : old)
                if (!current.contains(pid) || current[pid] != id)
                    log_.log("INFO", "Process",
                             "Terminated/replaced PID=" + std::to_string(pid) + " creation=" + id);
            old = std::move(current);
            snapshot_ = std::move(rows);
            rules_.apply(snapshot_, config_, log_);
        }
        wake_.wait_for(lock, std::chrono::milliseconds(config_.at("core").at("updateIntervalMs").get<int>()),
                       [this] { return stop_ || dirty_; });
    }
}
Json Engine::dispatch(const Json &q) {
    try {
        if (q.at("version") != 1)
            throw Error(ERROR_REVISION_MISMATCH, "Protocol version must be 1");
        std::string cmd = q.at("command");
        if (cmd == "SetPriority" || cmd == "SetAffinity" || cmd == "GetProcessDetails") {
            DWORD pid = unsigned32(q.at("pid"));
            auto id = decimal(q.at("creationTime"));
            if (cmd == "GetProcessDetails")
                return success(details(pid, id));
            auto result = change(pid, id, cmd == "SetPriority" ? "priority" : "affinity", q.at("value"));
            return success(result);
        }
        std::lock_guard lock(mutex_);
        if (cmd == "Ping" || cmd == "GetStatus")
            return success({{"state", "Running"},
                            {"pid", GetCurrentProcessId()},
                            {"configHealthy", configHealthy_},
                            {"intervalMs", config_.at("core").at("updateIntervalMs")},
                            {"processorGroups", GetActiveProcessorGroupCount()}});
        if (cmd == "GetProcessList")
            return success(snapshot_);
        if (cmd == "GetConfiguration")
            return success(config_);
        if (cmd == "ReloadConfiguration") {
            Json c;
            try {
                c = store_.load();
            } catch (...) {
                configHealthy_ = false;
                throw;
            }
            config_ = std::move(c);
            configHealthy_ = true;
            rules_.reset();
            dirty_ = true;
            wake_.notify_all();
            log_.log("INFO", "Config", "Configuration reloaded");
            return success();
        }
        bool priority = cmd == "AddPersistentPriorityRule" || cmd == "RemovePersistentPriorityRule";
        bool affinity = cmd == "AddPersistentAffinityRule" || cmd == "RemovePersistentAffinityRule";
        if (priority || affinity || cmd == "SetUpdateInterval") {
            if (!configHealthy_)
                throw Error(ERROR_INVALID_DATA,
                            "Config is corrupt; repair file and reload before saving rules");
            Json next = config_;
            if (cmd == "SetUpdateInterval")
                next["core"]["updateIntervalMs"] = q.at("value");
            else {
                auto &list = next[priority ? "priorityRules" : "affinityRules"];
                auto name = q.at("name").get<std::string>();
                auto key = wide(name);
                list.erase(std::remove_if(list.begin(), list.end(),
                                          [&](const Json &r) {
                                              auto other = wide(r.at("name"));
                                              return CompareStringOrdinal(key.c_str(), -1, other.c_str(), -1,
                                                                          TRUE) == CSTR_EQUAL;
                                          }),
                           list.end());
                if (cmd.starts_with("Add"))
                    list.push_back(
                        {{"matchType", "executableName"}, {"name", name}, {"value", q.at("value")}});
            }
            store_.save(next);
            config_ = std::move(next);
            rules_.reset();
            rules_.apply(snapshot_, config_, log_);
            dirty_ = true;
            wake_.notify_all();
            return success(config_);
        }
        throw Error(ERROR_NOT_SUPPORTED, "Unknown command: " + cmd);
    } catch (const Error &e) {
        log_.log("WARN", "Request", std::to_string(e.code) + " " + e.what());
        return failure(e.code, e.what());
    } catch (const std::exception &e) {
        log_.log("WARN", "MalformedRequest", e.what());
        return failure(ERROR_INVALID_DATA, e.what());
    }
}
} // namespace pc
