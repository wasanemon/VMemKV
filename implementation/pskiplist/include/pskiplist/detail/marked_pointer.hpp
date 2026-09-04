#pragma once

#include <cstdint>

namespace pskiplist {

// Marks a pointer's low bit to signal logical deletion (marked_offset.hpp's kForwardMarkBit
// idea, applied to bit 0 instead of bit 63) — safe since `::operator new` addresses are
// always aligned well beyond 2 bytes.
template <typename T>
[[nodiscard]] inline auto pack_marked_ptr(T *ptr, bool marked) -> uintptr_t {
  const auto value = reinterpret_cast<uintptr_t>(ptr);
  return marked ? (value | uintptr_t{1}) : value;
}

template <typename T>
[[nodiscard]] inline auto marked_ptr_value(uintptr_t raw) -> T * {
  return reinterpret_cast<T *>(raw & ~uintptr_t{1});
}

[[nodiscard]] inline auto marked_ptr_marked(uintptr_t raw) -> bool { return (raw & uintptr_t{1}) != 0; }

// marked_list.hpp traits binding it to pointer identities (null_id() is nullptr, unlike
// Level 0's offset-based kTail sentinel).
template <typename T>
struct PointerMarkedTraits {
  static auto pack(T *id, bool marked) -> uint64_t { return pack_marked_ptr<T>(id, marked); }
  static auto value(uint64_t raw) -> T * { return marked_ptr_value<T>(raw); }
  static auto is_marked(uint64_t raw) -> bool { return marked_ptr_marked(raw); }
  static auto null_id() -> T * { return nullptr; }
};

}  // namespace pskiplist
