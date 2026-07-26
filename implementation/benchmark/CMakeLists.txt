include(FetchContent)

# Google Benchmark
FetchContent_Declare(
  google_benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.4
  GIT_SHALLOW    TRUE
)

# Disable unnecessary parts of Google Benchmark to speed up build
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable testing" FORCE)
set(BENCHMARK_ENABLE_EXCEPTIONS ON CACHE BOOL "Enable exceptions" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "Disable install" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "Disable gtest" FORCE)
set(BENCHMARK_ENABLE_ASSEMBLY_TESTS OFF CACHE BOOL "Disable assembly tests" FORCE)

FetchContent_MakeAvailable(google_benchmark)

add_executable(bench_kv bench_kv.cpp)
target_link_libraries(bench_kv PRIVATE vmemkv vmemkv_headers benchmark::benchmark)

if(ENABLE_ROCKSDB AND RocksDB_FOUND)
  target_compile_definitions(bench_kv PRIVATE ENABLE_ROCKSDB)
  target_link_libraries(bench_kv PRIVATE RocksDB::rocksdb)
endif()

if(ENABLE_LMDB AND LMDB_FOUND)
  target_compile_definitions(bench_kv PRIVATE ENABLE_LMDB)
  target_link_libraries(bench_kv PRIVATE LMDB::lmdb)
endif()

option(ENABLE_GPERFTOOLS_PROFILER "Enable gperftools CPU profiler support for bench_kv" ON)
if(ENABLE_GPERFTOOLS_PROFILER)
  find_library(GPERFTOOLS_PROFILER_LIB
    NAMES profiler
    HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib)
  if(GPERFTOOLS_PROFILER_LIB)
    target_compile_definitions(bench_kv PRIVATE VMEMKV_HAVE_GPERFTOOLS_PROFILER)
    target_link_libraries(bench_kv PRIVATE "${GPERFTOOLS_PROFILER_LIB}")
    message(STATUS "Enabling gperftools CPU profiler: ${GPERFTOOLS_PROFILER_LIB}")
  else()
    message(STATUS "gperftools profiler not found; bench_kv will build without CPU profiler support")
  endif()
endif()
