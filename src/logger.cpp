#include "logger.hpp"
#include <fstream>
#include <iomanip>
namespace pc {
Logger::Logger(const std::filesystem::path &d) : path_(d / L"Core.log") {
    std::filesystem::create_directories(d);
}
void Logger::log(const std::string &s, const std::string &c, const std::string &m) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (std::filesystem::exists(path_) && std::filesystem::file_size(path_) > 2 * 1024 * 1024) {
            auto backup = path_;
            backup += L".1";
            std::filesystem::remove(backup);
            std::filesystem::rename(path_, backup);
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::ofstream f(path_, std::ios::app);
        f.exceptions(std::ios::failbit | std::ios::badbit);
        f << std::setfill('0') << std::setw(4) << t.wYear << '-' << std::setw(2) << t.wMonth << '-'
          << std::setw(2) << t.wDay << ' ' << std::setw(2) << t.wHour << ':' << std::setw(2) << t.wMinute
          << ':' << std::setw(2) << t.wSecond << '.' << std::setw(3) << t.wMilliseconds << " [" << s << "] ["
          << c << "] " << Json(m).dump() << '\n';
        f.flush();
    } catch (...) {
        OutputDebugStringW(L"PControl logging failed\n");
    }
}
} // namespace pc
