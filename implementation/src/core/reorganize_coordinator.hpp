#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace vmemkv {

// Coordinates single-flight reorganize ownership among writers.
// Soft path: one writer may run reorganize; followers continue writes.
// Hard path: one writer runs reorganize, followers wait for completion.
class ReorganizeCoordinator {
 public:
  struct ReorganizeHints {
    size_t append_size;
    size_t append_capacity;
    size_t soft_threshold_percent;
    size_t hard_threshold_percent;
  };

  struct AcquireResult {
    bool acquired;
    uint64_t wait_epoch;
  };

  template <typename Fn>
  void maybe_reorganize_if_needed(const ReorganizeHints &hints, Fn &&run_fn) {
    if (!is_reorganize_needed(hints)) {
      return;
    }

    if (is_hard_needed(hints)) {
      execute_hard(std::forward<Fn>(run_fn));
      return;
    }

    execute_soft(std::forward<Fn>(run_fn));
  }

 private:
  auto try_acquire_soft() noexcept -> bool {
    bool expected = false;
    return running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }

  auto acquire_or_wait_hard() noexcept -> AcquireResult {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return AcquireResult{true, 0};
    }
    return AcquireResult{false, completed_epoch_.load(std::memory_order_acquire) + 1};
  }

  template <typename Fn>
  void run_with_ownership(Fn &&run_fn) {
    struct ReleaseGuard {
      ReorganizeCoordinator &coordinator;
      ~ReleaseGuard() { coordinator.release_after_run(); }
    } release_guard{*this};

    run_fn();
  }

  void release_after_run() noexcept {
    completed_epoch_.fetch_add(1, std::memory_order_release);
    running_.store(false, std::memory_order_release);
  }

  void wait_for_epoch(uint64_t target_epoch) const noexcept {
    while (completed_epoch_.load(std::memory_order_acquire) < target_epoch) {
      std::this_thread::yield();
    }
  }

  static auto threshold_entries(size_t capacity, size_t percent) noexcept -> size_t {
    constexpr size_t kPercentBase = 100;
    return (capacity * percent) / kPercentBase;
  }

  static auto soft_threshold(const ReorganizeHints &hints) noexcept -> size_t {
    return threshold_entries(hints.append_capacity, hints.soft_threshold_percent);
  }

  static auto hard_threshold(const ReorganizeHints &hints) noexcept -> size_t {
    return threshold_entries(hints.append_capacity, hints.hard_threshold_percent);
  }

  static auto is_reorganize_needed(const ReorganizeHints &hints) noexcept -> bool {
    return hints.append_size >= soft_threshold(hints);
  }

  static auto is_hard_needed(const ReorganizeHints &hints) noexcept -> bool {
    if (hints.append_size >= hints.append_capacity) {
      return true;
    }
    return hints.append_size >= hard_threshold(hints);
  }

  template <typename Fn>
  void execute_soft(Fn &&run_fn) {
    // Soft path: followers do not wait; only a successful owner runs reorganize.
    if (try_acquire_soft()) {
      run_with_ownership(std::forward<Fn>(run_fn));
    }
  }

  template <typename Fn>
  void execute_hard(Fn &&run_fn) {
    // Hard path: followers wait for the current owner to finish, then return to caller.
    const auto acquire = acquire_or_wait_hard();
    if (acquire.acquired) {
      run_with_ownership(std::forward<Fn>(run_fn));
      return;
    }
    wait_for_epoch(acquire.wait_epoch);
  }

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> completed_epoch_{0};
};

}  // namespace vmemkv
