#pragma once

#include <atomic>
#include <cstdint>

#include "pskiplist/detail/marked_offset.hpp"
#include "pskiplist/detail/tombstone.hpp"

namespace pskiplist {

template <typename Key, typename Value = uint64_t>
struct DurableNode {
  std::atomic<uint64_t> epoch{0};  // creation epoch, set once at allocation
  Key key{};
  std::atomic<NodeState> state{NodeState::kLive};
  // Shadow of state as of the last checkpoint (high_level_design.md 4.1).
  std::atomic<NodeState> checkpointed_state{NodeState::kLive};
  // Seqlock guarding `value`: even = stable, odd = write in flight (also the write-side mutex).
  mutable std::atomic<uint64_t> version{0};
  Value value{};
  Value checkpointed_value{};
  std::atomic<uint64_t> mutation_epoch{0};
  mutable std::atomic<uint64_t> forward0{pack_forward(kNullOffset, false)};
  std::atomic<Offset> next_checkpoint_unlink{kNullOffset};  // checkpoint-gated unlink queue link
  std::atomic<bool> pending_checkpoint_unlink_queued{false};
};

}  // namespace pskiplist
