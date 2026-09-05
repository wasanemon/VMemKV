#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <vector>

namespace pskiplist {

// Bump-pointer block arena backing UpperChunk allocations, mirroring RocksDB's memtable Arena
// (see high_level_design.md 2.4.2). Blocks are never freed one node at a time: each tracks how
// many nodes are still live, and its buffer is freed only once that count hits zero and the
// block has been retired (superseded as the bump target) -- see release().
class UpperArena {
 public:
  struct Block {
    std::byte *data = nullptr;
    size_t capacity = 0;
    std::atomic<size_t> bump_offset{0};
    std::atomic<int> live_count{0};
    std::atomic<bool> retired{false};
    std::atomic<bool> queued_for_free{false};  // guards against queueing the same block twice
  };

  static constexpr size_t kDefaultBlockSize = 1 << 20;  // 1MB

  explicit UpperArena(size_t block_size = kDefaultBlockSize) : block_size_(block_size) {
    current_.store(new_block(block_size_), std::memory_order_relaxed);
  }

  ~UpperArena() {
    // No concurrent access is possible once the whole PSkipList is being destroyed, so every
    // block is freed unconditionally (deleting an already-null `data` is a no-op).
    for (Block *block : all_blocks_) {
      delete[] block->data;
      delete block;
    }
  }

  UpperArena(const UpperArena &) = delete;
  auto operator=(const UpperArena &) -> UpperArena & = delete;

  // Bump-allocates `bytes` (rounded up to kAlignUnit) from the current block, installing a new
  // one if it doesn't fit. Lock-free on the common path. Returns the memory and, via `out_block`,
  // the block it came from -- pass that to release() exactly once when done.
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

  // Decrements `block`'s live count; if it was the last one on an already-retired block, queues
  // the buffer for freeing -- not immediately, since a writer may still be mid-CAS against it.
  // The caller must call drain_pending_blocks() only after an EBR drain proves that's safe.
  void release(Block *block) {
    if (block->live_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      maybe_queue_for_free(block);
    }
  }

  // Actually frees blocks queued by release()/install_new_block() -- see release()'s comment
  // for why this must be paced by the caller's own EBR drain, not called from allocate()/release().
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

  // Unlocked: called only before concurrency exists, or while already holding blocks_mutex_.
  auto new_block(size_t size) -> Block * {
    auto *block = new Block();
    block->data = new std::byte[size];
    block->capacity = size;
    all_blocks_.push_back(block);
    return block;
  }

  // Installs a fresh bump target and retires `old_current`. Only the thread winning
  // blocks_mutex_ installs one; others retry against the already-updated current_.
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
