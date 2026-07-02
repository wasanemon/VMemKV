cmake_minimum_required(VERSION 3.16)
project(VMemKV CXX)

# Allow older 3rd-party CMake files that use cmake_minimum_required(VERSION 3.0)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

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

add_library(vmemkv_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(vmemkv_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(vmemkv_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  target_compile_options(vmemkv_warnings INTERFACE /W4 /WX)
endif()

# ── Static library for source implementation ─────────────────────────────────
add_library(vmemkv STATIC
  src/t2_flat_file/t2_flat_file.cpp
)
target_link_libraries(vmemkv PUBLIC vmemkv_headers vmemkv_warnings)

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
  include(CTest)
  add_subdirectory(tests)
endif()

# ── Benchmarks ────────────────────────────────────────────────────────────────
option(ENABLE_BENCHMARK "Build nanobench suite" ON)
if(ENABLE_BENCHMARK)
  add_subdirectory(benchmark)
endif()

# ── Git Hooks Auto-Configuration ──────────────────────────────────────────────
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/../.git")
  message(STATUS "Configuring Git hooks path...")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} config core.hooksPath "implementation/githooks"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
    RESULT_VARIABLE GIT_HOOKS_RESULT
  )
  if(NOT GIT_HOOKS_RESULT EQUAL 0)
    message(WARNING "Failed to configure Git hooks path.")
  else()
    message(STATUS "Git hooks path successfully set to implementation/githooks")
  endif()
  
  # Make the hook executable on UNIX systems
  if(UNIX)
    execute_process(
      COMMAND chmod +x "${CMAKE_SOURCE_DIR}/githooks/pre-commit"
      RESULT_VARIABLE CHMOD_RESULT
    )
  endif()
endif()

# ── clang-tidy Static Analysis Target (CI-only) ──────────────────────────────
find_program(CLANG_TIDY_PROGRAM clang-tidy)
if(CLANG_TIDY_PROGRAM)
  set(CLANG_TIDY_HEADER_FILTER "(^|.*/)(src|include/vmemkv)/.*\\.(hpp|cpp)$")

  # Find all C++ files for analysis
  file(GLOB_RECURSE ALL_CXX_FILES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
  )
  string(JOIN " " ALL_CXX_FILES_JOINED ${ALL_CXX_FILES})
  
  # Create a custom target for clang-tidy
  add_custom_target(clang-tidy-check
    COMMAND bash -lc "set -o pipefail; ${CLANG_TIDY_PROGRAM} -quiet --extra-arg=-w --header-filter='${CLANG_TIDY_HEADER_FILTER}' -p ${CMAKE_BINARY_DIR} ${ALL_CXX_FILES_JOINED} 2>&1 | sed -E '/^[0-9]+ warnings generated\\.$/d; /Suppressed [0-9]+ warnings/d; /Use -header-filter=\\.\\*/d; /Use -system-headers/d'"
    COMMENT "Running clang-tidy static analysis..."
    VERBATIM
  )
endif()
