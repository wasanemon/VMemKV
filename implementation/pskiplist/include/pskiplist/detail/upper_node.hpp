#pragma once

#include <atomic>
#include <cstddef>
#include <new>

#include "pskiplist/detail/marked_offset.hpp"
#include "pskiplist/detail/marked_pointer.hpp"

namespace pskiplist {

// A volatile, DRAM-only skip-list node participating in levels 1..height. `forwards[level]`
// packs (successor pointer, mark bit) the same way Level 0's forward0 does (marked_pointer.hpp
// / marked_offset.hpp); all `height` entries are one contiguous allocation following the
// header, only ever built via allocate_upper_node() below. `levels_remaining` counts down as
// each level's splice completes; whoever brings it to 0 enqueues this node for EBR reclaim via
// the dedicated `next_pending` link (kept separate from forwards[0] since level 1 isn't
// guaranteed to be the last level spliced).
struct UpperNode {
  Offset durable_offset;
  int height;
  std::atomic<int> levels_remaining{0};
  std::atomic<UpperNode *> next_pending{nullptr};
  std::atomic<uint64_t> forwards[1];  // actually `height` entries — see allocate_upper_node()
};

[[nodiscard]] inline auto allocate_upper_node(Offset durable_offset, int height) -> UpperNode * {
  void *memory = ::operator new(sizeof(UpperNode) + sizeof(std::atomic<uint64_t>) * static_cast<size_t>(height - 1));
  auto *node = static_cast<UpperNode *>(memory);
  ::new (&node->durable_offset) Offset(durable_offset);
  ::new (&node->height) int(height);
  ::new (&node->levels_remaining) std::atomic<int>(height);
  ::new (&node->next_pending) std::atomic<UpperNode *>(nullptr);
  for (int level = 0; level < height; ++level) {
    ::new (&node->forwards[level]) std::atomic<uint64_t>(pack_marked_ptr<UpperNode>(nullptr, false));
  }
  return node;
}

inline void deallocate_upper_node(UpperNode *node) {
  node->next_pending.~atomic();
  node->levels_remaining.~atomic();
  for (int level = 0; level < node->height; ++level) {
    node->forwards[level].~atomic();
  }
  ::operator delete(node);
}

}  // namespace pskiplist
