// bytes.hpp - Shared byte-span ordering and comparison helpers.
#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstring>
#include <span>

namespace vmemkv {

// Byte-wise key ordering: memcmp over the shared prefix, shorter key first on ties.
inline auto bytes_compare_3way(std::span<const std::byte> a, std::span<const std::byte> b) noexcept -> int {
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

inline auto bytes_equal(std::span<const std::byte> a, std::span<const std::byte> b) noexcept -> bool {
  return a.size() == b.size() && bytes_compare_3way(a, b) == 0;
}

inline auto bytes_in_range(std::span<const std::byte> key,
                           std::span<const std::byte> lo,
                           std::span<const std::byte> hi) noexcept -> bool {
  return bytes_compare_3way(key, lo) >= 0 && bytes_compare_3way(key, hi) <= 0;
}

}  // namespace vmemkv
