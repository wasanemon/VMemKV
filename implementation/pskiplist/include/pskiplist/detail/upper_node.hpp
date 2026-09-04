#pragma once

#include <atomic>
#include <cstddef>
#include <new>

#include "pskiplist/detail/marked_offset.hpp"
#include "pskiplist/detail/marked_pointer.hpp"

namespace pskiplist {

// A single volatile, DRAM-only skip-list node participating in levels 1..height (2.4節).
// Its forward pointers for all `height` levels are allocated as one contiguous block
// following the header rather than as `height` separate allocations (8章), so a search
// descending through this node's levels stays in one cache-line neighborhood. Only ever
// constructed via allocate_upper_node() below, which owns the combined allocation.
//
// Each `forwards[level]` packs (successor pointer, mark bit) into one atomic<uint64_t> via
// marked_pointer.hpp — the same idea as Level 0's forward0 (marked_offset.hpp), letting a
// single CAS on a predecessor's word validate both "the successor is still what I read" and
// "the predecessor itself hasn't been marked out from under me" (see marked_list.hpp).
//
// `levels_remaining` counts down from `height` as each level's splice completes (marked_list.hpp's
// on_splice callback); whoever brings it to 0 knows this node is gone from every list it
// participated in and is the one who enqueues it for EBR reclaim (reclaim() in skiplist.hpp).
//
// `next_pending` is a dedicated field for that reclaim queue rather than reusing forwards[0]:
// keeping it separate avoids coupling reclaim-queue linkage to level 1 specifically being the
// last level spliced, which the levels_remaining counter alone doesn't guarantee ordering-wise.
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
