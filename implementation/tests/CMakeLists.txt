cmake_minimum_required(VERSION 3.16)

# ─── test_kv_store ────────────────────────────────────────────────────────────
add_executable(test_kv_store test_kv_store.cpp test_wal.cpp test_checkpoint.cpp test_crash_recovery.cpp test_t1_index.cpp test_t2_flat_file.cpp test_main.cpp)
target_link_libraries(test_kv_store PRIVATE vmemkv vmemkv_headers doctest::doctest)


if(ENABLE_ROCKSDB AND RocksDB_FOUND)
  target_compile_definitions(test_kv_store PRIVATE ENABLE_ROCKSDB=1)
  target_link_libraries(test_kv_store PRIVATE RocksDB::rocksdb)
endif()

if(ENABLE_LMDB AND LMDB_FOUND)
  target_compile_definitions(test_kv_store PRIVATE ENABLE_LMDB=1)
  target_link_libraries(test_kv_store PRIVATE LMDB::lmdb)
endif()
add_test(NAME test_kv_store COMMAND test_kv_store)
