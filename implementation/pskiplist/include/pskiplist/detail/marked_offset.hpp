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

}  // namespace pskiplist
