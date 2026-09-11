#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
namespace pc {
using Json = nlohmann::json;
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&h) noexcept : value(h.value) {
        h.value = nullptr;
    }
    explicit operator bool() const {
        return value && value != INVALID_HANDLE_VALUE;
    }
    operator HANDLE() const {
        return value;
    }
};
struct Error : std::runtime_error {
    DWORD code;
    Error(DWORD c, const std::string &m) : runtime_error(m), code(c) {}
};
void check(bool ok, const std::string &operation);
std::string utf8(const std::wstring &s);
std::wstring wide(const std::string &s);
std::wstring identity();
std::wstring pipeName();
std::filesystem::path dataDir();
Json success(Json data = Json::object());
Json failure(DWORD code, const std::string &message);
uint64_t decimal(const std::string &value);
DWORD unsigned32(const Json &value);
Json parseJson(const std::string &text);
uint64_t creation(HANDLE process);
} // namespace pc
