// reference_tracker.hpp — Thread-local reference tracking for lock-free memory reclamation (SMR)
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>

#include "core/spin_backoff.hpp"

namespace vmemkv {

inline constexpr size_t kDefaultMaxThreads = 256;
inline constexpr size_t kCacheLineSize = 64;

// A lightweight, portably decoupled thread-local slot array for tracking active references.
// Used to implement Epoch-based Reclamation (EBR) and Hazard Pointer (HP) mechanisms
// without the atomic cache-bouncing overhead of shared_ptr reference counting.
// Elements are cacheline-aligned (alignas(64)) to completely eliminate False Sharing.
template <typename T, size_t MaxThreads = kDefaultMaxThreads>
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

  ThreadReferenceTracker() noexcept {
    for (size_t i = 0; i < MaxThreads; ++i) {
      slots_[i].value.store(T{}, std::memory_order_relaxed);
    }
  }

  ~ThreadReferenceTracker() noexcept = default;

  ThreadReferenceTracker(const ThreadReferenceTracker &) = delete;
  auto operator=(const ThreadReferenceTracker &) -> ThreadReferenceTracker & = delete;

  // Registers the current thread's reference. seq_cst (not release): paired with
  // wait_until_retired()'s seq_cst load below to close a store-buffering race where a writer's
  // "is draining_ set?" check and the drain scan's "has this slot registered?" check could each
  // observe the other's pre-update value -- acquire/release alone doesn't rule that out for two
  // independent atomics read/written by both sides. See T2FlatFile::acquire_write_handle().
  void acquire(T val) const noexcept { slots_[get_thread_id()].value.store(val, std::memory_order_seq_cst); }

  // Clears the current thread's reference.
  void release() const noexcept { slots_[get_thread_id()].value.store(T{}, std::memory_order_release); }

  // Waits (spins/yields) until all thread slots no longer reference the specified old value.
  // Useful for Hazard-pointer-like pointer retired validation. seq_cst load: see acquire()'s comment.
  // SpinBackoff (not a bare yield()): a pure yield-spin here converges quickly on an
  // under-subscribed machine but reproduced as genuine, sustained starvation on CI's real
  // low-core-count, contended runner (see SpinBackoff's own doc comment for the precedent this
  // repeats -- the same fix already applied to VMemKVImpl::get_impl()/try_in_place_update()'s
  // retry loops for the identical reason).
  void wait_until_retired(T old_val) const noexcept {
    for (size_t i = 0; i < MaxThreads; ++i) {
      SpinBackoff backoff;
      while (slots_[i].value.load(std::memory_order_seq_cst) == old_val) {
        backoff.wait();
      }
    }
  }

  // Waits until all thread slots have cleared (are T{}) or have advanced beyond the target epoch value.
  // Useful for Epoch-based Reclamation. SpinBackoff -- see wait_until_retired()'s identical comment.
  void wait_until_epoch(T target_epoch) const noexcept {
    for (size_t i = 0; i < MaxThreads; ++i) {
      SpinBackoff backoff;
      while (true) {
        T epoch_val = slots_[i].value.load(std::memory_order_acquire);
        if (epoch_val == T{} || epoch_val >= target_epoch) {
          break;
        }
        backoff.wait();
      }
    }
  }

 private:
  auto get_thread_id() const noexcept -> size_t {
    // No reference back to the tracker and no destructor: `reg` is thread_local, shared by
    // every ThreadReferenceTracker<T, MaxThreads> instance this thread ever registers with, so
    // a destructor tied to one specific instance could run after that instance was destroyed.
    // Safe to skip: every acquire() is paired with a release() via Guard's RAII, so a live
    // tracker's slot is already back to T{} well before its thread could exit.
    struct SlotRegistration {
      size_t slot_id = static_cast<size_t>(-1);

      void register_slot(const ThreadReferenceTracker *trt) noexcept {
        static std::atomic<size_t> search_start{0};
        size_t start = search_start.fetch_add(1) % MaxThreads;

        for (size_t i = 0; i < MaxThreads; ++i) {
          size_t idx = (start + i) % MaxThreads;
          T expected = T{};
          if (trt->slots_[idx].value.compare_exchange_strong(expected, T(1), std::memory_order_acq_rel)) {
            trt->slots_[idx].value.store(T{}, std::memory_order_release);
            slot_id = idx;
            return;
          }
        }
        slot_id = start;
      }
    };

    thread_local static SlotRegistration reg;
    if (reg.slot_id == static_cast<size_t>(-1)) {
      reg.register_slot(this);
    }
    return reg.slot_id;
  }

  struct alignas(kCacheLineSize) AlignedSlot {
    std::atomic<T> value;
  };

  mutable std::array<AlignedSlot, MaxThreads> slots_;
};

}  // namespace vmemkv
