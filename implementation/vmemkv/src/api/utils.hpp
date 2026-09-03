// utils.hpp - Internal mathematical and layout helpers for VMemKV.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vmemkv {

inline constexpr uint64_t kDefaultAlignmentBytes = 8;

struct EmptyOption {};
static_assert(sizeof(EmptyOption) <= 1, "EmptyOption must be a zero-sized or minimal size empty helper");

constexpr auto align_up(uint64_t size, uint64_t alignment = kDefaultAlignmentBytes) noexcept -> uint64_t {
  return (size + alignment - 1) & ~(alignment - 1);
}

// FNV-1a64: shared by every on-disk format (WAL, checkpoint) that needs a fast, non-cryptographic
// checksum for detecting torn writes / bit rot. Not for security use.
inline constexpr uint64_t kFnvOffsetBasis64 = 14695981039346656037ULL;
inline constexpr uint64_t kFnvPrime64 = 1099511628211ULL;

inline auto fnv1a64_update(uint64_t hash, const void *data, size_t size) noexcept -> uint64_t {
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime64;
  }
  return hash;
}

// Every self-describing on-disk header (WalRecordHeader, T1ChkFileHeader, ManifestHeader) has a
// `checksum` field covering the rest of the header plus payload, computed with that field
// treated as zero. Taking `header` by value lets writer and reader share this one call: a
// writer's checksum is already zero-initialized, and a reader's real value is discarded by the
// copy before hashing. Duck-typed on a `.checksum` member -- no dependency between formats.
template <typename Header>
inline auto checksum_header(Header header) noexcept -> uint64_t {
  header.checksum = 0;
  return fnv1a64_update(kFnvOffsetBasis64, &header, sizeof(header));
}

}  // namespace vmemkv
