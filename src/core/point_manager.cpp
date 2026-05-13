/**
 * @file point_manager.cpp
 * @brief PointManager —— 所有模板实现在头文件中
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#include <indurtdb/core/point_manager_interface.hpp>

namespace indurtdb {
namespace core {

// 显式模板实例化 —— 编译期生成四种类型的 write 方法
template bool PointManager::write<bool>(PointId, const bool&);
template bool PointManager::write<int32_t>(PointId, const int32_t&);
template bool PointManager::write<double>(PointId, const double&);
template bool PointManager::write<const char*>(PointId, const char* const&);

} // namespace core
} // namespace indurtdb
