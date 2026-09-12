// rival_common.hpp — Shared helpers for the rival (non-VMemKV) backend wrappers.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vmemkv::rivals {

// Byte-wise key ordering: memcmp over the shared prefix, shorter key first on ties.
inline auto compare_bytes(std::span<const std::byte> a, std::span<const std::byte> b) noexcept -> int {
  const std::size_t min_len = std::min(a.size(), b.size());
  const int cmp = min_len == 0 ? 0 : std::memcmp(a.data(), b.data(), min_len);
  if (cmp != 0) {
    return cmp;
  }
  if (a.size() == b.size()) {
    return 0;
  }
  return a.size() < b.size() ? -1 : 1;
}

// Unique per-instance path: appends "_<n><ext>" with a process-wide counter.
inline auto make_unique_instance_path(std::string base, std::string_view ext) -> std::string {
  static std::atomic<uint64_t> instance_counter{0};
  base += "_";
  base += std::to_string(instance_counter.fetch_add(1, std::memory_order_relaxed));
  base.append(ext.data(), ext.size());
  return base;
}

// Sibling path holding a master under construction.
inline auto building_path(const std::string &master) -> std::string { return master + ".building"; }

// Renames src onto dst, throwing on failure.
inline void atomic_rename(const std::string &src, const std::string &dst, const std::string &failure_prefix) {
  std::error_code ec;
  std::filesystem::rename(src, dst, ec);
  if (ec) {
    throw std::runtime_error(failure_prefix + ": " + ec.message());
  }
}

}  // namespace vmemkv::rivals
