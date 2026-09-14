// defrag_controller.hpp - T2 defragmentation for VMemKVImpl.
//
// Relocates live records out of garbage-heavy, fully-frozen 8MiB segments to the append
// frontier, then hole-punches the evacuated segments once their moves are WAL-durable (see
// docs/t2_defragment_design.md). Frozen means fully below base_boundary: nothing there is
// ever written in place again (in-place updates targeting it redirect out-of-place), and
// nothing new is ever appended there (appends only advance bytes_used), so a frozen
// segment's live set only shrinks -- relocation never races an arrival.
//
// Crash safety falls out of append-only discipline, never out of ordering tricks:
// - A crash between a record's copy-append and its T1 remap leaves an orphan copy; replay
//   never sees the move (it wasn't WAL-logged yet) and rebuilds T1 against the intact old
//   bytes. Progress lost, correctness kept.
// - A crash between remap and punch leaves the old bytes intact; the segment is rediscovered
//   (live counter 0, fully garbage) and re-queued by the next cycle. Self-healing.
// - Punch happens only for segments evacuated in a *previous* cycle whose moves were all
//   awaited durable, so replay always reproduces the remapped state before any reader could
//   observe punched zeros through a stale offset.
// - Moves replay as ordinary updates (append + overwrite).
//
// Records may straddle a segment boundary (up to ~1MiB overhang). Punching a whole segment
// is still exact: victim S's collection span is [S_start - slop, S_end), so every live tail
// reaching into S from S-1 is relocated with S, and S's own overhang into S+1 is garbage
// S+1's own future punch covers.
#pragma once

#include <linux/magic.h>
#include <sys/vfs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "api/utils.hpp"
#include "t1_index/sharded_t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"
#include "vmemkv/read_path.hpp"
#include "vmemkv/t2_ownership.hpp"
#include "vmemkv/write_path.hpp"
#include "wal/wal.hpp"

