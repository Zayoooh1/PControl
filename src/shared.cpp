#include "shared.hpp"
#include <charconv>
#include <sddl.h>
#include <shlobj.h>
namespace pc {
void check(bool ok, const std::string &op) {
    if (!ok) {
        auto c = GetLastError();
        LPWSTR p = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, c, 0, (LPWSTR)&p, 0, nullptr);
        std::string m = op + ": " + (p ? utf8(p) : "Windows error");
        if (p)
            LocalFree(p);
        throw Error(c, m);
    }
}
std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr,
                                nullptr);
    check(n > 0, "UTF-8");
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    check(n > 0, "UTF-16");
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}
std::wstring identity() {
    Handle t;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t.value), "Token");
    DWORD n = 0;
    GetTokenInformation(t, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> b(n);
    check(GetTokenInformation(t, TokenUser, b.data(), n, &n), "Token user");
    LPWSTR s = nullptr;
    check(ConvertSidToStringSidW(((TOKEN_USER *)b.data())->User.Sid, &s), "SID");
    std::wstring r = s;
    LocalFree(s);
    return r;
}
std::wstring pipeName() {
    DWORD session = 0;
    check(ProcessIdToSessionId(GetCurrentProcessId(), &session), "Session");
    return L"\\\\.\\pipe\\PControl.v1." + identity() + L"." + std::to_wstring(session);
}
std::filesystem::path dataDir() {
    wchar_t overridePath[32768];
    DWORD n = GetEnvironmentVariableW(L"PCONTROL_DATA_DIR", overridePath, 32768);
    if (n && n < 32768)
        return overridePath;
    PWSTR p = nullptr;
    auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p);
    if (FAILED(hr))
        throw Error((DWORD)hr, "LocalAppData unavailable");
    std::filesystem::path r = p;
    CoTaskMemFree(p);
    return r / L"PControl";
}
Json success(Json data) {
    return {{"version", 1}, {"success", true}, {"errorCode", 0}, {"errorMessage", ""}, {"data", data}};
}
Json failure(DWORD c, const std::string &m) {
    return {{"version", 1}, {"success", false}, {"errorCode", c}, {"errorMessage", m}, {"data", nullptr}};
}
uint64_t decimal(const std::string &s) {
    uint64_t r = 0;
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), r);
    if (s.empty() || e != std::errc() || p != s.data() + s.size())
        throw Error(ERROR_INVALID_PARAMETER, "Invalid unsigned decimal");
    return r;
}
uint64_t creation(HANDLE p) {
    FILETIME c, e, k, u;
    check(GetProcessTimes(p, &c, &e, &k, &u), "GetProcessTimes");
    return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}
DWORD unsigned32(const Json &v) {
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<int64_t>() < 0) ||
        v.get<uint64_t>() > UINT32_MAX)
        throw Error(ERROR_INVALID_PARAMETER, "Expected unsigned 32-bit integer");
    return v.get<DWORD>();
}
Json parseJson(const std::string &text) {
    return Json::parse(text, [](int depth, Json::parse_event_t, Json &) {
        if (depth > 32)
            throw Error(ERROR_INVALID_DATA, "JSON nesting exceeds 32 levels");
        return true;
    });
}
} // namespace pc
