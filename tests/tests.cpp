#include "engine.hpp"
#include "process.hpp"
#include <fstream>
#include <iostream>
int main() {
    int checks = 0;
    auto require = [&](bool b, const char *m) {
        if (!b)
            throw std::runtime_error(m);
        ++checks;
    };
    auto dir = std::filesystem::temp_directory_path() /
               (L"PControl.Tests." + std::to_wstring(GetCurrentProcessId()));
    try {
        require(pc::decimal("18446744073709551615") == UINT64_MAX, "uint64 decimal");
        for (auto s : {"", "-1", "1x", "18446744073709551616"}) {
            bool rejected = false;
            try {
                pc::decimal(s);
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Invalid decimal rejected");
        }
        pc::Config store(dir);
        for (const auto &value : pc::Json::array({-1, 4294967296ULL, 1.5, "32"})) {
            bool rejected = false;
            try {
                pc::unsigned32(value);
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Integer range/type rejected");
        }
        bool nestedRejected = false;
        try {
            pc::parseJson(std::string(40, '[') + "0" + std::string(40, ']'));
        } catch (...) {
            nestedRejected = true;
        }
        require(nestedRejected, "Deep JSON rejected");
        auto cfg = pc::defaults();
        store.save(cfg);
        require(store.load() == cfg, "Config roundtrip");
        auto invalid = cfg;
        invalid["core"]["updateIntervalMs"] = 0;
        bool rejected = false;
        try {
            store.save(invalid);
        } catch (...) {
            rejected = true;
        }
        require(rejected && store.load() == cfg, "Invalid config preserves file");
        auto oversized = cfg;
        oversized["extra"] = std::string(1024 * 1024, 'x');
        rejected = false;
        try {
            store.save(oversized);
        } catch (...) {
            rejected = true;
        }
        require(rejected && store.load() == cfg, "Oversized config preserves file");
        {
            pc::Logger logger(dir);
            {
                std::ofstream f(dir / L"Core.log");
                f << std::string(2 * 1024 * 1024 + 1, 'x');
            }
            logger.log("INFO", "Test", "rotation");
            require(std::filesystem::exists(dir / L"Core.log.1") &&
                        std::filesystem::file_size(dir / L"Core.log") < 1024,
                    "Log rotation");
        }
        auto all = pc::enumerate();
        require(!all.empty(), "Process enumeration");
        DWORD pid = GetCurrentProcessId();
        uint64_t id = pc::creation(GetCurrentProcess());
        auto d = pc::details(pid, id);
        require(d.at("creationTime") == std::to_string(id), "Creation identity");
        rejected = false;
        try {
            pc::change(pid, id + 1, "priority", NORMAL_PRIORITY_CLASS);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Stale PID blocked");
        DWORD original = GetPriorityClass(GetCurrentProcess());
        pc::change(pid, id, "priority", BELOW_NORMAL_PRIORITY_CLASS);
        require(GetPriorityClass(GetCurrentProcess()) == BELOW_NORMAL_PRIORITY_CLASS, "Priority changed");
        pc::change(pid, id, "priority", original);
        if (GetActiveProcessorGroupCount() == 1) {
            DWORD_PTR old = 0, system = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &old, &system);
            unsigned first = 0;
            while (!(system & (DWORD_PTR(1) << first)))
                ++first;
            pc::change(pid, id, "affinity", {{"group", 0}, {"cpus", {first}}});
            DWORD_PTR current = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &current, &system);
            require(current == (DWORD_PTR(1) << first), "Affinity changed");
            SetProcessAffinityMask(GetCurrentProcess(), old);
            rejected = false;
            try {
                pc::cpuMask({{"group", 0}, {"cpus", pc::Json::array()}});
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Empty affinity rejected");
        }
        {
            pc::Engine engine(dir);
            require(!engine.dispatch({{"version", 9}, {"command", "Ping"}}).at("success"),
                    "Version mismatch rejected");
            require(!engine.dispatch({{"version", 1}, {"command", "Invalid"}}).at("success"),
                    "Unknown command rejected");
            require(!engine.dispatch({{"version", 1}, {"command", "SetPriority"}}).at("success"),
                    "Malformed command rejected");
            require(engine.dispatch({{"version", 1}, {"command", "Ping"}}).at("success"), "Engine ping");
            {
                std::ofstream f(dir / L"config.json");
                f << "broken";
            }
            require(!engine.dispatch({{"version", 1}, {"command", "ReloadConfiguration"}}).at("success"),
                    "Corrupt reload rejected");
            require(!engine.dispatch({{"version", 1}, {"command", "SetUpdateInterval"}, {"value", 500}})
                         .at("success"),
                    "Corrupt reload blocks writes");
        }
        {
            std::ofstream f(dir / L"config.json");
            f << "broken";
        }
        {
            pc::Engine engine(dir);
            require(!engine.dispatch({{"version", 1}, {"command", "SetUpdateInterval"}, {"value", 1500}})
                         .at("success"),
                    "Corrupt config write blocked");
        }
        std::filesystem::remove_all(dir);
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