namespace vmemkv {

struct DefragState {
  // Single-flights defrag_cycle() across the background worker and explicit defragment().
  std::atomic<bool> running{false};
  // Guards pending_punch_ only -- never held across a fallocate() call (see
  // punch_evacuated_segments()). pending_punch_count_ mirrors the size for the lock-free
  // trigger predicate.
  mutable std::mutex punch_mu;
  std::vector<uint64_t> pending_punch_;
  mutable std::atomic<size_t> pending_punch_count{0};
  // Trigger markers: checkpoint count and overhead (basis points) at the last full scan cycle.
  mutable std::atomic<uint64_t> last_checkpoint{0};
  mutable std::atomic<uint64_t> last_overhead_bp{0};
  // Hole-punch support latch: without it relocation cannot reclaim.
  mutable std::atomic<bool> punch_probed{false};
  mutable std::atomic<bool> punch_supported{true};
  // Last-cycle stats published to get_statistics().
  mutable std::atomic<uint64_t> cycle_count{0};
  mutable std::atomic<uint64_t> last_duration_us{0};
  mutable std::atomic<uint64_t> last_moved_bytes{0};
  mutable std::atomic<uint64_t> last_punched_bytes{0};
};

namespace defrag_detail {

// Max record overhang past a segment end: aligned lengths stay under 1MiB (block_count <
// 65536 in 16-byte units), so 1MiB of slop always covers a boundary crosser.
inline constexpr uint64_t kPunchSlopBytes = 1ULL << 20;

// Full-range T1 scan bounds: every 16-byte prefix falls in [lo, hi].
inline constexpr std::array<std::byte, 16> kDefragScanLo{};
inline const std::array<std::byte, 16> kDefragScanHi = [] {
  std::array<std::byte, 16> hi{};
  hi.fill(std::byte{0xFF});
  return hi;
}();

// Probes hole-punch support once per process (fstatfs magic; tmpfs cannot punch holes).
// Without this, relocation-only cycles on such filesystems would grow bytes_used without
// ever reclaiming anything -- each cycle re-victimizing the frontier -- until T2 capacity
// throws. A first failed punch attempt latches support off the same way (fallocate failures
// here are structural, never transient: the range is always valid and aligned).
inline auto punch_supported(DefragState &state, const std::filesystem::path &t2_path) -> bool {
  if (!state.punch_probed.load(std::memory_order_relaxed)) {
    struct statfs sfs {};
    bool supported = true;
    if (::statfs(vmemkv::derive_t2_chk_path(t2_path).c_str(), &sfs) == 0) {
      supported = (sfs.f_type != TMPFS_MAGIC);
    }
    state.punch_supported.store(supported, std::memory_order_relaxed);
    state.punch_probed.store(true, std::memory_order_relaxed);
  }
  return state.punch_supported.load(std::memory_order_relaxed);
}

inline auto scan_wanted(const DefragState &state, uint64_t overhead_bp, uint64_t checkpoint_count) -> bool {
  if (overhead_bp < 500) {
    return false;
  }
  const uint64_t threshold_bp = (100 - T2Config::kDefragSpaceOverheadPercent) * 100;
  const uint64_t last_bp = state.last_overhead_bp.load(std::memory_order_relaxed);
  if (overhead_bp >= threshold_bp && last_bp < threshold_bp) {
    return true;
  }
  if (overhead_bp >= last_bp + 500) {
    return true;
  }
  return checkpoint_count != state.last_checkpoint.load(std::memory_order_relaxed);
}

// Cheap trigger predicate for the background poll: atomics only, no scan. A full scan cycle
// runs on force, when overhead crosses the configured threshold, when it has worsened
// notably since the last scan, or when a checkpoint froze new segments since then. Punch of
// previously-evacuated segments rides along whenever one is pending.
template <typename ConfigT>
auto defrag_wanted(const DefragState &state,
                   const T2Ownership<ConfigT> &own,
                   const T2FlatFile &t2,
                   bool recovering,
                   uint64_t checkpoint_count) -> bool {
  if (recovering) {
    return false;
  }
  if (state.pending_punch_count.load(std::memory_order_relaxed) > 0) {
    return true;
  }
  const uint64_t used = t2.bytes_used();
  if (used == 0) {
    return false;
  }
  const uint64_t live = own.live_total();
  if (live >= used) {
    return false;
  }
  return scan_wanted(state, (used - live) * 10000 / used, checkpoint_count);
}

// A segment is punchable only while evacuated (live counter 0) *and* fully frozen: re-check
// both here, at punch time, not just at queue time.
template <typename ConfigT>
auto is_evacuated_segment(const T2Ownership<ConfigT> &own, const T2Memory *mem, uint64_t seg) -> bool {
  if (seg >= own.segment_count()) {
    return false;
  }
  if (own.seg_live(seg) != 0) {
    return false;
  }
  const uint64_t seg_end = (seg + 1) * ConfigT::T2SegmentBytes;
  return seg_end <= mem->base_boundary.load(std::memory_order_acquire);
}

// Punches segments evacuated by the previous cycle. Returns punched bytes. Already-hollow
// ranges resolve inside the punch call and count nothing; a failed punch latches support
// off and drops the queue. The punch mutex only protects queue bookkeeping -- the list is
// copied out first and every fallocate() runs lock-free, then the queue is reconciled by
// removing exactly the punched segments (so segments queued meanwhile are never lost).
template <typename ConfigT>
auto punch_evacuated_segments(DefragState &state,
                              T2Ownership<ConfigT> &own,
                              const T2FlatFile &t2,
                              const T2Memory *mem) -> uint64_t {
  std::vector<uint64_t> segs;
  {
    std::lock_guard<std::mutex> punch_lock(state.punch_mu);
    segs = state.pending_punch_;
  }
  std::vector<uint64_t> punched;
  uint64_t punched_bytes = 0;
  bool punch_failed = false;
  for (const uint64_t seg : segs) {
    if (!is_evacuated_segment(own, mem, seg)) {
      continue;  // Left for the next cycle.
    }
    const uint64_t seg_start = seg * ConfigT::T2SegmentBytes;
    switch (t2.punch_if_occupied(seg_start, ConfigT::T2SegmentBytes)) {
      case vmemkv::T2FlatFile::PunchOutcome::Punched:
        punched.push_back(seg);
        punched_bytes += ConfigT::T2SegmentBytes;
        break;
      case vmemkv::T2FlatFile::PunchOutcome::AlreadyHollow:
        punched.push_back(seg);
        break;
      case vmemkv::T2FlatFile::PunchOutcome::Failed:
        // Structural (aligned valid range), never transient: latch support off and drop
        // the queue rather than retry-storming every cycle.
        punch_failed = true;
        break;
    }
    if (punch_failed) {
      break;
    }
  }
  {
    std::lock_guard<std::mutex> punch_lock(state.punch_mu);
    if (punch_failed) {
      state.punch_supported.store(false, std::memory_order_relaxed);
      state.pending_punch_.clear();
    } else if (!punched.empty()) {
      for (uint64_t seg : punched) {
        state.pending_punch_.erase(std::remove(state.pending_punch_.begin(), state.pending_punch_.end(), seg),
                                   state.pending_punch_.end());
      }
    }
    state.pending_punch_count.store(state.pending_punch_.size(), std::memory_order_relaxed);
  }
  return punched_bytes;
}

// Selects victim segments from the authoritative live counters: frozen segments at >=50%
// garbage, greediest first, bounded by the per-cycle move budget. Garbage uses exact used
// bytes (not the hint-undercounted live), so a segment reads as full as it physically is.
// Zero-live frozen segments select too (100% garbage): the punch phase hollow-skips the
// already-punched ones and queues the rest, which also heals restarts (no punch list kept).
template <typename ConfigT>
auto select_victims(const T2Ownership<ConfigT> &own, uint64_t base, uint64_t used) -> std::vector<uint64_t> {
  struct Victim {
    uint64_t seg;
    uint64_t garbage;
  };
  const uint64_t n = own.segment_count();
  std::vector<Victim> victims;
  for (uint64_t seg = 0; seg < n; ++seg) {
    const uint64_t seg_start = seg * ConfigT::T2SegmentBytes;
    const uint64_t seg_end = seg_start + ConfigT::T2SegmentBytes;
    if (seg_end > base || seg_start >= used) {
      continue;
    }
    const uint64_t seg_used = std::min(seg_end, used) - seg_start;
    const uint64_t seg_live = std::min(own.seg_live(seg), seg_used);
    if (seg_used > seg_live && (seg_used - seg_live) * 2 >= seg_used) {
      victims.push_back(Victim{seg, seg_used - seg_live});
    }
  }
  std::sort(victims.begin(), victims.end(), [](const Victim &a, const Victim &b) { return a.garbage > b.garbage; });
  std::vector<uint64_t> victim_segs;
  uint64_t est_move = 0;
  for (const auto &victim : victims) {
    if (est_move >= ConfigT::T2DefragMaxMoveBytesPerCycle) {
      break;
    }
    victim_segs.push_back(victim.seg);
    est_move += std::min(own.seg_live(victim.seg), ConfigT::T2SegmentBytes);
  }
  std::sort(victim_segs.begin(), victim_segs.end());
  return victim_segs;
}

// Collects live offsets starting in the victim spans, ascending and deduplicated. Spans
// extend one slop (max record overhang) below each victim so live tails reaching into it
// relocate too; anything else in the slop zone stays put.
template <typename ConfigT, typename T1>
auto collect_victim_offsets(T1 &t1,
                            const T2FlatFile &t2,
                            const T2Ownership<ConfigT> &own,
                            const std::vector<uint64_t> &victim_segs,
                            uint64_t used,
                            const vmemkv::T2Memory *mem) -> std::vector<uint64_t> {
  std::vector<uint64_t> offsets;
  t1.scan(
      std::span<const std::byte>(kDefragScanLo.data(), kDefragScanLo.size()),
      std::span<const std::byte>(kDefragScanHi.data(), kDefragScanHi.size()),
      [&](std::span<const std::byte> /*index_key*/, uint64_t payload, uint64_t hash) {
        if (payload == vmemkv::STORE_NOT_FOUND || t1_detail::is_inline(hash)) {
          return;
        }
        const uint64_t off = payload & detail::kPayloadOffsetMask;
        if (off >= used) {
          return;
        }
        const uint64_t seg = T2Ownership<ConfigT>::seg_index(off);
        if (std::binary_search(victim_segs.begin(), victim_segs.end(), seg)) {
          offsets.push_back(off);
          return;
        }
        if (seg + 1 < own.segment_count() && std::binary_search(victim_segs.begin(), victim_segs.end(), seg + 1)) {
          const uint64_t span_start = (seg + 1) * ConfigT::T2SegmentBytes;
          if (off + kPunchSlopBytes >= span_start && off < span_start) {
            const T2RecordView head = t2.at(off, mem);
            const uint64_t aligned =
                vmemkv::align_up(sizeof(ValueRecordHeader) + head.header->key_len + head.header->value_len);
            if (off + aligned > span_start) {
              offsets.push_back(off);
            }
          }
        }
      });
  std::sort(offsets.begin(), offsets.end());
  offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
  return offsets;
}

// Relocates one live record (identified by its current T2 offset) to the append frontier.
// Returns the moved record's exact aligned bytes, or 0 when the entry moved on concurrently
// (offset mismatch under the stripe lock), vanished, or went inline.
template <typename ConfigT, typename T1, typename LockStripe, typename MaybeReorg>
auto relocate_offset(T1 &t1,
                     T2FlatFile &t2,
                     Wal &wal,
                     T2Ownership<ConfigT> &own,
                     LockStripe &&lock_stripe,
                     MaybeReorg &&maybe_reorg,
                     uint64_t bucket_offset,
                     const vmemkv::T2Memory *mem,
                     WalGroupAwaiter &awaiter) -> uint64_t {
  // Victim bytes are frozen (below base_boundary): never mutated in place, fully written
  // before the checkpoint that froze them -- readable without a lock. The T1 entry naming
  // them can still move concurrently, so everything below revalidates under the stripe lock.
  T2RecordView frozen = t2.at(bucket_offset & detail::kPayloadOffsetMask, mem);
  const uint64_t record_aligned =
      vmemkv::align_up(sizeof(ValueRecordHeader) + frozen.header->key_len + frozen.header->value_len);
  // Spans into the frozen mapping: valid through this section (the mapping lives with the
  // process, these bytes go nowhere before next cycle's punch at the earliest, and every
  // consumer below copies synchronously).
  const std::span<const std::byte> key_span = frozen.key;
  const std::span<const std::byte> value_span = frozen.value;

  std::lock_guard<std::mutex> key_lock(lock_stripe(key_span));
  const auto cur = t1.get_with_hash(key_span);
  if (cur.payload_bits == vmemkv::STORE_NOT_FOUND || t1_detail::is_inline(cur.raw_hash) ||
      (cur.payload_bits & detail::kPayloadOffsetMask) != (bucket_offset & detail::kPayloadOffsetMask)) {
    return 0;
  }
  // Same append + overwrite + WAL-update combination as update_impl()'s fall-through path
  // (including its segment accounting); small values may go inline, which evacuates just as well.
  if (!write_entry_lockfree<ConfigT>(t1, t2, own, maybe_reorg, key_span, value_span, cur)) {
    throw std::runtime_error("defrag relocate: T1 put did not apply");
  }
  awaiter.add(wal.reserve_update(key_span, value_span));
  return record_aligned;
}

// Scan + relocate phase of a cycle. Reserves batch first for group-commit fusion and awaits
// incrementally (not only at the end): one cycle can relocate up to
// T2DefragMaxMoveBytesPerCycle, far more than the WAL ring holds, and reserve() spins until
// a drain retires slots -- a drain only happens inside await_durable().
template <typename ConfigT, typename T1, typename LockStripe, typename MaybeReorg>
auto relocate_phase(T1 &t1,
                    T2FlatFile &t2,
                    Wal &wal,
                    T2Ownership<ConfigT> &own,
                    LockStripe &&lock_stripe,
                    MaybeReorg &&maybe_reorg,
                    const std::vector<uint64_t> &victim_segs,
                    uint64_t used,
                    const vmemkv::T2Memory *mem) -> uint64_t {
  uint64_t moved_bytes = 0;
  std::vector<uint64_t> offsets = collect_victim_offsets(t1, t2, own, victim_segs, used, mem);
  WalGroupAwaiter awaiter(wal);
  constexpr std::size_t kDefragAwaitEveryNRelocates = 1024;
  for (const uint64_t off : offsets) {
    moved_bytes += relocate_offset(t1, t2, wal, own, lock_stripe, maybe_reorg, off, mem, awaiter);
    if (awaiter.size() >= kDefragAwaitEveryNRelocates) {
      awaiter.drain();
    }
    if (moved_bytes >= ConfigT::T2DefragMaxMoveBytesPerCycle) {
      break;
    }
  }
  awaiter.drain();
  return moved_bytes;
}

// Queues verified-evacuated victims for next cycle's punch. A nonzero counter means the
// bucket missed something -- never punch on doubt. Short lock, no I/O inside.
template <typename ConfigT>
void queue_evacuated_victims(DefragState &state,
                             const T2Ownership<ConfigT> &own,
                             const std::vector<uint64_t> &victim_segs) {
  std::lock_guard<std::mutex> punch_lock(state.punch_mu);
  for (const uint64_t seg : victim_segs) {
    if (seg < own.segment_count() && own.seg_live(seg) == 0) {
      state.pending_punch_.push_back(seg);
    }
  }
  state.pending_punch_count.store(state.pending_punch_.size(), std::memory_order_relaxed);
}

// One defragmentation cycle: punches segments evacuated by the previous cycle, then (unless
// this is a punch-only run) rescans, selects victims, and relocates. Returns true when a
// full scan ran. Single-flight via DefragState::running; checkpoint needs no exclusion --
// its msync range always lies above any punched range, and defrag's T1 puts are ordinary
// concurrent writes.
template <typename ConfigT, typename T1, typename LockStripe, typename MaybeReorg>
auto defrag_cycle(DefragState &state,
                  T1 &t1,
                  T2FlatFile &t2,
                  Wal &wal,
                  T2Ownership<ConfigT> &own,
                  const std::filesystem::path &t2_path,
                  LockStripe &&lock_stripe,
                  MaybeReorg &&maybe_reorg,
                  uint64_t checkpoint_count,
                  bool force) -> bool {
  bool expected_running = false;
  if (!state.running.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel)) {
    return false;
  }
  struct RunningReset {
    std::atomic<bool> *flag;
    ~RunningReset() { flag->store(false, std::memory_order_release); }
  } reset{&state.running};

