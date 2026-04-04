/**
 * @file config_loader.cpp
 * @brief ConfigLoader实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "../include/indurtdb/core/point_manager_interface.hpp"

namespace indurtdb {
namespace core {

class ConfigLoader {
public:
    ConfigLoader() = default;
    
    ~ConfigLoader() = default;
    
    bool load(const std::string& path) {
        (void)path;
        return false;
    }
    
    bool save(const std::string& path) {
        (void)path;
        return false;
    }
};

} // namespace core
} // namespace indurtdb
