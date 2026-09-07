// reference_tracker.hpp — Thread-local reference tracking for lock-free memory reclamation (SMR)
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "core/spin_backoff.hpp"

namespace vmemkv {

inline constexpr size_t kCacheLineSize = 64;

// A process-wide, monotonically increasing ID assigned once per thread, shared by every
// ThreadReferenceTracker<T> instance regardless of T or which specific instance a thread happens
// to touch first -- this is what lets each tracker index a thread's slot directly (get() is the
// same value everywhere): a slot index assigned in one tracker instance is guaranteed free in
// every other tracker instance too, so two threads can never collide on the same slot in a
// shared tracker regardless of which tracker each first touched. Never reused once assigned, even
// after the thread exits -- see ThreadReferenceTracker's own comment for why that's an acceptable
// tradeoff here.
class GlobalThreadId {
 public:
  static auto get() noexcept -> size_t {
    thread_local const size_t thread_id = next_id_.fetch_add(1, std::memory_order_relaxed);
    return thread_id;
  }

 private:
  static inline std::atomic<size_t> next_id_{0};
};

// A lightweight, portably decoupled thread-local slot array for tracking active references.
// Used to implement Epoch-based Reclamation (EBR) and Hazard Pointer (HP) mechanisms
// without the atomic cache-bouncing overhead of shared_ptr reference counting.
// Elements are cacheline-aligned (alignas(64)) to completely eliminate False Sharing.
//
// Backed by a fixed-size array of atomic pointers to fixed-size chunks, each chunk allocated
// (and the pointer published) only once GlobalThreadId::get() first needs a slot in it -- no
// risk of two threads sharing a slot (see GlobalThreadId's own comment). Growing never
// reallocates or moves already-published memory: a chunk, once allocated, is never touched again
// by growth, so a concurrent index-based read of an already-published chunk can never race with
// another thread's growth of a *different* chunk. The common case (a thread whose chunk already exists) is a
// single atomic load, no lock at all. Tradeoffs: a slot is never reclaimed once grown into, even
// after that thread exits -- acceptable here since every acquire() is paired with a release() via
// Guard's RAII (so a live slot always reads back to T{} once idle) and this store's thread
// population is pool-shaped, not spawn-a-thread-per-request; and the total thread-ID space this
// can address is bounded (see kMaxChunks), generous enough for that same pool-shaped population.
template <typename T>
class ThreadReferenceTracker {
 public:
  // RAII Guard template to automate reference acquisition and release.
  class Guard {
   public:
    Guard(const ThreadReferenceTracker &tracker, T val) noexcept : tracker_(tracker), val_(val) {
      tracker_.acquire(val_);
    }
    ~Guard() noexcept { tracker_.release(); }

    auto operator->() const noexcept -> T { return val_; }
    operator T() const noexcept { return val_; }

    Guard(const Guard &) = delete;
    auto operator=(const Guard &) -> Guard & = delete;

   private:
    const ThreadReferenceTracker &tracker_;
    T val_;
  };

  ThreadReferenceTracker() noexcept = default;

  // Frees every chunk this instance ever allocated (see chunks_'s comment: allocation is
  // append-only and never reclaimed while live, so this is the only place chunks are freed).
  ~ThreadReferenceTracker() noexcept {
    for (auto &chunk_ptr : chunks_) {
      delete[] chunk_ptr.load(std::memory_order_relaxed);
    }
  }

  ThreadReferenceTracker(const ThreadReferenceTracker &) = delete;
  auto operator=(const ThreadReferenceTracker &) -> ThreadReferenceTracker & = delete;

  // Registers the current thread's reference. seq_cst (not release): paired with
  // wait_until_retired()'s seq_cst load below to close a store-buffering race where a writer's
  // "is writer_stop_ set?" check and the stop scan's "has this slot registered?" check could each
  // observe the other's pre-update value -- acquire/release alone doesn't rule that out for two
  // independent atomics read/written by both sides. See T2FlatFile::acquire_write_handle().
  void acquire(T val) const noexcept { slot_for_current_thread().store(val, std::memory_order_seq_cst); }

  // Clears the current thread's reference.
  void release() const noexcept { slot_for_current_thread().store(T{}, std::memory_order_release); }

  // Waits (spins/yields) until all thread slots no longer reference the specified old value.
  // Useful for Hazard-pointer-like pointer retired validation. seq_cst load: see acquire()'s
  // comment. SpinBackoff (not a bare yield()): avoids sustained starvation on a contended,
  // low-core-count machine -- see SpinBackoff's own doc comment.
  void wait_until_retired(T old_val) const noexcept {
    for_each_slot([&](std::atomic<T> &slot) {
      SpinBackoff backoff;
      while (slot.load(std::memory_order_seq_cst) == old_val) {
        backoff.wait();
      }
    });
  }

