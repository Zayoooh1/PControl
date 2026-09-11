#pragma once
#include "config.hpp"
#include "rules.hpp"
#include <atomic>
#include <condition_variable>
#include <thread>
namespace pc {
class Engine {
    Logger log_;
    Config store_;
    Json config_, snapshot_ = Json::array();
    Rules rules_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread monitor_;
    bool stop_ = false, dirty_ = true, configHealthy_ = true;
    void monitor();

  public:
    explicit Engine(const std::filesystem::path &directory);
    ~Engine();
    Json dispatch(const Json &q);
    void event(const std::string &m) {
        log_.log("INFO", "IPC", m);
    }
};
} // namespace pc
