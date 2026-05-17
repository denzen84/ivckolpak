#pragma once
#include <memory>
#include <unordered_map>
#include <string>
#include <mutex>
#include "config.h"

class ConfigRepository {
public:
    static ConfigRepository& instance() {
        static ConfigRepository repo;
        return repo;
    }
    void register_camera(std::string id, std::shared_ptr<const CameraConfig> cfg) {
        std::lock_guard lock(mtx_);
        cameras_[std::move(id)] = std::move(cfg);
    }
    std::shared_ptr<const CameraConfig> get_camera(const std::string& id) const {
        std::lock_guard lock(mtx_);
        auto it = cameras_.find(id);
        return it != cameras_.end() ? it->second : nullptr;
    }
    void set_global(std::shared_ptr<const GlobalConfig> cfg) {
        std::lock_guard lock(mtx_);
        global_ = std::move(cfg);
    }
    std::shared_ptr<const GlobalConfig> get_global() const {
        std::lock_guard lock(mtx_);
        return global_;
    }
private:
    ConfigRepository() = default;
    mutable std::mutex mtx_;
    std::shared_ptr<const GlobalConfig> global_;
    std::unordered_map<std::string, std::shared_ptr<const CameraConfig>> cameras_;
};