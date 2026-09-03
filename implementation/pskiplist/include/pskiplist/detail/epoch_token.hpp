#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace pskiplist {

// RAII registration for a 2-slot epoch-based reclamation scheme: registers into
// whichever of `active[0]`/`active[1]` matches the current parity of `epoch_parity` for
// the token's lifetime. A reclaimer flips `epoch_parity` and waits for the slot the
// flip vacated to drain before treating anything observed under it as safe to free.
class EpochToken {
 public:
  EpochToken(std::array<std::atomic<int64_t>, 2> &active, std::atomic<uint64_t> &epoch_parity) : active_(active) {
    parity_ = epoch_parity.load(std::memory_order_acquire) & 1;
    active_[parity_].fetch_add(1, std::memory_order_acq_rel);
  }
  ~EpochToken() { active_[parity_].fetch_sub(1, std::memory_order_acq_rel); }
  EpochToken(const EpochToken &) = delete;
  auto operator=(const EpochToken &) -> EpochToken & = delete;

 private:
  std::array<std::atomic<int64_t>, 2> &active_;
  uint64_t parity_;
};

}  // namespace pskiplist
