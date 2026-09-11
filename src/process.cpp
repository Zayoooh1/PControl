#include "process.hpp"
#include <algorithm>
#include <array>
#include <tlhelp32.h>
namespace pc {
namespace {
std::wstring windowsDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    UINT length = GetWindowsDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (!length)
        check(false, "GetWindowsDirectory");
    if (length >= path.size()) {
        path.resize(static_cast<size_t>(length) + 1);
        length = GetWindowsDirectoryW(path.data(), static_cast<UINT>(path.size()));
        if (!length || length >= path.size())
            throw Error(ERROR_BUFFER_OVERFLOW, "Windows directory path unavailable");
    }
    path.resize(length);
    return path;
}
std::wstring normalizedPath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    return path;
}
bool equalsIgnoreCase(const std::wstring &left, const std::wstring &right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
Handle open(DWORD pid, uint64_t expected, DWORD access) {
    Handle h(OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    check(bool(h), "OpenProcess PID " + std::to_string(pid));
    if (!expected || creation(h) != expected)
        throw Error(ERROR_INVALID_PARAMETER, "Stale or unknown process identity (PID reuse)");
    DWORD exit = 0;
    check(GetExitCodeProcess(h, &exit), "Process state");
    if (exit != STILL_ACTIVE)
        throw Error(ERROR_PROCESS_ABORTED, "Process terminated");
    return h;
}
Json inspect(HANDLE h) {
    Json r;
    r["creationTime"] = std::to_string(creation(h));
    wchar_t path[32768];
    DWORD n = 32768;
    if (QueryFullProcessImageNameW(h, 0, path, &n))
        r["path"] = utf8(std::wstring(path, n));
    else
        r["pathError"] = GetLastError();
    DWORD pr = GetPriorityClass(h);
    r["priority"] = pr;
    if (!pr)
        r["priorityError"] = GetLastError();
    DWORD_PTR p = 0, s = 0;
    bool affinity = GetProcessAffinityMask(h, &p, &s) != FALSE;
    r["affinityKnown"] = affinity && p != 0 && GetActiveProcessorGroupCount() == 1;
    r["affinityMask"] = std::to_string(p);
    r["systemMask"] = std::to_string(s);
    if (!affinity)
        r["affinityError"] = GetLastError();
    BOOL wow = FALSE;
    if (IsWow64Process(h, &wow))
        r["architecture"] = wow ? "x86" : "x64";
    else
        r["architecture"] = "Unknown";
    return r;
}
} // namespace
bool isWindowsSystemProcess(const std::wstring &path, const std::wstring &name, DWORD pid,
                            const std::wstring &windowsRoot) {
    const auto executable = normalizedPath(path);
    const auto root = normalizedPath(windowsRoot);
    if (!executable.empty() && !root.empty() && executable.size() > root.size() &&
        CompareStringOrdinal(executable.c_str(), static_cast<int>(root.size()), root.c_str(),
                             static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
        executable[root.size()] == L'\\')
        return true;

    static constexpr std::array<const wchar_t *, 13> fallback = {
        L"System Idle Process", L"[System Process]", L"System",       L"Registry",  L"smss.exe",
        L"csrss.exe",           L"wininit.exe",      L"services.exe", L"lsass.exe", L"fontdrvhost.exe",
        L"winlogon.exe",        L"svchost.exe",      L"dwm.exe"};
    if (!executable.empty())
        return false;
    if (pid == 0 || pid == 4)
        return true;
    return std::any_of(fallback.begin(), fallback.end(),
                       [&](const wchar_t *candidate) { return equalsIgnoreCase(name, candidate); });
}
void sortProcessRows(Json &rows) {
    std::stable_sort(rows.begin(), rows.end(), [](const Json &left, const Json &right) {
        bool leftSystem = left.value("isWindowsSystemProcess", false);
        bool rightSystem = right.value("isWindowsSystemProcess", false);
        if (leftSystem != rightSystem)
            return !leftSystem;
        auto leftName = wide(left.value("name", ""));
        auto rightName = wide(right.value("name", ""));
        int names = CompareStringOrdinal(leftName.c_str(), static_cast<int>(leftName.size()),
                                         rightName.c_str(), static_cast<int>(rightName.size()), TRUE);
        if (names != CSTR_EQUAL)
            return names == CSTR_LESS_THAN;
        return left.value("pid", DWORD{}) < right.value("pid", DWORD{});
    });
}
bool validPriority(DWORD v) {
    return v == IDLE_PRIORITY_CLASS || v == BELOW_NORMAL_PRIORITY_CLASS || v == NORMAL_PRIORITY_CLASS ||
           v == ABOVE_NORMAL_PRIORITY_CLASS || v == HIGH_PRIORITY_CLASS || v == REALTIME_PRIORITY_CLASS;
}
DWORD_PTR cpuMask(const Json &v) {
    if (GetActiveProcessorGroupCount() != 1)
        throw Error(ERROR_NOT_SUPPORTED, "Stage 1 affinity supports one processor group only");
    if (unsigned32(v.at("group")) != 0)
        throw Error(ERROR_NOT_SUPPORTED, "Unsupported processor group");
    const auto &cpus = v.at("cpus");
    if (!cpus.is_array() || cpus.empty())
        throw Error(ERROR_INVALID_PARAMETER, "Select at least one CPU");
    DWORD count = GetActiveProcessorCount(0);
    DWORD_PTR mask = 0;
    for (auto &x : cpus) {
        if (!x.is_number_integer())
            throw Error(ERROR_INVALID_PARAMETER, "Invalid CPU index");
        DWORD c = unsigned32(x);
        if (c >= 64 || c >= count)
            throw Error(ERROR_INVALID_PARAMETER, "CPU outside active group");
        mask |= DWORD_PTR(1) << c;
    }
    return mask;
}
Json enumerate() {
    const auto windowsRoot = windowsDirectory();
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    check(bool(snap), "Process snapshot");
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    Json rows = Json::array();
    BOOL more = Process32FirstW(snap, &e);
    while (more) {
        Json p = {{"pid", e.th32ProcessID}, {"name", utf8(e.szExeFile)},
                  {"creationTime", "0"},    {"path", ""},
                  {"priority", 0},          {"architecture", "Unknown"},
                  {"affinityKnown", false}, {"affinityMask", "0"},
                  {"systemMask", "0"},      {"accessError", 0}};
        DWORD session = 0;
        if (ProcessIdToSessionId(e.th32ProcessID, &session))
            p["session"] = session;
        Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID));
        if (h) {
            try {
                p.update(inspect(h));
            } catch (const Error &x) {
                p["accessError"] = x.code;
            }
        } else
            p["accessError"] = GetLastError();
        p["isWindowsSystemProcess"] =
            isWindowsSystemProcess(wide(p.value("path", "")), e.szExeFile, e.th32ProcessID, windowsRoot);
        rows.push_back(p);
        more = Process32NextW(snap, &e);
    }
    if (GetLastError() != ERROR_NO_MORE_FILES)
        check(false, "Enumerate processes");
    sortProcessRows(rows);
    return rows;
}
Json details(DWORD pid, uint64_t expected) {
    auto h = open(pid, expected, 0);
    auto r = inspect(h);
    r["pid"] = pid;
    return r;
}
Json change(DWORD pid, uint64_t expected, const std::string &kind, const Json &v) {
    auto h = open(pid, expected, PROCESS_SET_INFORMATION);
    if (kind == "priority") {
        DWORD p = unsigned32(v);
        if (!validPriority(p))
            throw Error(ERROR_INVALID_PARAMETER, "Invalid priority class");
        check(SetPriorityClass(h, p), "SetPriorityClass PID " + std::to_string(pid));
    } else {
        auto mask = cpuMask(v);
        DWORD_PTR old = 0, system = 0;
        check(GetProcessAffinityMask(h, &old, &system), "GetProcessAffinityMask");
        if ((mask & system) != mask)
            throw Error(ERROR_INVALID_PARAMETER, "CPU unavailable for target process");
        check(SetProcessAffinityMask(h, mask), "SetProcessAffinityMask PID " + std::to_string(pid));
    }
    return inspect(h);
}
} // namespace pc
