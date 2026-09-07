// test_support.hpp - Shared helpers for the test_kv_store binary's constituent .cpp files.
#pragma once

#include <unistd.h>

#include <atomic>
#include <checkpoint/checkpoint.hpp>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vmemkv/config.hpp>
#include <wal/wal.hpp>

namespace vmemkv_test {

// Config override providing a small T1 append-region capacity (2^Log2 entries) so
// reorganize()/split triggers without needing thousands of puts per test run. Both fields must be
// redeclared: Entries is computed from Log2 inside Config<>'s own scope, so overriding Log2 alone
// would silently leave Entries at the base 2^22 default.
template <size_t Log2>
struct TinyAppendConfig : vmemkv::Config<> {
  static constexpr size_t T1AppendCapacityLog2 = Log2;
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << Log2;
};

inline auto to_span(const std::string &text) -> std::span<const std::byte> {
  return std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size());
}

// Wraps a per-entry function into T1Index::reorganize()'s/ShardedT1Index::checkpoint_all_shards()'s
// OffsetMapper contract (a single batch call over the whole merged span): invokes `fn` once per
// entry, in order, so a test's mapper can block *inside* it to freeze a reorganize() call
// mid-flight for a race test with exact per-entry timing. A generic (`auto`) span parameter lets
// one adapter serve every EntrySnapshot type across callers.
template <typename PerEntryFn>
auto per_entry_offset_mapper(PerEntryFn fn) {
  return [fn = std::move(fn)](auto merged) {
    for (auto &entry : merged) {
      entry.payload_bits = fn(entry.payload_bits, entry.hash);
    }
  };
}

// Base directory for test-created T2/WAL/checkpoint files. Sourced from VMEMKV_TEST_TMPDIR if
// set, falling back to the system temp directory otherwise.
inline auto test_temp_root() -> const std::filesystem::path & {
  static const std::filesystem::path root = [] {
    if (const char *env = std::getenv("VMEMKV_TEST_TMPDIR"); env != nullptr && *env != '\0') {
      return std::filesystem::path(env);
    }
    return std::filesystem::temp_directory_path();
  }();
  return root;
}

// Builds a unique-per-process-per-call temp file path under `prefix` (pid + a monotonic counter
// shared across every test file that calls this), and removes any stale file left at that path
// (and, if requested, its WAL sibling) by a previous run.
inline auto reserve_unique_temp_path(std::string_view prefix,
                                     bool also_remove_wal_sibling = false) -> std::filesystem::path {
  static std::atomic<uint64_t> counter{0};
  const uint64_t sequence_number = counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp_path =
      test_temp_root() / (std::string(prefix) + "_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                          std::to_string(sequence_number));
  std::error_code ignored;
  std::filesystem::remove(temp_path, ignored);
  if (also_remove_wal_sibling) {
    vmemkv::remove_wal_segments(vmemkv::derive_wal_path(temp_path));
  }
  return temp_path;
}

// Removes every file a VMemKVImpl store could have created at `t2_path`: the T2 data file, its
// WAL segments, manifest, and T1/T2 checkpoint files.
inline void remove_store_files(const std::filesystem::path &t2_path) {
  std::error_code ignored;
  std::filesystem::remove(t2_path, ignored);
  vmemkv::remove_wal_segments(vmemkv::derive_wal_path(t2_path));
  std::filesystem::remove(vmemkv::derive_manifest_path(t2_path), ignored);
  std::filesystem::remove(vmemkv::derive_t1_chk_path(t2_path), ignored);
  std::filesystem::remove(vmemkv::derive_t2_chk_path(t2_path), ignored);
}

// RAII guard around a path from reserve_unique_temp_path(): implicitly converts to the reserved
// path (so it drops into a call expecting a std::filesystem::path unchanged) and runs `cleanup`
// on it both immediately (covering a stale leftover from a previous run, like
// reserve_unique_temp_path()'s own `also_remove_wal_sibling`) and again on destruction --
// including when a REQUIRE failure unwinds out of the test case, which an end-of-test cleanup
// call would otherwise miss.
class ScopedTempPath {
 public:
  ScopedTempPath(std::string_view prefix, std::function<void(const std::filesystem::path &)> cleanup)
      : path_(reserve_unique_temp_path(prefix)), cleanup_(std::move(cleanup)) {
    cleanup_(path_);
  }
  ~ScopedTempPath() { cleanup_(path_); }

  ScopedTempPath(const ScopedTempPath &) = delete;
  auto operator=(const ScopedTempPath &) -> ScopedTempPath & = delete;

  operator const std::filesystem::path &() const noexcept { return path_; }
  [[nodiscard]] auto get() const noexcept -> const std::filesystem::path & { return path_; }

 private:
  std::filesystem::path path_;
  std::function<void(const std::filesystem::path &)> cleanup_;
};

inline void remove_plain_file(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

inline auto bytes_of(std::string_view value) -> std::vector<std::byte> {
  std::vector<std::byte> out(value.size());
  std::memcpy(out.data(), value.data(), value.size());
  return out;
}

inline auto as_span(const std::vector<std::byte> &value) -> std::span<const std::byte> {
  return std::span<const std::byte>(value.data(), value.size());
}

inline auto span_to_string(std::span<const std::byte> value) -> std::string {
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

// Shared core of test_kv_store.cpp's get_bytes_sync() and test_crash_recovery.cpp's get_bytes():
// both do the same store->get()-with-callback dance and only differ in the collection type they
// hand back, so each is a thin wrapper around this.
template <typename StorePtr, typename Key>
auto get_optional_bytes(const StorePtr &store, const Key &key) -> std::optional<std::vector<std::byte>> {
  std::optional<std::vector<std::byte>> result;
  const bool found =
      store->get(key, [&](std::span<const std::byte> val) { result = std::vector<std::byte>(val.begin(), val.end()); });
  if (found) {
    return result;
  }
  return std::nullopt;
}

}  // namespace vmemkv_test
