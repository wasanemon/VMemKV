#pragma once

#include <concepts>
#include <type_traits>

namespace pskiplist {

// Key is stored as raw bytes directly in the mmap'd file (2.1節) and is never
// placement-new constructed slot-by-slot — only trivially copyable types can be safely
// read from and overwritten onto memory that was never explicitly constructed.
template <typename Key>
concept SkipListKey = std::is_trivially_copyable_v<Key> && std::default_initializable<Key>;

template <typename Compare, typename Key>
concept SkipListCompare = std::default_initializable<Compare> && std::predicate<Compare, const Key &, const Key &>;

}  // namespace pskiplist
