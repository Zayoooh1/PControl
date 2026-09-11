#include "process.hpp"
#include <tlhelp32.h>
namespace pc {
namespace {
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
        rows.push_back(p);
        more = Process32NextW(snap, &e);
    }
    if (GetLastError() != ERROR_NO_MORE_FILES)
        check(false, "Enumerate processes");
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
