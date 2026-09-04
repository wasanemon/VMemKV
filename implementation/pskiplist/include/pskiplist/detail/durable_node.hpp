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
  // Shadow of `state`/`value` as of the last checkpoint, and the epoch of the most recent
  // mutation — see skiplist.hpp's write_value_locked() / recover(). checkpointed_state exists
  // because reverting `value` alone during recover() no longer implies reverting state too, now
  // that they're separate fields instead of one packed word.
  std::atomic<NodeState> checkpointed_state{NodeState::kLive};
  // Seqlock guarding `value`: even = stable, odd = a write is in flight. Also doubles as the
  // write-side mutual exclusion (CAS even->odd) for concurrent put()s targeting this node — see
  // skiplist.hpp's write_value_locked()/read_value().
  mutable std::atomic<uint64_t> version{0};
  Value value{};
  Value checkpointed_value{};
  std::atomic<uint64_t> mutation_epoch{0};
  mutable std::atomic<uint64_t> forward0{pack_forward(kNullOffset, false)};
  // Checkpoint-gated physical-unlink queue linkage — see enqueue_pending_checkpoint_unlink().
  std::atomic<Offset> next_checkpoint_unlink{kNullOffset};
  std::atomic<bool> pending_checkpoint_unlink_queued{false};
};

}  // namespace pskiplist
