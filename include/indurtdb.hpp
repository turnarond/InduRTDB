/**
 * @file indurtdb.hpp
 * @brief InduRTDB 主包含文件
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#pragma once

// Version information
#define INDURTDB_VERSION_MAJOR 2
#define INDURTDB_VERSION_MINOR 0
#define INDURTDB_VERSION_PATCH 0
#define INDURTDB_VERSION_STRING "2.0.0"

// Basic types
#include "indurtdb/types/basic_types.hpp"

// Memory layout
#include "indurtdb/types/memory_layout.hpp"

// OSAL interfaces
#include "indurtdb/osal/interface.hpp"

// Core
#include "indurtdb/core/seqlock.hpp"
#include "indurtdb/core/point_manager_interface.hpp"
#include "indurtdb/core/subscription_manager_interface.hpp"
#include "indurtdb/core/config_loader.hpp"

// Main C++ API
#include "indurtdb/api/indurtdb.hpp"

// C ABI
#include "indurtdb/api/c/indurtdb_c.h"
