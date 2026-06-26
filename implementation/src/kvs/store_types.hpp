// store_types.hpp — Common key/value types for all store implementations.
//
// Keys : up to 16 bytes, stored as std::array<uint64_t,2> in big-endian so
//        that std::array's lexicographic < preserves string lexicographic order.
// Values: uint64_t; STORE_NOT_FOUND (= UINT64_MAX) signals key-absent.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

using StoreKey = std::array<uint64_t, 2>;
static constexpr uint64_t STORE_NOT_FOUND = UINT64_MAX;

inline StoreKey make_store_key(std::string_view sv) noexcept
{
    // Build a uint64_t from up to 8 bytes in big-endian order so that
    // std::array<uint64_t,2>::operator< preserves string lex order.
    // Guard: n==0 returns 0 without shifting (v << 64 is UB in C++).
    auto u64be = [](const char *src, size_t n) noexcept -> uint64_t
    {
        size_t m = std::min(n, size_t{8});
        if (m == 0)
            return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < m; ++i)
            v = (v << 8) | static_cast<unsigned char>(src[i]);
        return v << ((8 - m) * 8);
    };
    size_t s = sv.size();
    return {u64be(sv.data(), s),
            u64be(sv.data() + std::min(s, size_t{8}), s > 8 ? s - 8 : 0)};
}