  // Waits until all thread slots have cleared (are T{}) or have advanced beyond the target epoch value.
  // Useful for Epoch-based Reclamation. SpinBackoff -- see wait_until_retired()'s identical comment.
  // seq_cst load: see acquire()'s comment -- the same independent-atomics store-buffering hazard
  // applies here whenever a caller pairs this wait against its own separately-published boundary
  // (e.g. VMemKVImpl::InPlaceUpdateBarrier waiting against capture_watermark_): without it, a
  // registration racing this scan could be invisible to it even though the registering thread's
  // own read of the boundary is guaranteed ordered after the publish.
  void wait_until_epoch(T target_epoch) const noexcept {
    for_each_slot([&](std::atomic<T> &slot) {
      SpinBackoff backoff;
      while (true) {
        T epoch_val = slot.load(std::memory_order_seq_cst);
        if (epoch_val == T{} || epoch_val >= target_epoch) {
          break;
        }
        backoff.wait();
      }
    });
  }

 private:
  struct alignas(kCacheLineSize) AlignedSlot {
    std::atomic<T> value{T{}};
  };

  // Chunk granularity: kChunkSize slots per allocation (64 * 64-byte AlignedSlot == one 4 KiB
  // page per chunk). kMaxChunks bounds the total addressable thread-ID space at
  // kChunkSize * kMaxChunks == 65536 -- generous for this store's pool-shaped thread population
  // (see class comment); chunks_ itself only costs kMaxChunks * sizeof(pointer) == 8 KiB
  // regardless of how many chunks are ever actually allocated, since unused entries stay null.
  static constexpr size_t kChunkSize = 64;
  static constexpr size_t kMaxChunks = 1024;

  // Fast path: this thread's chunk already exists -- one atomic load, no lock. Slow path (a
  // thread ID whose chunk this tracker has never allocated): ensure_chunk() takes growth_mutex_
  // and allocates it. Growth never touches an already-published chunk's memory or moves it, so a
  // concurrent index-based read of a *different*, already-published chunk can never race with
  // this allocation (see class comment for why this rules out the std::deque design's race).
  auto slot_for_current_thread() const -> std::atomic<T> & {
    const size_t thread_id = GlobalThreadId::get();
    const size_t chunk_index = thread_id / kChunkSize;
    if (chunk_index >= kMaxChunks) {
      throw std::runtime_error("ThreadReferenceTracker: thread-ID space exhausted");
    }
    AlignedSlot *chunk = chunks_[chunk_index].load(std::memory_order_acquire);
    if (chunk == nullptr) {
      chunk = ensure_chunk(chunk_index);
    }
    return chunk[thread_id % kChunkSize].value;
  }

  // Allocates chunks_[chunk_index] if another thread hasn't already (checked again under the
  // lock). A racing wait_until_retired()/wait_until_epoch() call reading high_chunk_/chunks_
  // without this lock is safe to interleave either way -- see the class comment for why a writer
  // whose registration isn't yet visible to a concurrent wait can't have observed whatever
  // boundary that wait is enforcing either (it hasn't reached the read that would matter yet), so
  // missing a just-published chunk here is never a correctness problem, only a growth timing detail.
  auto ensure_chunk(size_t chunk_index) const -> AlignedSlot * {
    const std::lock_guard<std::mutex> lock(growth_mutex_);
    AlignedSlot *chunk = chunks_[chunk_index].load(std::memory_order_relaxed);
    if (chunk == nullptr) {
      chunk = new AlignedSlot[kChunkSize];
      chunks_[chunk_index].store(chunk, std::memory_order_release);
      if (const size_t new_high = chunk_index + 1; new_high > high_chunk_.load(std::memory_order_relaxed)) {
        high_chunk_.store(new_high, std::memory_order_release);
      }
    }
    return chunk;
  }

  // Invokes `fn(slot)` for every slot in every chunk allocated so far. A null (never-allocated)
  // chunk is skipped outright: no thread has ever registered in its ID range for this instance,
  // so it can hold no live reference `fn` would need to observe.
  template <typename Fn>
  void for_each_slot(Fn &&fn) const {
    const size_t chunk_count = high_chunk_.load(std::memory_order_acquire);
    for (size_t c = 0; c < chunk_count; ++c) {
      AlignedSlot *chunk = chunks_[c].load(std::memory_order_acquire);
      if (chunk == nullptr) {
        continue;
      }
      for (size_t i = 0; i < kChunkSize; ++i) {
        fn(chunk[i].value);
      }
    }
  }

  mutable std::mutex growth_mutex_;
  mutable std::array<std::atomic<AlignedSlot *>, kMaxChunks> chunks_{};
  // High-water mark of chunk indices ever allocated (not necessarily dense -- see for_each_slot()).
  mutable std::atomic<size_t> high_chunk_{0};
};

}  // namespace vmemkv
