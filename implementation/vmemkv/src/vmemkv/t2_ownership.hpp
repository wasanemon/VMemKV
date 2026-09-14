// t2_ownership.hpp - T2-side ownership state for VMemKVImpl.
//
// Aggregates the T2 segment live-byte accounting, the checkpoint capture watermark, and
// the in-place-update barrier: the three pieces of state that together decide whether a
// byte range is safe to durabilize, punch, or rewrite in place.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "t1_index/t1_index.hpp"
#include "vmemkv/hooks.hpp"

namespace vmemkv {

namespace detail {
// T1 payload codec shared by the ownership counters and the read/write paths: a T2-offset
// payload is `offset | (block_count << kPayloadSizeShift)`, 16-byte-granular size hint.
inline constexpr uint64_t kPayloadSizeShift = 48;
inline constexpr uint64_t kPayloadOffsetMask = (1ULL << kPayloadSizeShift) - 1;
inline constexpr uint64_t kRecordBlockAlignment = 16;
}  // namespace detail

// Per-segment live bytes (size-hint sums) plus the store-wide total driving the defrag
// overhead trigger. Sized from the constructor's capacity argument; atomics because
// same-segment entries on different key stripes update concurrently.
template <typename ConfigT>
class T2Ownership {
 public:
  explicit T2Ownership(size_t segment_count) : seg_live_(segment_count) {
    // std::atomic's default constructor leaves the value indeterminate, so zero the freshly
    // sized table explicitly (vector sizing alone does not do it).
    for (auto &counter : seg_live_) {
      counter.store(0, std::memory_order_relaxed);
    }
  }

  // T2 segment index for a masked (kPayloadOffsetMask-applied) byte offset.
  static auto seg_index(uint64_t masked_offset) noexcept -> uint64_t {
    return masked_offset / ConfigT::T2SegmentBytes;
  }

  // Live-byte size a T2-offset payload contributes to segment accounting. Same 16-byte
  // granularity as the embedded hint (low_level_design.md 2.2): systematically at or under
  // the true aligned length, which is fine for ratios and triggers -- and zero-exactness
  // (counter == 0 means evacuated) still holds because every hint is strictly positive.
  static auto hint_bytes(uint64_t payload_bits) noexcept -> uint64_t {
    return (payload_bits >> detail::kPayloadSizeShift) * detail::kRecordBlockAlignment;
  }

  // Adjusts segment accounting for one T1 payload replacement. Callers hold the key's
  // stripe lock (or run single-threaded, during WAL replay / T1 checkpoint load), so the
  // old classification cannot race the put it describes. Either side that is a tombstone or
  // an inline value addresses no T2 segment and contributes nothing.
  void note_replace(uint64_t old_bits, uint64_t old_hash, bool new_is_offset, uint64_t new_bits) {
    if (old_bits != vmemkv::STORE_NOT_FOUND && !t1_detail::is_inline(old_hash)) {
      subtract_live(seg_index(old_bits & detail::kPayloadOffsetMask), hint_bytes(old_bits));
    }
    if (new_is_offset) {
      add_live(seg_index(new_bits & detail::kPayloadOffsetMask), hint_bytes(new_bits));
    }
  }

  void add_live(uint64_t seg, uint64_t hint) {
    if (seg < seg_live_.size()) {
      seg_live_[seg].fetch_add(hint, std::memory_order_relaxed);
      live_total_.fetch_add(hint, std::memory_order_relaxed);
    }
  }

  void subtract_live(uint64_t seg, uint64_t hint) {
    if (seg < seg_live_.size()) {
      seg_live_[seg].fetch_sub(hint, std::memory_order_relaxed);
      live_total_.fetch_sub(hint, std::memory_order_relaxed);
    }
  }

  auto seg_live(uint64_t seg) const noexcept -> uint64_t {
    return seg_live_[seg].load(std::memory_order_relaxed);
  }

  auto segment_count() const noexcept -> size_t { return seg_live_.size(); }

  auto live_total() const noexcept -> uint64_t { return live_total_.load(std::memory_order_relaxed); }

  // Write-side barrier claimed by checkpoint_internal(): any in-place update whose
  // allow_in_place check observes offset < watermark is guaranteed to be redirected
  // out-of-place instead of racing checkpoint's durabilizing read of that offset. Set once
  // per cycle to `target` (msync() covers the whole range in one shot, so the claim must
  // too) and never moved again until the cycle commits (reverted on abort, since nothing
  // durable happened). Conservative by construction: only ever needs to be *at least* as far
  // along as what's genuinely being read this cycle, never exactly so.
  //
  // seq_cst: paired with the barrier's own seq_cst registration/scan and the reader's seq_cst
  // load of this same field -- see ThreadReferenceTracker::acquire()'s comment for the
  // independent-atomics race this closes (watermark and the barrier's slot array are two
  // separate atomics touched by both sides).
  void claim_watermark(uint64_t target) noexcept { watermark_.store(target, std::memory_order_seq_cst); }

  void revert_watermark(uint64_t old_boundary) noexcept {
    watermark_.store(old_boundary, std::memory_order_seq_cst);
  }

  auto load_watermark() const noexcept -> uint64_t { return watermark_.load(std::memory_order_seq_cst); }

  [[nodiscard]] auto enter_in_place(uint64_t offset) const noexcept -> InPlaceUpdateBarrier::Guard {
    return barrier_.enter(offset);
  }

  // Drains any in-place write that had already passed its allow_in_place check against the old
  // (pre-claim) watermark and is still physically writing -- see InPlaceUpdateBarrier's own
  // contract. Without this, such a write could still be in flight when msync() reads this
  // range, or when base_boundary publishes past it below.
  void drain_in_place(uint64_t boundary) const noexcept { barrier_.wait_until_retired(boundary); }

 private:
  std::vector<std::atomic<uint64_t>> seg_live_;
  std::atomic<uint64_t> live_total_{0};
  std::atomic<uint64_t> watermark_{0};
  InPlaceUpdateBarrier barrier_;
};

}  // namespace vmemkv
