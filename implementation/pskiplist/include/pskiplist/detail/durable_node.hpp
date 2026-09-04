#pragma once

#include <atomic>
#include <cstdint>

#include "pskiplist/detail/marked_offset.hpp"

namespace pskiplist {

template <typename Key>
struct DurableNode {
  std::atomic<uint64_t> epoch{0};  // creation epoch — never touched again after allocation
  Key key{};
  std::atomic<uint64_t> value{0};
  // Shadow of `value` as of the last checkpoint that covers it, and the epoch of the most
  // recent mutation — see skiplist.hpp's shadow_if_first_touch_since_checkpoint() and
  // recover().
  std::atomic<uint64_t> checkpointed_value{0};
  std::atomic<uint64_t> mutation_epoch{0};
  mutable std::atomic<uint64_t> forward0{pack_forward(kNullOffset, false)};
  // Linkage and dedup guard for the checkpoint-gated physical-unlink queue — see
  // enqueue_pending_checkpoint_unlink() in skiplist.hpp.
  std::atomic<Offset> next_checkpoint_unlink{kNullOffset};
  std::atomic<bool> pending_checkpoint_unlink_queued{false};
};

}  // namespace pskiplist
