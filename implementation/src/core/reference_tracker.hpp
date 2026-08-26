// reference_tracker.hpp — Thread-local reference tracking for lock-free memory reclamation (SMR)
#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

#include "core/spin_backoff.hpp"

namespace vmemkv {

inline constexpr size_t kCacheLineSize = 64;

// A process-wide, monotonically increasing ID assigned once per thread, shared by every
// ThreadReferenceTracker<T> instance regardless of T or which specific instance a thread happens
// to touch first -- this is what lets each tracker index a thread's slot directly (get() is the
// same value everywhere), rather than each tracker independently searching for a "free" slot and
// caching that instance-specific answer under one shared thread_local (the previous design's bug:
// a slot free in the first tracker a thread ever touched says nothing about whether that same
// index is free in a second, unrelated tracker instance of the same type -- two threads could
// each get a locally-unique slot in different first-touched trackers, then collide when both
// later touch a third, shared tracker). Never reused once assigned, even after the thread exits
// -- see ThreadReferenceTracker's own comment for why that's an acceptable tradeoff here.
class GlobalThreadId {
 public:
  static auto get() noexcept -> size_t {
    thread_local const size_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    return id;
  }

 private:
  static inline std::atomic<size_t> next_id_{0};
};

// A lightweight, portably decoupled thread-local slot array for tracking active references.
// Used to implement Epoch-based Reclamation (EBR) and Hazard Pointer (HP) mechanisms
// without the atomic cache-bouncing overhead of shared_ptr reference counting.
// Elements are cacheline-aligned (alignas(64)) to completely eliminate False Sharing.
//
// Backed by a std::deque, grown on demand to fit GlobalThreadId::get()'s highest value seen so
// far -- no fixed thread-count ceiling, and no risk of two threads sharing a slot (see
// GlobalThreadId's own comment for the bug this replaces). push_back/emplace_back on a deque
// invalidates iterators but never references/pointers to existing elements, so growth (behind
// growth_mutex_, only taken when a thread's ID hasn't been seen by this tracker before) never
// disturbs another thread's already-established slot reference; the common case (a thread whose
// ID already fits within capacity_) is a single atomic load, no lock at all. Tradeoff: a slot is
// never reclaimed once grown into, even after that thread exits -- acceptable here since every
// acquire() is paired with a release() via Guard's RAII (so a live slot always reads back to T{}
// once idle) and this store's thread population is pool-shaped, not spawn-a-thread-per-request.
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
  ~ThreadReferenceTracker() noexcept = default;

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
  // Useful for Hazard-pointer-like pointer retired validation. seq_cst load: see acquire()'s comment.
  // SpinBackoff (not a bare yield()): a pure yield-spin here converges quickly on an
  // under-subscribed machine but reproduced as genuine, sustained starvation on CI's real
  // low-core-count, contended runner (see SpinBackoff's own doc comment for the precedent this
  // repeats -- the same fix already applied to VMemKVImpl::get_impl()/try_in_place_update()'s
  // retry loops for the identical reason).
  void wait_until_retired(T old_val) const noexcept {
    const size_t count = current_slot_count();
    for (size_t i = 0; i < count; ++i) {
      SpinBackoff backoff;
      while (slots_[i].value.load(std::memory_order_seq_cst) == old_val) {
        backoff.wait();
      }
    }
  }

  // Waits until all thread slots have cleared (are T{}) or have advanced beyond the target epoch value.
  // Useful for Epoch-based Reclamation. SpinBackoff -- see wait_until_retired()'s identical comment.
  // seq_cst load: see acquire()'s comment -- the same independent-atomics store-buffering hazard
  // applies here whenever a caller pairs this wait against its own separately-published boundary
  // (e.g. VMemKVImpl::InPlaceUpdateBarrier waiting against capture_watermark_): without it, a
  // registration racing this scan could be invisible to it even though the registering thread's
  // own read of the boundary is guaranteed ordered after the publish.
  void wait_until_epoch(T target_epoch) const noexcept {
    const size_t count = current_slot_count();
    for (size_t i = 0; i < count; ++i) {
      SpinBackoff backoff;
      while (true) {
        T epoch_val = slots_[i].value.load(std::memory_order_seq_cst);
        if (epoch_val == T{} || epoch_val >= target_epoch) {
          break;
        }
        backoff.wait();
      }
    }
  }

 private:
  struct alignas(kCacheLineSize) AlignedSlot {
    std::atomic<T> value{T{}};
  };

  // Fast path: this thread's id already has a slot -- one atomic load, no lock. Slow path (a
  // thread this tracker has never seen before): take growth_mutex_ and grow the deque up to and
  // including `id`. A racing wait_until_retired()/wait_until_epoch() call taking the same mutex
  // to read current_slot_count() is safe to interleave either way -- see the class comment for
  // why a writer whose registration isn't yet visible to a concurrent wait can't have observed
  // whatever boundary that wait is enforcing either (it hasn't reached the read that would
  // matter yet), so missing it here is never a correctness problem, only a growth timing detail.
  auto slot_for_current_thread() const -> std::atomic<T> & {
    const size_t id = GlobalThreadId::get();
    if (id < capacity_.load(std::memory_order_acquire)) {
      return slots_[id].value;
    }
    const std::lock_guard<std::mutex> lock(growth_mutex_);
    while (slots_.size() <= id) {
      slots_.emplace_back();
    }
    capacity_.store(slots_.size(), std::memory_order_release);
    return slots_[id].value;
  }

  auto current_slot_count() const -> size_t {
    const std::lock_guard<std::mutex> lock(growth_mutex_);
    return slots_.size();
  }

  mutable std::mutex growth_mutex_;
  // Mirrors slots_.size() so slot_for_current_thread()'s fast path can check it without taking
  // growth_mutex_; only ever written (under the mutex) after a push_back, so a thread observing
  // capacity_ > id here is guaranteed slots_[id] already exists.
  mutable std::atomic<size_t> capacity_{0};
  mutable std::deque<AlignedSlot> slots_;
};

}  // namespace vmemkv
