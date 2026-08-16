cmake_minimum_required(VERSION 3.16)

# ─── reflink-capable test volume ─────────────────────────────────────────────
# checkpoint_and_defragment() requires ioctl(FICLONE) (see TODO.md item 5), which the system temp
# directory is not guaranteed to support (e.g. plain ext4). This provisions/verifies a small
# loopback XFS (reflink=1) volume -- see scripts/ensure_xfs_dev_volume.sh's own comment for the
# one-time sudo step it needs and can't perform unattended. Runs on every build (idempotent, exits
# immediately once the volume already exists) so a stale/unmounted volume from a previous machine
# boot gets caught before tests silently fail on a non-reflink filesystem.
# Deliberately NOT an add_dependencies() of test_kv_store: the underlying script's one-time
# mkfs/mount step needs an interactive sudo password (see the script's own comment), so making the
# *build* depend on it would break `cmake --build` on a machine where it hasn't been run yet
# instead of just leaving tests to fail at run time with a clear cause. Invoke explicitly
# (`cmake --build build --target ensure_xfs_dev_volume`) once per machine, or wire it into CI.
set(VMEMKV_TEST_XFS_MOUNT "/mnt/vmemkv-xfs" CACHE PATH "Reflink-capable mount point used for test-created T2/WAL/checkpoint files")
add_custom_target(ensure_xfs_dev_volume
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ensure_xfs_dev_volume.sh ${VMEMKV_TEST_XFS_MOUNT}
  COMMENT "Ensuring ${VMEMKV_TEST_XFS_MOUNT} is a mounted, reflink-capable XFS volume"
  VERBATIM
)

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
set_tests_properties(test_kv_store PROPERTIES ENVIRONMENT "VMEMKV_TEST_TMPDIR=${VMEMKV_TEST_XFS_MOUNT}")
# 600s: this suite normally finishes in ~1-2 minutes even on CI, so this is a generous ceiling for
# a healthy run, not a budget to fill -- CI runner time isn't free. It's also comfortably past the
# 300s mark where ci.yml's watchdog dumps thread backtraces if the process is still alive, so a
# genuine hang still gets diagnosed (via the watchdog) well before this kills it.
set_tests_properties(test_kv_store PROPERTIES TIMEOUT 600)
