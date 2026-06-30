// utils.hpp - Internal mathematical and layout helpers for VMemKV.
#pragma once

#include <cstdint>

namespace vmemkv {

struct EmptyOption {};
static_assert(sizeof(EmptyOption) <= 1, "EmptyOption must be a zero-sized or minimal size empty helper");

inline constexpr uint64_t align_up(uint64_t size, uint64_t alignment = 8) noexcept
{
    return (size + alignment - 1) & ~(alignment - 1);
}

inline constexpr uint64_t align_down(uint64_t size, uint64_t alignment) noexcept
{
    return size & ~(alignment - 1);
}

} // namespace vmemkv
