#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace t1_detail {

template <typename Slot, typename Key, typename Callback, typename IsLive>
inline auto scan_append_scalar(const Slot *append_region,
                               size_t append_size,
                               Key lower_bound,
                               Key upper_bound,
                               Callback &callback,
                               IsLive is_live) -> size_t {
  size_t count = 0;
  for (size_t index = 0; index < append_size; ++index) {
    const Slot &slot = append_region[index];
    if (!slot.published.load(std::memory_order_acquire)) {
      continue;
    }
    const uint64_t value = slot.payload_bits.load(std::memory_order_acquire);
    if (!is_live(value) || slot.key < lower_bound || upper_bound < slot.key) {
      continue;
    }
    callback(slot.key, value);
    ++count;
  }
  return count;
}

#if defined(__x86_64__) || defined(_M_X64)
template <typename Slot, typename Key, typename Callback, typename IsLive>
inline auto scan_append_simd(const Slot *append_region,
                             size_t append_size,
                             Key lower_bound,
                             Key upper_bound,
                             Callback &callback,
                             IsLive is_live) -> size_t {
  size_t count = 0;
  const uint64_t lower_bound_first = lower_bound[0];
  const uint64_t upper_bound_first = upper_bound[0];
  size_t index = 0;

  for (; index + 4 <= append_size; index += 4) {
    __m256i keys = _mm256_set_epi64x(static_cast<long long>(append_region[index + 3].key[0]),
                                     static_cast<long long>(append_region[index + 2].key[0]),
                                     static_cast<long long>(append_region[index + 1].key[0]),
                                     static_cast<long long>(append_region[index + 0].key[0]));
    __m256i lower_bound_vec = _mm256_set1_epi64x(static_cast<long long>(lower_bound_first));
    __m256i upper_bound_vec = _mm256_set1_epi64x(static_cast<long long>(upper_bound_first));
    __m256i flip = _mm256_set1_epi64x(std::numeric_limits<long long>::min());

    __m256i k_bias = _mm256_xor_si256(keys, flip);
    __m256i lower_bound_bias = _mm256_xor_si256(lower_bound_vec, flip);
    __m256i upper_bound_bias = _mm256_xor_si256(upper_bound_vec, flip);
    __m256i ge_lower_bound =
        _mm256_or_si256(_mm256_cmpgt_epi64(k_bias, lower_bound_bias), _mm256_cmpeq_epi64(k_bias, lower_bound_bias));
    __m256i le_upper_bound =
        _mm256_or_si256(_mm256_cmpgt_epi64(upper_bound_bias, k_bias), _mm256_cmpeq_epi64(k_bias, upper_bound_bias));

    int mask = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_and_si256(ge_lower_bound, le_upper_bound)));
    while (mask != 0) {
      int lane = std::countr_zero(static_cast<unsigned>(mask));
      const Slot &slot = append_region[index + static_cast<size_t>(lane)];
      if (!slot.published.load(std::memory_order_acquire)) {
        mask &= mask - 1;
        continue;
      }
      const uint64_t value = slot.payload_bits.load(std::memory_order_acquire);
      if (is_live(value) && !(slot.key < lower_bound) && !(upper_bound < slot.key)) {
        callback(slot.key, value);
        ++count;
      }
      mask &= mask - 1;
    }
  }

  return count +
         scan_append_scalar(append_region + index, append_size - index, lower_bound, upper_bound, callback, is_live);
}
#endif

template <bool UseSimdScan, typename Slot, typename Key, typename Callback, typename IsLive>
inline auto scan_append(const Slot *append_region,
                        size_t append_size,
                        Key lower_bound,
                        Key upper_bound,
                        Callback &callback,
                        IsLive is_live) -> size_t {
  if constexpr (UseSimdScan && std::is_same_v<typename Key::value_type, uint64_t>) {
#if defined(__x86_64__) || defined(_M_X64)
    return scan_append_simd(append_region, append_size, lower_bound, upper_bound, callback, is_live);
#else
    return scan_append_scalar(append_region, append_size, lower_bound, upper_bound, callback, is_live);
#endif
  } else {
    return scan_append_scalar(append_region, append_size, lower_bound, upper_bound, callback, is_live);
  }
}

}  // namespace t1_detail
