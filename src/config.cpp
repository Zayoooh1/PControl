#include "config.hpp"
#include "process.hpp"
#include <fstream>
#include <set>
namespace pc {
Json defaults() {
    return {{"schemaVersion", 1},
            {"core", {{"updateIntervalMs", 1500}}},
            {"priorityRules", Json::array()},
            {"affinityRules", Json::array()}};
}
void validateConfig(const Json &j) {
    if (j.at("schemaVersion") != 1)
        throw Error(ERROR_REVISION_MISMATCH, "Unsupported config version");
    auto interval = unsigned32(j.at("core").at("updateIntervalMs"));
    if (interval < 250 || interval > 60000)
        throw Error(ERROR_INVALID_PARAMETER, "Interval must be 250..60000 ms");
    for (const auto &kind : {"priorityRules", "affinityRules"}) {
        const auto &a = j.at(kind);
        if (!a.is_array() || a.size() > 1024)
            throw Error(ERROR_INVALID_DATA, "Invalid rules array");
        std::set<std::wstring> seen;
        for (auto &r : a) {
            if (r.at("matchType") != "executableName")
                throw Error(ERROR_NOT_SUPPORTED, "Unsupported match type");
            auto name = wide(r.at("name").get<std::string>());
            if (name.empty() || name.size() > 260 ||
                std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }) ||
                name.find_first_of(L"\\/:*?\"<>|\r\n\t") != std::wstring::npos || name.front() == L' ' ||
                name.back() == L' ')
                throw Error(ERROR_INVALID_PARAMETER, "Use an exact executable name without a path");
            CharLowerBuffW(name.data(), (DWORD)name.size());
            if (!seen.insert(name).second)
                throw Error(ERROR_INVALID_DATA, "Duplicate rule");
            if (std::string(kind) == "priorityRules") {
                if (!r.at("value").is_number_integer() || !validPriority(unsigned32(r.at("value"))))
                    throw Error(ERROR_INVALID_PARAMETER, "Invalid priority");
            } else
                cpuMask(r.at("value"));
        }
    }
}
Json Config::load() {
    if (!std::filesystem::exists(path_))
        return defaults();
    if (std::filesystem::file_size(path_) > 1024 * 1024)
        throw Error(ERROR_FILE_TOO_LARGE, "Config exceeds 1 MiB");
    std::ifstream f(path_);
    if (!f)
        throw Error(ERROR_READ_FAULT, "Cannot read config");
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto j = parseJson(bytes);
    validateConfig(j);
    return j;
}
void Config::save(const Json &j) {
    validateConfig(j);
    std::filesystem::create_directories(path_.parent_path());
    auto temp = path_;
    temp += L".tmp";
    auto bytes = j.dump(2);
    if (bytes.size() > 1024 * 1024)
        throw Error(ERROR_FILE_TOO_LARGE, "Config exceeds 1 MiB");
    {
        Handle h(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr));
        check(bool(h), "Create config temporary file");
        DWORD n = 0;
        check(WriteFile(h, bytes.data(), (DWORD)bytes.size(), &n, nullptr), "Write config");
        if (n != bytes.size())
            throw Error(ERROR_WRITE_FAULT, "Incomplete config write");
        check(FlushFileBuffers(h), "Flush config");
    }
    check(MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Replace config");
}
} // namespace pc
