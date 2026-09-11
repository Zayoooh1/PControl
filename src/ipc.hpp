#pragma once
#include "shared.hpp"
#include <atomic>
#include <functional>
namespace pc {
Json request(Json message);
void serve(std::atomic_bool &stop, const std::function<Json(const Json &)> &dispatch,
           const std::function<void(const std::string &)> &event);
} // namespace pc
