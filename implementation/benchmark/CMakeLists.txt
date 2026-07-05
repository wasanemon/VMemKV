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
