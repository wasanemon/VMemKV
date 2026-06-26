cmake_minimum_required(VERSION 3.16)
project(mincore_kv_bench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(bench bench.cpp)
target_compile_options(bench PRIVATE -O3 -march=native)
