#pragma once

#include <cstdint>

namespace pskiplist {

// A node's lifecycle: kLive (findable, its value trustworthy), kTombstonedLinked (logically
// removed, but still linked into Level 0 — physical unlink is deferred to checkpoint()),
// kTombstonedUnlinked (physical unlink completed; the offset is retired for good and will be
// reclaimed). Kept as its own field (durable_node.hpp) rather than packed into `value`'s bits,
// so `Value` can be an arbitrary trivially-copyable type with no bits reserved for state.
enum class NodeState : uint8_t {
  kLive = 0,
  kTombstonedLinked = 1,
  kTombstonedUnlinked = 2,
};

}  // namespace pskiplist
