cmake_minimum_required(VERSION 3.16)
project(pskiplist CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(pskiplist INTERFACE)
target_include_directories(pskiplist INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(pskiplist_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(pskiplist_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  target_compile_options(pskiplist_warnings INTERFACE /W4 /WX)
endif()

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

enable_testing()
add_subdirectory(tests)
