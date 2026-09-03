#pragma once

#include <concepts>

namespace pskiplist {

template <typename Key>
concept SkipListKey = std::copyable<Key> && std::default_initializable<Key>;

template <typename Compare, typename Key>
concept SkipListCompare =
    std::default_initializable<Compare> && std::predicate<Compare, const Key &, const Key &>;

}  // namespace pskiplist
