include(FetchContent)

# nanobench — header-only, no separate build step needed
FetchContent_Declare(
  nanobench
  GIT_REPOSITORY https://github.com/martinus/nanobench.git
  GIT_TAG        v4.3.11
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nanobench)

add_executable(bench_kv bench_kv.cpp)
target_link_libraries(bench_kv PRIVATE vmemkv vmemkv_headers nanobench::nanobench)
target_compile_options(bench_kv PRIVATE -O3)


if(ENABLE_ROCKSDB AND RocksDB_FOUND)
  target_compile_definitions(bench_kv PRIVATE ENABLE_ROCKSDB)
  target_link_libraries(bench_kv PRIVATE RocksDB::rocksdb)
endif()
