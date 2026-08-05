// reference_tracker.hpp — Thread-local reference tracking for lock-free memory reclamation (SMR)
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>

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

  // Registers the current thread's reference.
  void acquire(T val) const noexcept { slots_[get_thread_id()].value.store(val, std::memory_order_release); }

  // Clears the current thread's reference.
  void release() const noexcept { slots_[get_thread_id()].value.store(T{}, std::memory_order_release); }

  // Waits (spins/yields) until all thread slots no longer reference the specified old value.
  // Useful for Hazard-pointer-like pointer retired validation.
  void wait_until_retired(T old_val) const noexcept {
    for (size_t i = 0; i < MaxThreads; ++i) {
      while (slots_[i].value.load(std::memory_order_acquire) == old_val) {
        std::this_thread::yield();
      }
    }
  }

  // Waits until all thread slots have cleared (are T{}) or have advanced beyond the target epoch value.
  // Useful for Epoch-based Reclamation.
  void wait_until_epoch(T target_epoch) const noexcept {
    for (size_t i = 0; i < MaxThreads; ++i) {
      while (true) {
        T epoch_val = slots_[i].value.load(std::memory_order_acquire);
        if (epoch_val == T{} || epoch_val >= target_epoch) {
          break;
        }
        std::this_thread::yield();
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
