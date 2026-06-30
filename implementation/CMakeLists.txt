cmake_minimum_required(VERSION 3.16)
project(VMemKV CXX)

# Allow older 3rd-party CMake files that use cmake_minimum_required(VERSION 3.0)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ── Interface library for headers ─────────────────────────────────────────────
option(ENABLE_ROCKSDB "Build with RocksDB backend" ON)
add_library(vmemkv_headers INTERFACE)
target_include_directories(vmemkv_headers INTERFACE 
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── Static library for source implementation ─────────────────────────────────
add_library(vmemkv STATIC
  src/t2_flat_file/t2_flat_file.cpp
)
target_link_libraries(vmemkv PUBLIC vmemkv_headers)

# ── doctest (single-header, header-only test framework) ─────────────────────
include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    TRUE
)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

# ── RocksDB (optional) ────────────────────────────────────────────────────────
if(ENABLE_ROCKSDB)
  find_package(RocksDB QUIET)
  if(NOT RocksDB_FOUND)
    find_library(ROCKSDB_LIB rocksdb
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
    find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
    if(ROCKSDB_LIB AND ROCKSDB_INCLUDE_DIR)
      add_library(RocksDB::rocksdb UNKNOWN IMPORTED)
      set_target_properties(RocksDB::rocksdb PROPERTIES
        IMPORTED_LOCATION "${ROCKSDB_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_INCLUDE_DIR}")
      set(RocksDB_FOUND TRUE)
      message(STATUS "Found RocksDB: ${ROCKSDB_LIB}")
    else()
      message(FATAL_ERROR "ENABLE_ROCKSDB=ON but RocksDB not found.")
    endif()
  endif()
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
option(ENABLE_TESTS "Build tests" ON)
if(ENABLE_TESTS)
  add_subdirectory(tests)
endif()

# ── Benchmarks ────────────────────────────────────────────────────────────────
option(ENABLE_BENCHMARK "Build nanobench suite" ON)
if(ENABLE_BENCHMARK)
  add_subdirectory(benchmark)
endif()
