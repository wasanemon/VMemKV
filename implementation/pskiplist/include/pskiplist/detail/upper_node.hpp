#pragma once

#include <atomic>
#include <cstddef>
#include <new>

#include "pskiplist/detail/marked_offset.hpp"

namespace pskiplist {

// A single volatile, DRAM-only skip-list node participating in levels 1..height (2.4節).
// Its forward pointers for all `height` levels are allocated as one contiguous block
// following the header rather than as `height` separate allocations (8章), so a search
// descending through this node's levels stays in one cache-line neighborhood. Only ever
// constructed via allocate_upper_node() below, which owns the combined allocation.
struct UpperNode {
  Offset durable_offset;
  int height;
  std::atomic<UpperNode *> forwards[1];  // actually `height` entries — see allocate_upper_node()
};

[[nodiscard]] inline auto allocate_upper_node(Offset durable_offset, int height) -> UpperNode * {
  void *memory =
      ::operator new(sizeof(UpperNode) + sizeof(std::atomic<UpperNode *>) * static_cast<size_t>(height - 1));
  auto *node = static_cast<UpperNode *>(memory);
  ::new (&node->durable_offset) Offset(durable_offset);
  ::new (&node->height) int(height);
  for (int level = 0; level < height; ++level) {
    ::new (&node->forwards[level]) std::atomic<UpperNode *>(nullptr);
  }
  return node;
}

inline void deallocate_upper_node(UpperNode *node) {
  for (int level = 0; level < node->height; ++level) {
    node->forwards[level].~atomic();
  }
  ::operator delete(node);
}

}  // namespace pskiplist
