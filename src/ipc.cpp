#include "ipc.hpp"
#include <sddl.h>
namespace pc {
namespace {
constexpr DWORD limit = 4 * 1024 * 1024;
bool complete(HANDLE h, OVERLAPPED &o, BOOL immediate, DWORD &count, DWORD timeout) {
    if (immediate)
        return true;
    if (GetLastError() != ERROR_IO_PENDING)
        return false;
    if (WaitForSingleObject(o.hEvent, timeout) != WAIT_OBJECT_0) {
        CancelIoEx(h, &o);
        GetOverlappedResult(h, &o, &count, TRUE);
        SetLastError(ERROR_TIMEOUT);
        return false;
    }
    return GetOverlappedResult(h, &o, &count, FALSE) != FALSE;
}
void transfer(HANDLE h, void *buffer, DWORD size, bool writing) {
    auto p = static_cast<BYTE *>(buffer);
    auto end = GetTickCount64() + 3000;
    while (size) {
        Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        check(bool(ev), "IPC event");
        OVERLAPPED o{};
        o.hEvent = ev;
        DWORD n = 0;
        BOOL ok = writing ? WriteFile(h, p, size, &n, &o) : ReadFile(h, p, size, &n, &o);
        DWORD remaining = GetTickCount64() < end ? static_cast<DWORD>(end - GetTickCount64()) : 0;
        check(complete(h, o, ok, n, remaining), writing ? "IPC write" : "IPC read");
        if (!n)
            throw Error(ERROR_BROKEN_PIPE, "IPC disconnected");
        p += n;
        size -= n;
    }
}
Json read(HANDLE h) {
    DWORD n = 0;
    transfer(h, &n, 4, false);
    if (!n || n > limit)
        throw Error(ERROR_INVALID_DATA, "IPC frame too large or empty");
    std::string s(n, 0);
    transfer(h, s.data(), n, false);
    return parseJson(s);
}
void write(HANDLE h, const Json &j) {
    auto s = j.dump();
    if (s.size() > limit)
        throw Error(ERROR_BUFFER_OVERFLOW, "IPC response exceeds limit");
    DWORD n = (DWORD)s.size();
    transfer(h, &n, 4, true);
    transfer(h, s.data(), n, true);
}
} // namespace
Json request(Json message) {
    auto name = pipeName();
    WaitNamedPipeW(name.c_str(), 1000);
    Handle h(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    check(bool(h), "Connect Core");
    message["version"] = message.value("version", 1);
    write(h, message);
    auto result = read(h);
    BYTE ack = 1;
    transfer(h, &ack, 1, true);
    if (result.value("version", 0) != 1)
        throw Error(ERROR_REVISION_MISMATCH, "Core protocol mismatch");
    return result;
}
void serve(std::atomic_bool &stop, const std::function<Json(const Json &)> &dispatch,
           const std::function<void(const std::string &)> &event) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    auto acl = L"D:P(A;;GA;;;" + identity() + L")";
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &sd, nullptr),
          "Pipe ACL");
    struct Free {
        PSECURITY_DESCRIPTOR p;
        ~Free() {
            LocalFree(p);
        }
    } cleanup{sd};
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    auto name = pipeName();
    while (!stop) {
        Handle h(CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0,
            &sa));
        check(bool(h), "Create pipe");
        Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        check(bool(ev), "Connect event");
        OVERLAPPED o{};
        o.hEvent = ev;
        BOOL connected = ConnectNamedPipe(h, &o);
        DWORD e = connected ? 0 : GetLastError();
        if (e == ERROR_IO_PENDING) {
            while (!stop && WaitForSingleObject(ev, 200) == WAIT_TIMEOUT) {
            }
            DWORD n = 0;
            if (stop) {
                CancelIoEx(h, &o);
                GetOverlappedResult(h, &o, &n, TRUE);
                break;
            }
            check(GetOverlappedResult(h, &o, &n, FALSE), "Accept pipe");
        } else if (e && e != ERROR_PIPE_CONNECTED)
            throw Error(e, "Accept pipe failed");
        try {
            ULONG session = 0;
            DWORD own = 0;
            check(GetNamedPipeClientSessionId(h, &session), "Client session");
            check(ProcessIdToSessionId(GetCurrentProcessId(), &own), "Own session");
            if (session != own)
                throw Error(ERROR_ACCESS_DENIED, "Different client session");
            event("GUI connected");
            Json r;
            try {
                auto q = read(h);
                r = dispatch(q);
            } catch (const Error &x) {
                event(std::string("IPC error: ") + x.what());
                r = failure(x.code, x.what());
            } catch (const std::exception &x) {
                event(std::string("Malformed request: ") + x.what());
                r = failure(ERROR_INVALID_DATA, x.what());
            }
            write(h, r);
            BYTE ack = 0;
            transfer(h, &ack, 1, false);
            if (ack != 1)
                throw Error(ERROR_INVALID_DATA, "Invalid response acknowledgement");
        } catch (const std::exception &x) {
            event(std::string("IPC error / malformed request: ") + x.what());
        }
        DisconnectNamedPipe(h);
        event("GUI disconnected");
    }
}
} // namespace pc
