#pragma once

#include <cstdint>

namespace pskiplist {

using Offset = uint64_t;
inline constexpr uint64_t kForwardMarkBit = uint64_t{1} << 63;
inline constexpr Offset kNullOffset = kForwardMarkBit - 1;

[[nodiscard]] inline constexpr auto pack_forward(Offset offset, bool marked) -> uint64_t {
  return offset | (marked ? kForwardMarkBit : uint64_t{0});
}
[[nodiscard]] inline constexpr auto forward_offset(uint64_t raw) -> Offset { return raw & ~kForwardMarkBit; }
[[nodiscard]] inline constexpr auto forward_marked(uint64_t raw) -> bool { return (raw & kForwardMarkBit) != 0; }

// A tagged (offset, generation counter) word for the free list's Treiber stack head: the
// counter changes on every push/pop, ruling out ABA for a thread that stalls mid-pop and
// later CASes against a stale head that coincidentally names the same offset again.
inline constexpr int kTaggedOffsetBits = 40;
inline constexpr uint64_t kTaggedOffsetMask = (uint64_t{1} << kTaggedOffsetBits) - 1;
inline constexpr Offset kMaxTaggedOffset = kTaggedOffsetMask;

inline constexpr uint32_t kTaggedGenerationMask = (uint32_t{1} << (64 - kTaggedOffsetBits)) - 1;

[[nodiscard]] inline constexpr auto pack_tagged(Offset offset, uint32_t tag) -> uint64_t {
  return (offset & kTaggedOffsetMask) | (static_cast<uint64_t>(tag & kTaggedGenerationMask) << kTaggedOffsetBits);
}
[[nodiscard]] inline constexpr auto tagged_offset(uint64_t raw) -> Offset { return raw & kTaggedOffsetMask; }
[[nodiscard]] inline constexpr auto tagged_generation(uint64_t raw) -> uint32_t {
  return static_cast<uint32_t>(raw >> kTaggedOffsetBits);
}

}  // namespace pskiplist
