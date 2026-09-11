#pragma once
#include "shared.hpp"
#include <mutex>
namespace pc {
class Logger {
    std::filesystem::path path_;
    std::mutex mutex_;

  public:
    explicit Logger(const std::filesystem::path &dir);
    void log(const std::string &severity, const std::string &category, const std::string &message) noexcept;
};
} // namespace pc
