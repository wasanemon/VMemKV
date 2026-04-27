#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace t1_detail {

template <typename Slot, typename Key, typename Callback, typename IsLive>
inline size_t scan_append_scalar(const Slot *append_region,
                                 size_t append_size,
                                 Key lo,
                                 Key hi,
                                 Callback &cb,
                                 IsLive is_live)
{
    size_t count = 0;
    for (size_t i = 0; i < append_size; ++i)
    {
        const Slot &slot = append_region[i];
        if (!slot.published.load(std::memory_order_acquire))
            continue;
        const uint64_t value = slot.value.load(std::memory_order_acquire);
        if (!is_live(value) || slot.key < lo || hi < slot.key)
            continue;
        cb(slot.key, value);
        ++count;
    }
    return count;
}

#if defined(__x86_64__) || defined(_M_X64)
template <typename Slot, typename Key, typename Callback, typename IsLive>
inline size_t scan_append_simd(const Slot *append_region,
                               size_t append_size,
                               Key lo,
                               Key hi,
                               Callback &cb,
                               IsLive is_live)
{
    size_t count = 0;
    const uint64_t lo0 = lo[0];
    const uint64_t hi0 = hi[0];
    size_t i = 0;

    for (; i + 4 <= append_size; i += 4)
    {
        __m256i keys = _mm256_set_epi64x(
            static_cast<long long>(append_region[i + 3].key[0]),
            static_cast<long long>(append_region[i + 2].key[0]),
            static_cast<long long>(append_region[i + 1].key[0]),
            static_cast<long long>(append_region[i + 0].key[0]));
        __m256i lo_v = _mm256_set1_epi64x(static_cast<long long>(lo0));
        __m256i hi_v = _mm256_set1_epi64x(static_cast<long long>(hi0));
        __m256i flip = _mm256_set1_epi64x(std::numeric_limits<long long>::min());

        __m256i k_bias = _mm256_xor_si256(keys, flip);
        __m256i lo_bias = _mm256_xor_si256(lo_v, flip);
        __m256i hi_bias = _mm256_xor_si256(hi_v, flip);
        __m256i ge_lo = _mm256_or_si256(
            _mm256_cmpgt_epi64(k_bias, lo_bias),
            _mm256_cmpeq_epi64(k_bias, lo_bias));
        __m256i le_hi = _mm256_or_si256(
            _mm256_cmpgt_epi64(hi_bias, k_bias),
            _mm256_cmpeq_epi64(k_bias, hi_bias));

        int mask = _mm256_movemask_pd(
            _mm256_castsi256_pd(_mm256_and_si256(ge_lo, le_hi)));
        while (mask != 0)
        {
            int lane = std::countr_zero(static_cast<unsigned>(mask));
            const Slot &slot = append_region[i + static_cast<size_t>(lane)];
            if (!slot.published.load(std::memory_order_acquire))
            {
                mask &= mask - 1;
                continue;
            }
            const uint64_t value = slot.value.load(std::memory_order_acquire);
            if (is_live(value) && !(slot.key < lo) && !(hi < slot.key))
            {
                cb(slot.key, value);
                ++count;
            }
            mask &= mask - 1;
        }
    }

    return count + scan_append_scalar(append_region + i, append_size - i, lo, hi, cb, is_live);
}
#endif

template <bool UseSimdScan, typename Slot, typename Key, typename Callback, typename IsLive>
inline size_t scan_append(const Slot *append_region,
                          size_t append_size,
                          Key lo,
                          Key hi,
                          Callback &cb,
                          IsLive is_live)
{
    if constexpr (UseSimdScan)
    {
#if defined(__x86_64__) || defined(_M_X64)
        return scan_append_simd(append_region, append_size, lo, hi, cb, is_live);
#else
        return scan_append_scalar(append_region, append_size, lo, hi, cb, is_live);
#endif
    }
    else
    {
        return scan_append_scalar(append_region, append_size, lo, hi, cb, is_live);
    }
}

} // namespace t1_detail
