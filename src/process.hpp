#pragma once
#include "shared.hpp"
namespace pc {
Json enumerate();
Json details(DWORD pid, uint64_t expected);
Json change(DWORD pid, uint64_t expected, const std::string &kind, const Json &value);
bool validPriority(DWORD value);
DWORD_PTR cpuMask(const Json &selection);
} // namespace pc
