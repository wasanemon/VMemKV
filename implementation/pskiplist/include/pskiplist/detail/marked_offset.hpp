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

// A tagged (offset, generation counter) pair packed into one word, for the free list's
// Treiber stack head: the counter changes on every push and pop so a thread that stalls
// mid-pop with a stale head value can never CAS it back in undetected, even if the exact
// same offset has since been popped and pushed again (the classic ABA hazard for a plain
// offset-only stack head). 24 bits of counter is generous — wrapping it back to a value
// a stalled thread could still match would need that many free-list push/pop cycles to
// land on the same offset again during a single stall, which isn't realistic. 40 bits of
// offset is equally generous for node counts.
inline constexpr int kTaggedOffsetBits = 40;
inline constexpr uint64_t kTaggedOffsetMask = (uint64_t{1} << kTaggedOffsetBits) - 1;
inline constexpr Offset kMaxTaggedOffset = kTaggedOffsetMask;

[[nodiscard]] inline constexpr auto pack_tagged(Offset offset, uint32_t tag) -> uint64_t {
  return (offset & kTaggedOffsetMask) | (static_cast<uint64_t>(tag) << kTaggedOffsetBits);
}
[[nodiscard]] inline constexpr auto tagged_offset(uint64_t raw) -> Offset { return raw & kTaggedOffsetMask; }
[[nodiscard]] inline constexpr auto tagged_generation(uint64_t raw) -> uint32_t {
  return static_cast<uint32_t>(raw >> kTaggedOffsetBits);
}

}  // namespace pskiplist
