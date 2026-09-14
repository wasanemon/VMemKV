// checkpoint_coordinator.hpp - Checkpoint/reorganize orchestration for VMemKVImpl.
//
// Owns the reorg single-flight state (ReorgState) and every phase of a checkpoint cycle.
// VMemKVImpl keeps thin delegating members (reorganize_internal() stays a member because
// tests call it directly with injected hooks). Durabilization here is always in place
// (msync, no relocation); T2's live mmap is MAP_SHARED, so the durable copy and the live
// copy are always the same bytes.
#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "checkpoint/checkpoint.hpp"
#include "core/background_poll.hpp"
#include "core/single_flight.hpp"
#include "t1_index/sharded_t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"
#include "vmemkv/hooks.hpp"
#include "vmemkv/t2_ownership.hpp"
#include "wal/wal.hpp"

namespace vmemkv {

// T1Only: in-memory merge, zero I/O, T2 untouched. Checkpoint: durabilizes T2's live tail
// in-place and persists the result as a manifest-committed checkpoint (low_level_design.md
// 5.2, 5.5). VMemKVImpl re-exports this as its nested ReorgMode for API compatibility.
enum class ReorgMode { T1Only, Checkpoint };

// Single-flight and accounting state for reorganize/checkpoint cycles.
struct ReorgState {
  std::atomic<bool> requested{false};
  std::atomic<bool> running{false};
  std::atomic<uint64_t> checkpoint_count{0};
  mutable std::atomic<uint64_t> total_wait_us{0};
  // Strides maybe_reorganize_if_needed()'s wal_over_threshold() sampling: that check is an
  // fstat(), too expensive to pay on every write.
  mutable std::atomic<uint64_t> wal_check_counter{0};
#define VMEMKV_DECLARE_STAT_ATOMIC(name) mutable std::atomic<uint64_t> name##_{0};
  VMEMKV_CHECKPOINT_STATS_FIELDS(VMEMKV_DECLARE_STAT_ATOMIC)
#undef VMEMKV_DECLARE_STAT_ATOMIC
};

// Phase timings for one checkpoint_internal() cycle. Field names match
// VMEMKV_CHECKPOINT_STATS_FIELDS so publish/snapshot stay uniform. Durations in
// microseconds, byte counts in bytes. Only touched single-flight, so plain values suffice.
struct CheckpointTimings {
  VMEMKV_CHECKPOINT_STATS_FIELDS(VMEMKV_DECLARE_STAT_FIELD)
};

inline void publish_checkpoint_stats(ReorgState &state, const CheckpointTimings &timings) {
#define VMEMKV_PUBLISH_ONE(name) state.name##_.store(timings.name, std::memory_order_relaxed);
  VMEMKV_CHECKPOINT_STATS_FIELDS(VMEMKV_PUBLISH_ONE)
#undef VMEMKV_PUBLISH_ONE
}

inline void snapshot_checkpoint_stats(const ReorgState &state, VMemKVStatistics &out) {
#define VMEMKV_SNAPSHOT_ONE(name) out.name = state.name##_.load(std::memory_order_relaxed);
  VMEMKV_CHECKPOINT_STATS_FIELDS(VMEMKV_SNAPSHOT_ONE)
#undef VMEMKV_SNAPSHOT_ONE
}

namespace checkpoint_detail {

inline auto auto_reorg_suppressed() noexcept -> bool {
  return std::getenv("VMEMKV_SUPPRESS_AUTO_REORG") != nullptr;
}

inline auto wal_over_threshold(const Wal &wal) -> bool {
  return wal.size_bytes() >= WalConfig::kMaxBytesSinceCheckpoint;
}

inline void maybe_reorganize_if_needed(ReorgState &state, const Wal &wal) {
  // Sampled once every kWalCheckStride writes: the resulting delay past the intended byte
  // threshold is bounded by kWalCheckStride writes' worth of bytes, a rounding error against
  // the threshold itself.
  constexpr uint64_t kWalCheckStride = 64;
  if (state.wal_check_counter.fetch_add(1, std::memory_order_relaxed) % kWalCheckStride == 0) {
    if (wal_over_threshold(wal)) {
      state.requested.store(true, std::memory_order_release);
    }
  }
}

// Bounded poll, not an unconditional atomic::wait(): std::atomic<bool>::wait/notify's
// real-world guarantee doesn't rule out a missed wakeup, and this has no timed overload to
// bound it directly. Callers only reach this while another cycle is already in flight, so
// the wait is inherently on the order of a reorganize's own duration already.
//
// Only serializes explicit reorganize()/checkpoint() callers against a concurrently running
// cycle; insert/update/delete never call this.
inline void wait_until_reorg_not_running(const ReorgState &state) {
  constexpr auto kIdlePollInterval = kDefaultPoll10ms;
  const auto wait_start = std::chrono::steady_clock::now();
  while (state.running.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(kIdlePollInterval);
  }
  // Total wall-clock time explicit reorganize()/checkpoint() callers spent here waiting for a
  // concurrent cycle to finish, summed across all callers -- as opposed to checkpoint's own
  // wall-clock duration (most of which overlaps unblocked writer progress).
  const auto waited_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count();
  state.total_wait_us.fetch_add(static_cast<uint64_t>(waited_us), std::memory_order_relaxed);
}

struct CheckpointTarget {
  uint64_t old_base_boundary = 0;
  uint64_t target = 0;
};

// Briefly stops new appends only -- acquire_write_handle() (not a plain get_memory()) gates
// appends; update_value_at()'s in-place path is untouched and keeps running throughout.
// Every append already in flight when the stop takes effect holds its T2MemoryHandle for the
// whole reserve-then-write duration, so once stop_writers_and_wait() returns, `target` is
// guaranteed to be a fully-written frontier, never a merely-reserved one. Writers resume
// before this returns -- msync() below runs fully concurrently with new writes.
template <typename PreStopHook>
auto capture_checkpoint_target(T2FlatFile &t2, PreStopHook pre_stop_hook, std::chrono::nanoseconds &stop_phase)
    -> CheckpointTarget {
  CheckpointTarget out;
  out.old_base_boundary = t2.get_memory()->base_boundary.load(std::memory_order_acquire);
  const vmemkv::T2Memory *mem_to_drain = t2.get_memory();
  pre_stop_hook();
  const auto stop_start = std::chrono::steady_clock::now();
  t2.stop_writers_and_wait(mem_to_drain);
  stop_phase = std::chrono::steady_clock::now() - stop_start;
  {
    WriterResumeGuard resume_guard{&t2};
    out.target = t2.get_memory()->bytes_used.load(std::memory_order_acquire);
  }
  return out;
}

// msync()'s addr must be page-aligned; old_base_boundary is only block-aligned. Rounding the
// start down to the containing page and re-syncing that small overlap from the previous cycle
// is harmless -- msync() is idempotent over bytes already durable.
inline void msync_target_range(const T2Memory *mem, uint64_t old_base_boundary, uint64_t target) {
  if (target <= old_base_boundary) {
    return;
  }
  static const uint64_t kPageSize = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
  const uint64_t aligned_start = old_base_boundary - (old_base_boundary % kPageSize);
  const int msync_rc = ::msync(mem->base + aligned_start, target - aligned_start, MS_SYNC);
  if (msync_rc != 0) {
    throw std::system_error(errno, std::generic_category(), "msync t2 checkpoint");
  }
}

// The only place T1 gets published per cycle. No record's payload_bits ever changes here --
// checkpoint never relocates a record, so there is nothing for the offset_mapper to restamp.
// checkpoint_all_shards() visits every shard in ascending key order and streams each shard's
// entries into the checkpoint writer as it goes (never materializing the whole corpus at
// once), then hands back the directory's boundary keys once every shard is done -- written
// as the writer's trailer in finish().
template <typename ConfigT>
auto publish_t1_checkpoint(ShardedT1Index<ConfigT> &t1,
                           const std::filesystem::path &t2_path,
                           uint64_t checkpoint_lsn,
                           uint64_t target) -> std::chrono::nanoseconds {
  using EntrySnapshot = typename ShardedT1Index<ConfigT>::EntrySnapshot;
  auto offset_mapper_fn = [](std::span<EntrySnapshot> /*merged*/) {};
  vmemkv::ShardedT1CheckpointWriter t1_chk_writer(vmemkv::derive_t1_chk_path(t2_path));
  auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) { t1_chk_writer.add_shard(merged); };
  const auto start = std::chrono::steady_clock::now();
  const auto t1_boundaries = t1.checkpoint_all_shards(offset_mapper_fn, chk_writer_fn);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  t1_chk_writer.finish(t1_boundaries);
  vmemkv::write_manifest(vmemkv::derive_manifest_path(t2_path), checkpoint_lsn, target);
  return elapsed;
}

inline auto to_us(std::chrono::nanoseconds duration) -> uint64_t {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

// Durabilizes T2's live tail over the byte range that grew since the last cycle, advances
// base_boundary in place to cover it, and persists the result as a manifest-committed
// checkpoint. See docs/specification/why_vmemkv_does_not_need_undo_log.md for the
// correctness argument this relies on, and low_level_design.md 4.3/5.3 for the contract.
//
// The watermark claim below must happen before msync() reads anything: without it, an
// in-place update racing an offset in that range could be caught mid-write by msync() (torn
// on disk) or land after base_boundary's own publish below, silently reverting a live read
// the moment a base-resident, seqlock-free reader trusts that offset as immutable.
template <typename ConfigT, typename PreStopHook, typename PreFinishHook>
  requires CheckpointHook<PreStopHook> && CheckpointHook<PreFinishHook>
void checkpoint_internal(ReorgState &state,
                         ShardedT1Index<ConfigT> &t1,
                         T2FlatFile &t2,
                         Wal &wal,
                         T2Ownership<ConfigT> &own,
                         const std::filesystem::path &t2_path,
                         PreStopHook pre_stop_hook,
                         PreFinishHook pre_finish_hook) {
  const auto fn_start = std::chrono::steady_clock::now();
  const uint64_t checkpoint_lsn = wal.next_lsn() - 1;

  std::chrono::nanoseconds stop_phase{0};
  const CheckpointTarget ct = capture_checkpoint_target(t2, pre_stop_hook, stop_phase);

  // Claimed before msync() reads anything. Drains any in-place write that had already passed
  // its allow_in_place check against the old (pre-claim) watermark and is still physically
  // writing; without this, such a write could still be in flight when msync() reads this
  // range, or when base_boundary publishes past it below.
  own.claim_watermark(ct.target);
  const auto barrier_start = std::chrono::steady_clock::now();
  own.drain_in_place(ct.target);
  const auto barrier_phase = std::chrono::steady_clock::now() - barrier_start;

  std::chrono::nanoseconds msync_phase{0};
  std::chrono::nanoseconds t1_phase{0};
  try {
    const vmemkv::T2Memory *mem = t2.get_memory();
    const auto msync_start = std::chrono::steady_clock::now();
    msync_target_range(mem, ct.old_base_boundary, ct.target);
    msync_phase = std::chrono::steady_clock::now() - msync_start;

    pre_finish_hook();

    t1_phase = publish_t1_checkpoint(t1, t2_path, checkpoint_lsn, ct.target);
  } catch (...) {
    // Nothing durable actually happened (base_boundary hasn't advanced yet) -- un-claim the
    // range so in-place updates aren't blocked forever for a cycle that never published.
    own.revert_watermark(ct.old_base_boundary);
    throw;
  }

  t2.get_memory()->base_boundary.store(ct.target, std::memory_order_release);
  // No checkpoint_lsn needed here (low_level_design.md 5.5): rotate_segment() only ever
  // retires the generation active during the *previous* cycle, which this cycle's manifest
  // already covers by construction.
  const auto rotate_start = std::chrono::steady_clock::now();
  wal.rotate_segment();
  const auto rotate_phase = std::chrono::steady_clock::now() - rotate_start;

  state.checkpoint_count.fetch_add(1, std::memory_order_relaxed);
  // Published last (after checkpoint_count_ above), so a poller that wakes on checkpoint_count_
  // changing always sees this cycle's own numbers, never a torn mix with the next cycle's.
  CheckpointTimings timings;
  timings.last_checkpoint_duration_us = to_us(std::chrono::steady_clock::now() - fn_start);
  timings.last_checkpoint_msync_duration_us = to_us(msync_phase);
  timings.last_checkpoint_t1_reorganize_duration_us = to_us(t1_phase);
  timings.last_checkpoint_stop_writers_duration_us = to_us(stop_phase);
  timings.last_checkpoint_barrier_drain_duration_us = to_us(barrier_phase);
  timings.last_checkpoint_wal_rotate_duration_us = to_us(rotate_phase);
  timings.last_checkpoint_wal_rotate_leader_wait_us = wal.last_rotate_leader_wait_us();
  timings.last_checkpoint_bytes_synced = ct.target - ct.old_base_boundary;
  timings.last_checkpoint_corpus_bytes = ct.target;
  publish_checkpoint_stats(state, timings);
}

// Merges T1's sorted+append regions and, if `mode` is Checkpoint, also durabilizes T2's live
// data and persists the result as a manifest-committed checkpoint. Called under the running
// CAS guard.
template <typename ConfigT, typename PreStopHook, typename PreFinishHook>
  requires CheckpointHook<PreStopHook> && CheckpointHook<PreFinishHook>
void reorganize_internal(ReorgState &state,
                         ShardedT1Index<ConfigT> &t1,
                         T2FlatFile &t2,
                         Wal &wal,
                         T2Ownership<ConfigT> &own,
                         const std::filesystem::path &t2_path,
                         [[maybe_unused]] bool recovering,
                         ReorgMode mode,
                         PreStopHook pre_stop_hook,
                         PreFinishHook pre_finish_hook) {
  // Checkpointing during WAL replay is unsafe: recovery runs inside wal_.replay()'s callback,
  // and a checkpoint cycle expects to be the sole writer of T1/T2/manifest/WAL state for its
  // duration. Recovery only ever passes ReorgMode::T1Only -- assert rather than silently
  // override, so a future caller bug surfaces instead of being papered over.
  assert((!recovering || mode == ReorgMode::T1Only) &&
         "must not request a checkpoint while recovering -- see recover_from_wal()'s call site");

  switch (mode) {
    case ReorgMode::Checkpoint:
      checkpoint_internal(state, t1, t2, wal, own, t2_path, pre_stop_hook, pre_finish_hook);
      break;
    case ReorgMode::T1Only: {
      // T1-only reorganize (zero I/O): T2 isn't touched, so the mapper leaves every entry's
      // payload untouched. checkpoint_all_shards() with a no-op per-shard writer gives the
      // same synchronous, deterministic "every shard fully merged before this call returns"
      // contract.
      using EntrySnapshot = typename ShardedT1Index<ConfigT>::EntrySnapshot;
      t1.checkpoint_all_shards([](std::span<EntrySnapshot> /*merged*/) {},
                               [](std::span<const EntrySnapshot> /*merged*/) {});
      break;
    }
  }
}

// Shared wait/CAS/run/retry loop for reorganize()/checkpoint(). Checkpoint always forces at
// least one cycle to actually complete even if T1's append region is already empty (a
// checkpoint caller needs a durabilized manifest, not merely an empty append region); T1Only
// skips when there's nothing to merge.
//
// Performs (or waits for a concurrently-running) exactly one cycle, then returns -- never
// loops back to check whether T1's append region has become fully empty. Under sustained
// concurrent writes the append region is essentially never momentarily empty, so a caller
// re-checking it after every completed cycle could win the CAS against itself indefinitely
// and never return.
template <typename ConfigT, typename PreStopHook, typename PreFinishHook>
  requires CheckpointHook<PreStopHook> && CheckpointHook<PreFinishHook>
void run_reorganize(ReorgState &state,
                    ShardedT1Index<ConfigT> &t1,
                    T2FlatFile &t2,
                    Wal &wal,
                    T2Ownership<ConfigT> &own,
                    const std::filesystem::path &t2_path,
                    bool recovering,
                    ReorgMode mode,
                    PreStopHook pre_stop_hook,
                    PreFinishHook pre_finish_hook) {
  const bool force_run = (mode == ReorgMode::Checkpoint);
  while (true) {
    wait_until_reorg_not_running(state);

    if (!force_run && t1.append_size() == 0) {
      return;
    }

    if (auto guard = SingleFlightGuard::try_acquire(state.running); guard.holds) {
      reorganize_internal(state, t1, t2, wal, own, t2_path, recovering, mode, pre_stop_hook, pre_finish_hook);
      return;
    }
    // Lost the race: someone else is already running a cycle. Loop back and wait for it, then
    // try again -- needed for force_run=true, since the winner's own cycle might not be ours
    // (e.g. we wanted checkpoint() but a plain reorganize() won the race).
  }
}

}  // namespace checkpoint_detail
}  // namespace vmemkv
