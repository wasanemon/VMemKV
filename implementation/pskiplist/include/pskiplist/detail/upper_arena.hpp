#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <vector>

namespace pskiplist {

// Bump-pointer block arena backing UpperNode allocations (upper_node.hpp), mirroring RocksDB's
// memtable Arena (memory/arena.h). Upper levels are pure DRAM search hints, fully rebuilt from
// Level 0 on every recovery (PSkipList::rebuild_upper_levels()) -- unlike Level 0 they need no
// persistence or crash-safety story, only fast, densely-packed allocation. Individual nodes are
// never freed one at a time: instead each block tracks how many of its nodes are still live
// (`live_count`), and the block's underlying buffer is only actually freed once that count
// reaches zero *and* the block has been retired (superseded as the bump-allocation target) --
// see release()/maybe_queue_for_free(). Because rebuild_upper_levels() walks Level 0 in sorted
// key order, nodes allocated during a rebuild land in the arena in that same order, giving
// nearby keys' UpperNodes real physical locality that a plain per-node `::operator new` has no
// way to offer.
class UpperArena {
 public:
  struct Block {
    std::byte *data = nullptr;
    size_t capacity = 0;
    std::atomic<size_t> bump_offset{0};
    std::atomic<int> live_count{0};
    std::atomic<bool> retired{false};
    // Guards against release() and install_new_block() both observing "retired && live_count ==
    // 0" for the same block and queueing it for a free twice.
    std::atomic<bool> queued_for_free{false};
  };

  static constexpr size_t kDefaultBlockSize = 1 << 20;  // 1MB

  explicit UpperArena(size_t block_size = kDefaultBlockSize) : block_size_(block_size) {
    current_.store(new_block(block_size_), std::memory_order_relaxed);
  }

  ~UpperArena() {
    // The whole PSkipList (and this arena along with it) is being destroyed -- no concurrent
    // access is possible any more, so every block ever created is freed unconditionally here,
    // regardless of whether drain_pending_blocks() already got to it (a block whose buffer was
    // already freed has `data == nullptr`; deleting a null array is a no-op).
    for (Block *block : all_blocks_) {
      delete[] block->data;
      delete block;
    }
  }

  UpperArena(const UpperArena &) = delete;
  auto operator=(const UpperArena &) -> UpperArena & = delete;

  // Bump-allocates `bytes` from the current block (rounded up to kAlignUnit so every allocation
  // is at least std::max_align_t-aligned, sufficient for the std::atomic<uint64_t> forward-
  // pointer array placed inside it). Installs a fresh block if the current one doesn't fit it,
  // or -- if `bytes` alone exceeds a normal block -- an "irregular" block sized exactly to it,
  // mirroring RocksDB's Arena. Thread-safe: lock-free on the common path; only the (rare) thread
  // that actually loses the race to fill a block pays the blocks_mutex_ cost of installing the
  // next one. Returns the raw memory and, via `out_block`, the block it came from -- callers
  // must pass that same block to release() exactly once when the allocation is retired.
  [[nodiscard]] auto allocate(size_t bytes, Block **out_block) -> std::byte * {
    bytes = align_up(bytes);
    for (;;) {
      Block *block = current_.load(std::memory_order_acquire);
      size_t offset = block->bump_offset.load(std::memory_order_relaxed);
      for (;;) {
        if (offset + bytes > block->capacity) {
          break;
        }
        if (block->bump_offset.compare_exchange_weak(
                offset, offset + bytes, std::memory_order_acq_rel, std::memory_order_relaxed)) {
          block->live_count.fetch_add(1, std::memory_order_relaxed);
          *out_block = block;
          return block->data + offset;
        }
      }
      install_new_block(block, bytes);
    }
  }

  // Decrements `block`'s live count. If this was the last live node in a block that has already
  // been retired, queues the block's buffer for freeing -- NOT freed immediately: a writer that
  // read `current_` just before retirement could still be mid-CAS against this block's (still
  // valid) memory, so actually freeing it here would race that straggler. The caller (PSkipList::
  // reclaim(), which already runs an epoch-based-reclamation drain proving no such straggler
  // remains active) must call drain_pending_blocks() after that drain to actually free anything
  // queued here.
  void release(Block *block) {
    if (block->live_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      maybe_queue_for_free(block);
    }
  }

  // Actually frees every block queued by release()/install_new_block() so far. Safe to call only
  // after the caller has independently established (via EBR or an equivalent drain) that no
  // writer still holds a `current_`-derived reference to a retired block -- see release()'s
  // comment. Not called from allocate()/release() themselves, deliberately: freeing must be
  // externally paced by PSkipList::reclaim()'s own epoch drain, not by whichever thread happens
  // to observe a block's live_count hit zero.
  void drain_pending_blocks() {
    std::vector<Block *> to_free;
    {
      std::lock_guard<std::mutex> lock(pending_free_mutex_);
      to_free.swap(pending_free_);
    }
    for (Block *block : to_free) {
      delete[] block->data;
      block->data = nullptr;
    }
  }

 private:
  static constexpr size_t kAlignUnit = alignof(std::max_align_t);
  static auto align_up(size_t n) -> size_t { return (n + kAlignUnit - 1) & ~(kAlignUnit - 1); }

  // Not locked itself -- called either before any concurrency exists (constructor) or by a
  // caller already holding blocks_mutex_ (install_new_block()).
  auto new_block(size_t size) -> Block * {
    auto *block = new Block();
    block->data = new std::byte[size];
    block->capacity = size;
    all_blocks_.push_back(block);
    return block;
  }

  // Installs a fresh block as the new bump-allocation target and retires `old_current`. Only the
  // thread that wins blocks_mutex_ actually installs one; every other thread racing to fill the
  // same block just finds current_ already updated on its next outer-loop iteration and retries
  // against the fresh block.
  void install_new_block(Block *old_current, size_t min_bytes) {
    const std::lock_guard<std::mutex> lock(blocks_mutex_);
    if (current_.load(std::memory_order_acquire) != old_current) {
      return;  // another thread already installed one while we were waiting for the lock
    }
    const size_t new_size = min_bytes > block_size_ ? min_bytes : block_size_;
    Block *fresh = new_block(new_size);
    current_.store(fresh, std::memory_order_release);
    old_current->retired.store(true, std::memory_order_release);
    maybe_queue_for_free(old_current);
  }

  void maybe_queue_for_free(Block *block) {
    if (!block->retired.load(std::memory_order_acquire)) {
      return;  // still the current bump target -- more nodes may yet be allocated from it
    }
    if (block->live_count.load(std::memory_order_acquire) != 0) {
      return;  // still has live nodes
    }
    bool expected = false;
    if (!block->queued_for_free.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;  // release() and install_new_block() both observed zero -- only one queues it
    }
    const std::lock_guard<std::mutex> lock(pending_free_mutex_);
    pending_free_.push_back(block);
  }

  size_t block_size_;
  std::atomic<Block *> current_;

  std::mutex blocks_mutex_;
  std::vector<Block *> all_blocks_;  // every block ever created, for the destructor's final sweep

  std::mutex pending_free_mutex_;
  std::vector<Block *> pending_free_;  // retired, zero-live blocks awaiting drain_pending_blocks()
};

}  // namespace pskiplist
