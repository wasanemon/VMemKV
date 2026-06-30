// inline_value.hpp -- Core utilities for dynamic payload inlining.
#pragma once

#include <span>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <cstring>

namespace vmemkv::inline_value {

template <bool UseInlineShort, bool UseInline8B>
inline bool is_inline_payload(uint64_t payload) noexcept
{
    if (payload == ~0ULL)
        return false;

    if constexpr (!UseInlineShort && !UseInline8B)
    {
        return false;
    }
    else
    {
        return (payload & 1ULL) != 0;
    }
}

inline uint64_t pack(std::span<const std::byte> val) noexcept
{
    uint64_t val_u64 = 0;
    std::memcpy(&val_u64, val.data(), std::min<size_t>(val.size(), 8));

    if (val.size() >= 1 && val.size() <= 7)
    {
        val_u64 = (val_u64 << 1) | 1ULL;
        val_u64 |= (static_cast<uint64_t>(val.size()) << 60);
    }
    return val_u64;
}

inline std::vector<std::byte> unpack(uint64_t payload) noexcept
{
    size_t len = static_cast<size_t>((payload >> 60) & 7ULL);
    uint64_t val_u64 = 0;
    if (len >= 1 && len <= 7)
    {
        val_u64 = (payload & ~(7ULL << 60)) >> 1;
    }
    else
    {
        len = 8;
        val_u64 = payload;
    }

    std::vector<std::byte> out(len);
    std::memcpy(out.data(), &val_u64, len);
    return out;
}

} // namespace vmemkv::inline_value
