#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace pskiplist {

enum class EpochRole : uint8_t {
  kReader,
  kWriter,
};

// Per-parity-slot active counts, split by role. reclaim() (2.3節) waits for both readers
// and writers in a slot to drain, since either could hold a stale offset reference to a
// node about to be freed. checkpoint() (3章) waits only for writers, since a concurrent
// reader never mutates anything msync() could tear.
struct EpochSlot {
  std::atomic<int64_t> readers{0};
  std::atomic<int64_t> writers{0};
};

// RAII registration for a 2-slot epoch-based reclamation scheme: registers into whichever
// of `slots[0]`/`slots[1]` matches the current parity of `epoch` for the token's lifetime,
// under the counter matching `role`. Whoever advances `epoch` and waits for the slot the
// advance vacated to drain treats anything observed under it as safe to act on.
class EpochToken {
 public:
  EpochToken(std::array<EpochSlot, 2> &slots, std::atomic<uint64_t> &epoch, EpochRole role)
      : slots_(slots), role_(role) {
    parity_ = epoch.load(std::memory_order_acquire) & 1;
    counter().fetch_add(1, std::memory_order_acq_rel);
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
