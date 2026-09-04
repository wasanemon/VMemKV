#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace pskiplist {

enum class EpochRole : uint8_t {
  kReader,
  kWriter,
};

// Per-parity-slot active counts, split by role: reclaim() waits for both readers and writers
// to drain (either could hold a stale offset), checkpoint() waits only for writers (a reader
// never mutates anything msync() could tear).
struct EpochSlot {
  std::atomic<int64_t> readers{0};
  std::atomic<int64_t> writers{0};
};

// RAII registration into whichever of `slots[0]`/`slots[1]` matches `epoch`'s current parity,
// under the counter matching `role`. Whoever advances `epoch` and waits for the slot it
// vacated to drain treats anything observed under it as safe to act on.
class EpochToken {
 public:
  // Load-then-increment isn't atomic: a thread can read `epoch`, stall, and have reclaim()
  // drain that exact slot to 0 and free memory before the fetch_add lands. Re-reading `epoch`
  // after incrementing catches this — if it moved, undo and retry under the now-current
  // parity, which reclaim() can't have started draining yet.
  EpochToken(std::array<EpochSlot, 2> &slots, std::atomic<uint64_t> &epoch, EpochRole role)
      : slots_(slots), role_(role) {
    for (;;) {
      parity_ = epoch.load(std::memory_order_acquire) & 1;
      counter().fetch_add(1, std::memory_order_acq_rel);
      if ((epoch.load(std::memory_order_acquire) & 1) == parity_) {
        break;
      }
      counter().fetch_sub(1, std::memory_order_acq_rel);
    }
  }
  ~EpochToken() { counter().fetch_sub(1, std::memory_order_acq_rel); }
  EpochToken(const EpochToken &) = delete;
  auto operator=(const EpochToken &) -> EpochToken & = delete;

 private:
  [[nodiscard]] auto counter() -> std::atomic<int64_t> & {
    return role_ == EpochRole::kReader ? slots_[parity_].readers : slots_[parity_].writers;
  }

  std::array<EpochSlot, 2> &slots_;
  EpochRole role_;
  uint64_t parity_;
};

}  // namespace pskiplist
