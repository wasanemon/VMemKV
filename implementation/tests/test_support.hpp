// test_support.hpp - Shared helpers for the test_kv_store test files.
#pragma once

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <wal/wal.hpp>

namespace vmemkv_test {

// Builds a unique-per-process-per-call temp file path under `prefix` (pid + a monotonic counter
// shared across every test file that calls this), and removes any stale file left at that path
// (and, if requested, its WAL sibling) by a previous run.
inline auto reserve_unique_temp_path(std::string_view prefix,
                                     bool also_remove_wal_sibling = false) -> std::filesystem::path {
  static std::atomic<uint64_t> counter{0};
  const uint64_t sequence_number = counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp_path = std::filesystem::temp_directory_path() /
                                    (std::string(prefix) + "_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                                     std::to_string(sequence_number));
  std::error_code ignored;
  std::filesystem::remove(temp_path, ignored);
  if (also_remove_wal_sibling) {
    std::filesystem::remove(vmemkv::derive_wal_path(temp_path), ignored);
  }
  return temp_path;
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

}  // namespace vmemkv_test
