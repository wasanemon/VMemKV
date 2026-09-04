#pragma once

#include <cstdint>

namespace pskiplist {

// Marks the low bit of a pointer to signal logical deletion — the same idea as
// marked_offset.hpp's kForwardMarkBit for Offset, just applied to a pointer's bit 0 instead
// of Offset's bit 63. Safe because every allocator this project uses (`::operator new`)
// returns addresses aligned well beyond 2 bytes, so bit 0 of a live pointer is always 0.
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

// Traits for marked_list.hpp's generic algorithms, binding them to pointer identities.
// null_id() is nullptr — a pointer-based list's natural "no next" sentinel, unlike Level 0's
// offset-based list where that role is played by a real node (kTail).
template <typename T>
struct PointerMarkedTraits {
  static auto pack(T *id, bool marked) -> uint64_t { return pack_marked_ptr<T>(id, marked); }
  static auto value(uint64_t raw) -> T * { return marked_ptr_value<T>(raw); }
  static auto is_marked(uint64_t raw) -> bool { return marked_ptr_marked(raw); }
  static auto null_id() -> T * { return nullptr; }
};

}  // namespace pskiplist
