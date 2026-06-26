#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#if __has_include(<sys/mman.h>)
#include <sys/mman.h>
#include <unistd.h>
#define VMEMKV_HAS_MMAP_HINTS 1
#else
#define VMEMKV_HAS_MMAP_HINTS 0
#endif

namespace t1_detail {

#if VMEMKV_HAS_MMAP_HINTS
inline std::pair<void *, size_t> page_span(const void *ptr, size_t bytes)
{
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return {nullptr, 0};
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t page = static_cast<uintptr_t>(page_size);
    const uintptr_t aligned_start = start & ~(page - 1);
    const uintptr_t end = start + bytes;
    const uintptr_t aligned_end = (end + page - 1) & ~(page - 1);
    if (aligned_end <= aligned_start)
        return {nullptr, 0};
    return {reinterpret_cast<void *>(aligned_start), aligned_end - aligned_start};
}
#endif

inline void apply_region_hints(const void *ptr, size_t bytes)
{
#if VMEMKV_HAS_MMAP_HINTS
    if (ptr == nullptr || bytes == 0)
        return;
    auto [base, len] = page_span(ptr, bytes);
    if (base == nullptr || len == 0)
        return;
    (void)mlock(base, len);
#ifdef MADV_HUGEPAGE
    (void)madvise(base, len, MADV_HUGEPAGE);
#endif
#else
    (void)ptr;
    (void)bytes;
#endif
}

inline void set_sequential_hint(const void *ptr, size_t bytes)
{
#if VMEMKV_HAS_MMAP_HINTS && defined(MADV_SEQUENTIAL)
    if (ptr == nullptr || bytes == 0)
        return;
    auto [base, len] = page_span(ptr, bytes);
    if (base == nullptr || len == 0)
        return;
    (void)madvise(base, len, MADV_SEQUENTIAL);
#else
    (void)ptr;
    (void)bytes;
#endif
}

} // namespace t1_detail
