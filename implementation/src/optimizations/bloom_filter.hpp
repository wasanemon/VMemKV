#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace t1_detail {

class BloomFilter
{
public:
    explicit BloomFilter(size_t entry_count = 0)
    {
        reset(entry_count);
    }

    void reset(size_t entry_count)
    {
        if (entry_count == 0)
        {
            words_.clear();
            mask_bits_ = 0;
            return;
        }
        const size_t bits = std::bit_ceil(std::max<size_t>(64, entry_count * 8));
        words_.assign(bits / 64, 0);
        mask_bits_ = bits - 1;
    }

    void add(uint64_t hash) noexcept
    {
        if (words_.empty())
            return;
        set(bit_index(hash));
        set(bit_index(mix(hash)));
        set(bit_index(mix(mix(hash))));
    }

    bool maybe_contains(uint64_t hash) const noexcept
    {
        if (words_.empty())
            return true;
        return test(bit_index(hash)) &&
               test(bit_index(mix(hash))) &&
               test(bit_index(mix(mix(hash))));
    }

private:
    static uint64_t mix(uint64_t x) noexcept
    {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }

    size_t bit_index(uint64_t hash) const noexcept
    {
        return static_cast<size_t>(hash) & mask_bits_;
    }

    void set(size_t index) noexcept
    {
        words_[index / 64] |= (uint64_t{1} << (index % 64));
    }

    bool test(size_t index) const noexcept
    {
        return (words_[index / 64] & (uint64_t{1} << (index % 64))) != 0;
    }

    std::vector<uint64_t> words_;
    size_t mask_bits_ = 0;
};

} // namespace t1_detail