  // Relocation without reclamation only grows bytes_used: refuse the whole cycle where
  // holes cannot be punched (see punch_supported()).
  if (!punch_supported(state, t2_path)) {
    return false;
  }

  const auto cycle_start = std::chrono::steady_clock::now();
  uint64_t moved_bytes = 0;
  uint64_t punched_bytes = 0;
  bool scanned = false;

  const vmemkv::T2Memory *mem = t2.get_memory();
  const uint64_t base = mem->base_boundary.load(std::memory_order_acquire);
  const uint64_t used = mem->bytes_used.load(std::memory_order_acquire);

  // Phase 0: punch segments the previous cycle evacuated (quarantine: one full cycle between
  // evacuation and punch, so no in-flight reader can still name these offsets from before).
  // Already-hollow ranges (punched before, including before a restart, which keeps no punch
  // list) are skipped silently -- only real deallocations count toward punched_bytes.
  punched_bytes += punch_evacuated_segments(state, own, t2, mem);

  const uint64_t live = own.live_total();
  const uint64_t overhead_bp = (used == 0 || live >= used) ? 0 : (used - live) * 10000 / used;
  if (force || scan_wanted(state, overhead_bp, checkpoint_count)) {
    scanned = true;
    const std::vector<uint64_t> victim_segs = select_victims<ConfigT>(own, base, used);
    if (!victim_segs.empty()) {
      moved_bytes += relocate_phase(t1, t2, wal, own, lock_stripe, maybe_reorg, victim_segs, used, mem);
      queue_evacuated_victims(state, own, victim_segs);
    }
    state.last_checkpoint.store(checkpoint_count, std::memory_order_relaxed);
    state.last_overhead_bp.store(overhead_bp, std::memory_order_relaxed);
  }

  state.cycle_count.fetch_add(1, std::memory_order_relaxed);
  state.last_duration_us.store(
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - cycle_start)
              .count()),
      std::memory_order_relaxed);
  state.last_moved_bytes.store(moved_bytes, std::memory_order_relaxed);
  state.last_punched_bytes.store(punched_bytes, std::memory_order_relaxed);
  return scanned;
}

}  // namespace defrag_detail
}  // namespace vmemkv
