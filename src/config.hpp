#pragma once
#include "shared.hpp"
namespace pc {
Json defaults();
void validateConfig(const Json &config);
class Config {
    std::filesystem::path path_;

  public:
    explicit Config(const std::filesystem::path &d) : path_(d / L"config.json") {}
    Json load();
    void save(const Json &j);
};
} // namespace pc
