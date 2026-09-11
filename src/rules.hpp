#pragma once
#include "logger.hpp"
#include "shared.hpp"
#include <set>
namespace pc {
class Rules {
    std::set<std::string> applied_;

  public:
    void reset() {
        applied_.clear();
    }
    void apply(const Json &rows, const Json &config, Logger &log);
};
} // namespace pc
