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
        require(all.front().contains("isWindowsSystemProcess"), "System classification included in model");
        require(!pc::isWindowsSystemProcess(L"C:\\Program Files\\Vendor\\App.exe", L"App.exe", 100,
                                            L"C:\\Windows"),
                "Program Files application is ordinary");
        require(!pc::isWindowsSystemProcess(L"C:\\Users\\Test\\AppData\\Local\\App.exe", L"App.exe", 101,
                                            L"C:\\Windows"),
                "LocalAppData application is ordinary");
        require(pc::isWindowsSystemProcess(L"C:\\WINDOWS\\System32\\svchost.exe", L"svchost.exe", 102,
                                           L"c:/windows/"),
                "Windows directory comparison is case-insensitive");
        require(
            pc::isWindowsSystemProcess(L"C:\\Windows\\explorer.exe", L"explorer.exe", 106, L"C:\\Windows"),
            "Explorer follows the Windows path rule");
        require(!pc::isWindowsSystemProcess(L"C:\\WindowsFake\\App.exe", L"App.exe", 103, L"C:\\Windows"),
                "Similar path prefix is ordinary");
        require(pc::isWindowsSystemProcess(L"", L"System", 4, L"C:\\Windows"), "PID 4 System fallback");
        require(pc::isWindowsSystemProcess(L"", L"Registry", 104, L"C:\\Windows"), "Registry fallback");
        require(pc::isWindowsSystemProcess(L"", L"SVCHOST.EXE", 105, L"C:\\Windows"),
                "Known Windows executable fallback is case-insensitive");
        auto sorted =
            pc::Json::array({{{"pid", 9}, {"name", "zeta.exe"}, {"isWindowsSystemProcess", false}},
                             {{"pid", 7}, {"name", "SVCHOST.exe"}, {"isWindowsSystemProcess", true}},
                             {{"pid", 4}, {"name", "alpha.exe"}, {"isWindowsSystemProcess", false}},
                             {{"pid", 3}, {"name", "Alpha.exe"}, {"isWindowsSystemProcess", false}},
                             {{"pid", 2}, {"name", "csrss.exe"}, {"isWindowsSystemProcess", true}}});
        pc::sortProcessRows(sorted);
        require(sorted[0]["pid"] == 3 && sorted[1]["pid"] == 4 && sorted[2]["pid"] == 9 &&
                    sorted[3]["pid"] == 2 && sorted[4]["pid"] == 7,
                "Rows sort by group, case-insensitive name, then PID");
        bool reachedSystemGroup = false;
        for (const auto &process : all) {
            bool system = process.at("isWindowsSystemProcess");
            require(!reachedSystemGroup || system, "Enumerated user processes stay above Windows processes");
            reachedSystemGroup = reachedSystemGroup || system;
        }
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
