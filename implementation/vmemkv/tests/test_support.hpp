// test_support.hpp - Shared helpers for the test_kv_store binary's constituent .cpp files.
#pragma once

#include <doctest/doctest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <checkpoint/checkpoint.hpp>
#include <core/env.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vmemkv/config.hpp>
#include <wal/wal.hpp>

namespace vmemkv_test {

// Config override providing a small T1 append-region capacity (2^Log2 entries).
template <size_t Log2>
struct TinyAppendConfig : vmemkv::Config<> {
  static constexpr size_t T1AppendCapacityLog2 = Log2;
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << Log2;
};

inline auto to_span(const std::string &text) -> std::span<const std::byte> {
  return std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size());
}

// Adapts a per-entry function to T1Index::reorganize()'s OffsetMapper contract.
template <typename PerEntryFn>
auto per_entry_offset_mapper(PerEntryFn fn) {
  return [fn = std::move(fn)](auto merged) {
    for (auto &entry : merged) {
      entry.payload_bits = fn(entry.payload_bits, entry.hash);
    }
  };
}

// Base directory for test-created T2/WAL/checkpoint files.
inline auto test_temp_root() -> const std::filesystem::path & {
  static const std::filesystem::path root = [] {
    if (const std::string_view env = vmemkv::getenv_view("VMEMKV_TEST_TMPDIR"); !env.empty()) {
      return std::filesystem::path(std::string(env));
    }
    return std::filesystem::temp_directory_path();
  }();
  return root;
}

// Builds a unique-per-process-per-call temp file path under `prefix`.
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

// Removes every file a VMemKVImpl store could have created at `t2_path`.
inline void remove_store_files(const std::filesystem::path &t2_path) {
  vmemkv::remove_quiet(t2_path);
  vmemkv::remove_wal_segments(vmemkv::derive_wal_path(t2_path));
  vmemkv::remove_quiet(vmemkv::derive_manifest_path(t2_path));
  vmemkv::remove_quiet(vmemkv::derive_t1_chk_path(t2_path));
  vmemkv::remove_quiet(vmemkv::derive_t2_chk_path(t2_path));
}

// RAII guard around a path from reserve_unique_temp_path().
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

// Alias kept for existing callers; new code uses vmemkv::remove_quiet directly.
inline void remove_plain_file(const std::filesystem::path &path) { vmemkv::remove_quiet(path); }

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

// Fixed-width decimal key maker: prefix followed by the zero-padded index.
inline auto padded_key(std::string_view prefix, long long index, int width) -> std::string {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*s%0*lld", static_cast<int>(prefix.size()), prefix.data(), width, index);
  return {buf};
}

// Fixed-width decimal key maker with the default "k" prefix.
inline auto padded_key(long long index, int width) -> std::string { return padded_key("k", index, width); }

// Value maker used by crash-recovery tests: always >= 9 bytes, forcing every write through T2.
inline auto indexed_value(long long index) -> std::string { return "value_" + std::to_string(index) + "_pad"; }

// Flips a single byte at `offset` in the file at `path`.
inline void flip_byte_at(const std::filesystem::path &path, std::streamoff offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(offset);
  char original = 0;
  file.read(&original, 1);
  const char flipped = static_cast<char>(~original);
  file.seekp(offset);
  file.write(&flipped, 1);
}

// Resolves the WAL's active segment for either a WAL identity path or a T2 store path.
inline auto resolve_active_wal_segment(const std::filesystem::path &path) -> std::filesystem::path {
  if (const auto direct = vmemkv::find_active_wal_segment(path); direct.has_value()) {
    return *direct;
  }
  const auto derived = vmemkv::find_active_wal_segment(vmemkv::derive_wal_path(path));
  REQUIRE(derived.has_value());
  return *derived;
}

// Appends raw bytes shorter than a full WAL record header to the active segment.
inline void append_torn_header(const std::filesystem::path &path) {
  const auto active = resolve_active_wal_segment(path);
  std::ofstream out(active, std::ios::binary | std::ios::app);
  constexpr std::array<char, 10> garbage{};
  out.write(garbage.data(), garbage.size());
}

// Appends a full header declaring a longer payload than what actually follows.
inline void append_torn_payload(const std::filesystem::path &path) {
  vmemkv::WalRecordHeader header{};
  header.lsn = 0;
  header.checksum = 0;
  header.magic = vmemkv::kWalRecordMagic;
  header.key_len = 5;
  header.value_len = 5;
  header.type = static_cast<uint8_t>(vmemkv::WalRecordType::Insert);

  const auto active = resolve_active_wal_segment(path);
  std::ofstream out(active, std::ios::binary | std::ios::app);
  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  constexpr std::array<char, 3> partial_payload{'x', 'y', 'z'};
  out.write(partial_payload.data(), partial_payload.size());
}

}  // namespace vmemkv_test
