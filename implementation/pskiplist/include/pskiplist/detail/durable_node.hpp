#pragma once

#include <atomic>
#include <cstdint>

#include "pskiplist/detail/marked_offset.hpp"

namespace pskiplist {

template <typename Key>
struct DurableNode {
  std::atomic<uint64_t> epoch{0};
  Key key{};
  std::atomic<uint64_t> value{0};
  mutable std::atomic<uint64_t> forward0{pack_forward(kNullOffset, false)};
};

}  // namespace pskiplist
