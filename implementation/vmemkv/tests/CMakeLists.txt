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
# --duration=true: doctest prints each TEST CASE's file/line/name as it *starts*, not just a
# final summary -- combined with test_main.cpp's forced line-buffering (see its comment), this is
# what makes a hung test identifiable in CI at all: CTest pipes the binary's stdout (non-TTY), so
# without the buffering fix any output sitting unflushed at kill time is lost, and without
# --duration doctest doesn't print per-test progress in the first place, only a final summary.
add_test(NAME test_kv_store COMMAND test_kv_store --duration=true)
# 600s: this suite normally finishes in ~1-2 minutes even on CI, so this is a generous ceiling for
# a healthy run, not a budget to fill -- CI runner time isn't free. It's also comfortably past the
# 300s mark where ci.yml's watchdog dumps thread backtraces if the process is still alive, so a
# genuine hang still gets diagnosed (via the watchdog) well before this kills it.
set_tests_properties(test_kv_store PROPERTIES TIMEOUT 600)
