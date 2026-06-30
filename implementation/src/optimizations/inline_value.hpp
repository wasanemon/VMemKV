// inline_value.hpp -- Core utilities for dynamic payload inlining.
#pragma once

#include <span>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>

namespace vmemkv::inline_value {

inline uint64_t pack(std::span<const std::byte> val) noexcept
{
    uint64_t val_u64 = 0;
    std::memcpy(&val_u64, val.data(), val.size());
    return val_u64;
}

inline std::vector<std::byte> unpack(uint64_t payload, size_t size) noexcept
{
    std::vector<std::byte> out(size);
    std::memcpy(out.data(), &payload, size);
    return out;
}

} // namespace vmemkv::inline_value
