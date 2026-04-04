/**
 * @file indurtdb.hpp
 * @brief Main include file for InduRTDB library
 * @version 1.0.0
 * @date 2026-03-27
 * 
 * @copyright MIT License
 */

#pragma once

// Version information
#define INDURTDB_VERSION_MAJOR 1
#define INDURTDB_VERSION_MINOR 0
#define INDURTDB_VERSION_PATCH 0
#define INDURTDB_VERSION_STRING "1.0.0"

// Basic types
#include "indurtdb/types/basic_types.hpp"

// Memory layout
#include "indurtdb/types/memory_layout.hpp"

// OSAL interfaces
#include "indurtdb/osal/interface.hpp"

// Core interfaces
#include "indurtdb/core/point_manager_interface.hpp"

// Main API
#include "indurtdb/api/indurtdb.hpp"

// C ABI
#include "indurtdb/api/c/indurtdb_c.h"