// utils.hpp - Internal mathematical and layout helpers for VMemKV.
#pragma once

#include <cstdint>

namespace vmemkv {

inline constexpr uint64_t kDefaultAlignmentBytes = 8;

struct EmptyOption {};
static_assert(sizeof(EmptyOption) <= 1, "EmptyOption must be a zero-sized or minimal size empty helper");

constexpr auto align_up(uint64_t size, uint64_t alignment = kDefaultAlignmentBytes) noexcept -> uint64_t {
  return (size + alignment - 1) & ~(alignment - 1);
}

constexpr auto align_down(uint64_t size, uint64_t alignment) noexcept -> uint64_t { return size & ~(alignment - 1); }

}  // namespace vmemkv
