// vmemkv_impl.hpp - VMemKV coordinator implementation: routes reads/writes across T1Index and
// T2FlatFile, and owns checkpoint/reorganize orchestration.
//
// ─── CONCURRENCY SPECIFICATION & MATRIX ──────────────────────────────────────
//
// The VMemKV architecture enforces thread-safety at the VMemKVImpl level,
// coordinating and routing operations across two underlying structural layers:
// 1. T1Index (In-memory Index)
// 2. T2FlatFile (Binary Disk Log File)
//
// +--------------------+-------------------+---------------------------------------------------+
// | Component          | Read Operations   | Write Operations (Insert/Update/Delete)           |
// +--------------------+-------------------+---------------------------------------------------+
// | vmemkv::T1Index    | Thread-Safe       | Thread-Unsafe (relies on VMemKVImpl serialization)|
// | vmemkv::T2FlatFile  | Thread-Safe       | Thread-Unsafe (relies on VMemKVImpl serialization)|
// | vmemkv::VMemKVStore | Thread-Safe       | Thread-Safe (fully serialized/coordinated)        |
// +--------------------+-------------------+---------------------------------------------------+
//
// Below is the Concurrency Matrix detailing synchronization across macro operations:
//
// +--------------------------+------------+--------------------+---------------------+-----------------------------+
// | Operation A \ Operation B | Get / Scan | Write (Same Key)   | Write (Diff Key)    | Reorganize                  |
// +--------------------------+------------+--------------------+---------------------+-----------------------------+
// | Get / Scan               | Concurrent | Concurrent         | Concurrent          | Concurrent                  |
// | Write (Same Key)         | Concurrent | Serial (Stripes)   | Concurrent          | Concurrent                  |
// | Write (Diff Key)         | Concurrent | Concurrent         | Concurrent (atomic) | Concurrent                  |
// | Reorganize               | Concurrent | Concurrent         | Concurrent          | Bypassed (reorg_in_progress)|
// +--------------------------+------------+--------------------+---------------------+-----------------------------+
//

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vmemkv/config.hpp>

#include "checkpoint/checkpoint.hpp"
#include "core/reference_tracker.hpp"
#include "t1_index/t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"
#include "wal/wal.hpp"

inline constexpr std::size_t kCacheLineAlignment = 64;

struct alignas(kCacheLineAlignment) AlignedMutex {
  std::mutex mu;
  std::atomic<uint64_t> live_count{0};
  std::atomic<uint64_t> delete_count{0};
};

namespace vmemkv {

// AtFunc: () -> T2RecordView, called fresh on every retry attempt. Required because
// T2FlatFile::at() reads key_len/value_len unsynchronized to size the key/value spans; a view
// built once and reused across retries can carry a stale span size that the version check
// afterward can't catch, since the size was already wrong before the loop started. See
// tests/test_t2_flat_file.cpp for a deterministic regression demonstrating the resulting hang.
//
// CopyFunc's plain (non-atomic) reads of a record's key/value bytes race, in the C++
// abstract-machine sense, with T2FlatFile::update_value_at()'s plain memcpy of the same bytes --
// ThreadSanitizer reports this. It's benign by construction: the version check below discards any
// read that overlapped a concurrent write, so a torn read here is never actually used, only
// retried (same principle as a Linux kernel seqlock). Verified empirically, not just assumed:
// sustained concurrent stress at 64KB values (widening the memcpy race window well past this
// project's usual 200B test size) found zero torn reads across many runs, plain and under this
// same sanitizer. See tsan_suppressions.txt for the suppression and the full verification note.
template <typename AtFunc, typename CopyFunc>
inline auto read_t2_record_seqlock(AtFunc &&at_func, CopyFunc &&copy_func) {
  while (true) {
    const T2RecordView record = at_func();
    auto atomic_version = std::atomic_ref<const uint64_t>(record.header->version);
    uint64_t v1 = atomic_version.load(std::memory_order_acquire);
    if (v1 % 2 != 0) {
      std::this_thread::yield();
      continue;
    }

    auto result = copy_func(record);

    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t v2 = atomic_version.load(std::memory_order_acquire);
    if (v1 == v2) {
      return result;
    }
  }
}

// Test-only seam: fires once inside checkpoint_internal(), right before
// T2FlatFile::stop_writers_and_wait() is called (i.e. while writes are still handed out normally
// by acquire_write_handle()). Lets a test deterministically get a writer's T2MemoryHandle
// registered *before* the stop flag goes up, so the subsequent stop-and-wait has a real,
// still-in-flight writer to wait for -- exercising the exact handshake that closes the
// residual-window race (see T2FlatFile::stop_writers_and_wait()'s declaration). No-op in
// production.
struct NoOpPreStopHook {
  void operator()() const noexcept {}
};

// Test-only seam: fires once inside checkpoint_internal(), strictly before T1 is ever touched
// (T1 is only published once, later, from a single I/O-free t1_.reorganize() call). Lets a test
// throw here to verify that any failure up to and including this point leaves T1 completely
// untouched -- capture_watermark_ is the only state a caught exception needs to roll back (see
// checkpoint_internal()'s own try/catch). No-op in production.
struct NoOpPreFinishHook {
  void operator()() const {}
};

// Contract: once wait_until_retired(boundary) returns, no in-place T2 update whose target offset
// is < boundary is still in flight -- it has either fully completed its write or not yet started.
// Registration tracks the actual offset (not a generic epoch counter), so the contract reads
// literally rather than needing translation at each call site. This only drains writes already in
// flight; the caller is responsible for separately ensuring no *new* one can start below
// `boundary` (checkpoint_internal() uses capture_watermark_ for that) before relying on the
// result, or a fresh enter() could race back in immediately after this returns.
class InPlaceUpdateBarrier {
 public:
  using Guard = typename vmemkv::ThreadReferenceTracker<uint64_t>::Guard;

  // Registers the calling thread as about to attempt an in-place write targeting `offset`. Hold
  // the returned guard for exactly the duration of that attempt (through its
  // T2FlatFile::update_value_at() call, success or failure) -- release it as soon as the attempt
  // is decided, including on every early-return path that doesn't end up writing.
  [[nodiscard]] auto enter(uint64_t offset) const noexcept -> Guard {
    // +1: the tracker's "not registered" sentinel is T{} == 0, which offset 0 itself would
    // otherwise collide with (see ThreadReferenceTracker::wait_until_epoch()'s T{} check).
    return Guard(offsets_, offset + 1);
  }

  void wait_until_retired(uint64_t boundary) const noexcept { offsets_.wait_until_epoch(boundary + 1); }

 private:
  mutable vmemkv::ThreadReferenceTracker<uint64_t> offsets_;
};

// ─── VMemKVImpl Layer Coordination Overview ──────────────────────────────────
//
//                     [ Public KVStore Interface (StoreAdapter) ]
//                                         |
//                                         v
//                                 [ VMemKVImpl ]
//                                    |        |
//                                    v        v
//         [ T1Index (In-Memory Index) ]       [ T2FlatFile (Binary Disk File) ]
//         Stores Key -> Payload (offset/val)  Stores actual Variable-length Value
//
// ─── Operation Routing ────────────────────────────────────────────────────────
// * get(key)     : Look up Payload in T1. If inline, decode. If offset, resolve T2.
// * insert(key)  : Write value to T2 -> Get Offset -> Insert (Key, Offset) into T1.
// * reorganize() : Merges T1's append region into its sorted region. Zero I/O, T2 untouched.
// * checkpoint() : Durabilizes T2's live tail in place (msync, no relocation) and persists a
//                  manifest-committed checkpoint.
//
template <typename ConfigT = vmemkv::Config<>>
class VMemKVImpl {
 public:
  static constexpr bool kIsEnabled = true;
  using ConfigType = ConfigT;

  static auto name() -> std::string {
    std::vector<std::string> parts;
    if constexpr (ConfigT::UseBloomFilter) {
      parts.emplace_back("Bloom");
    }
    if constexpr (ConfigT::UseSimdScan) {
      parts.emplace_back("Simd");
    }

    if constexpr (ConfigT::UseT1InlineValue) {
      parts.emplace_back("T1InlineValue");
    }
    if constexpr (ConfigT::UseGetPopulateRead) {
      parts.emplace_back("GetPopulateRead");
    }

    if (parts.empty()) {
      return "VMemKV/Baseline";
    }

    std::string result = "VMemKV";
    for (const auto &part : parts) {
      result += "/" + part;
    }
    return result;
  }

  using T1IndexT = vmemkv::T1Index<ConfigT>;

  // T2's live mmap is MAP_SHARED (T2FlatFile's constructor); on-disk bytes below the manifest's
  // committed boundary are trustworthy, everything above it is not (low_level_design.md 5.1/5.3).
  // T1/T2 are wiped and rebuilt by replaying the WAL, unless a committed checkpoint (manifest) is
  // found, in which case T2FlatFile adopts its data file directly and T1 is fast-loaded from the
  // paired T1 checkpoint file, leaving only the rotated-down WAL tail to replay.
  VMemKVImpl(const std::filesystem::path &t2_path, uint64_t t2_bytes_capacity)
      : t2_(t2_path, t2_bytes_capacity, adopted_t2_bytes_used(t2_path)), wal_(vmemkv::derive_wal_path(t2_path)) {
    recovering_ = true;
    load_checkpoint_if_present(t2_path);
    recover_from_wal();
    recovering_ = false;
    reorg_worker_ = std::jthread(&VMemKVImpl::reorg_worker_loop, this);  // Started after recovery completes.
  }

  ~VMemKVImpl() noexcept {
    reorg_worker_.request_stop();
    reorg_requested_.store(true, std::memory_order_release);
    if (reorg_worker_.joinable()) {
      reorg_worker_.join();
    }
  }

  static constexpr uint64_t kSizeEmbeddingShift = 48;
  static constexpr uint64_t kOffsetMask = (1ULL << kSizeEmbeddingShift) - 1;
  static constexpr uint64_t kBlockAlignment = 16;

  // T1Only: in-memory merge, zero I/O, T2 untouched. Checkpoint: durabilizes T2's live tail
  // in-place (checkpoint_internal()) -- no record ever moves.
  enum class ReorgMode { T1Only, Checkpoint };

  VMemKVImpl(const VMemKVImpl &) = delete;
  auto operator=(const VMemKVImpl &) -> VMemKVImpl & = delete;
  VMemKVImpl(VMemKVImpl &&) = delete;
  auto operator=(VMemKVImpl &&) -> VMemKVImpl & = delete;

  // Merges T1's sorted+append regions and, if `mode` is Checkpoint, also durabilizes T2's live
  // data and persists the result as a manifest-committed checkpoint (low_level_design.md 5.2,
  // 5.6). Called under reorg_running_'s CAS guard (see reorganize()).
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void reorganize_internal(ReorgMode mode,
                           PreStopHook pre_stop_hook = PreStopHook{},
                           PreFinishHook pre_finish_hook = PreFinishHook{}) {
    // Checkpointing during WAL replay is unsafe: recover_from_wal() runs inside wal_.replay()'s
    // callback, and a checkpoint cycle expects to be the sole writer of T1/T2/manifest/WAL state
    // for its duration -- recursing into one mid-replay would let it observe a T1/T2 still being
    // reconstructed and publish a manifest against that incomplete state. recover_from_wal() is
    // the only caller that can run while recovering_ is true, and it always passes
    // ReorgMode::T1Only explicitly -- assert rather than silently override, so a future caller bug
    // surfaces instead of being papered over.
    assert((!recovering_ || mode == ReorgMode::T1Only) &&
           "must not request a checkpoint while recovering_ -- see recover_from_wal()'s call site");

    switch (mode) {
      case ReorgMode::Checkpoint:
        checkpoint_internal(pre_stop_hook, pre_finish_hook);
        break;
      case ReorgMode::T1Only:
        // T1-only reorganize (zero I/O): T2 isn't touched, so the mapper leaves every entry's
        // payload untouched.
        t1_.reorganize([](std::span<typename T1IndexT::EntrySnapshot> /*merged*/) {},
                       typename T1IndexT::NoOpChkWriter{});
        reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
        reset_tombstone_counters();
        break;
    }
    scan_active_.store(false, std::memory_order_relaxed);
  }

 private:
  // checkpoint_internal() briefly stops writers (T2FlatFile::stop_writers_and_wait()) while
  // finishing a cycle and must resume them on every exit path, including exceptions thrown
  // partway through.
  struct WriterResumeGuard {
    vmemkv::T2FlatFile *t2;
    ~WriterResumeGuard() { t2->resume_writers(); }
  };

  // Durabilizes T2's live tail via `msync()` over the byte range that grew since the last cycle,
  // advances base_boundary in place to cover it, and persists the result as a manifest-committed
  // checkpoint. T2's live mmap is MAP_SHARED (see T2FlatFile::map_file()'s comment): an in-place
  // update lands directly in the page cache, so this function never reads or copies a record's
  // bytes anywhere -- the durable copy and the live copy are always the same bytes. See
  // docs/specification/why_vmemkv_does_not_need_undo_log.md for the correctness argument this
  // relies on, and low_level_design.md 4.3/5.3 for the full contract.
  //
  // capture_watermark_ claims [old_base_boundary, target) below, before msync() reads it --
  // without this, an in-place update racing an offset in that range could be caught mid-write by
  // msync() (torn on disk) or land after base_boundary's own publish below, silently reverting a
  // live read the moment a base-resident, seqlock-free reader trusts that offset as immutable.
  // try_in_place_update()'s allow_in_place check guards against exactly this (see that function's
  // own comment and the crash-recovery regression test for this) -- msync() durabilizes the whole
  // [old_base_boundary, target) range in one shot, so the claim must cover that whole range at
  // once, not just the single record a per-record write would touch.
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void checkpoint_internal(PreStopHook pre_stop_hook = PreStopHook{}, PreFinishHook pre_finish_hook = PreFinishHook{}) {
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    // Phase-breakdown timing (see VMemKVStatistics::last_checkpoint_*) -- measures where
    // checkpoint_internal()'s wall-clock cost actually goes: msync() (should scale with the
    // synced delta, i.e. with trigger frequency) vs t1_.reorganize() (always O(total corpus),
    // independent of trigger frequency). Only touched here, single-flight via reorg_running_, so
    // plain local variables suffice; published to the atomics below once, at the very end.
    const auto fn_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration msync_duration{0};
    std::chrono::steady_clock::duration t1_reorganize_duration{0};
    std::chrono::steady_clock::duration stop_writers_duration{0};
    std::chrono::steady_clock::duration barrier_drain_duration{0};
    std::chrono::steady_clock::duration wal_rotate_duration{0};
    const uint64_t checkpoint_lsn = wal_.next_lsn() - 1;
    const uint64_t old_base_boundary = t2_.get_memory()->base_boundary.load(std::memory_order_acquire);

    // Briefly stops new appends only -- acquire_write_handle() (not a plain get_memory_handle())
    // gates append_default()'s callers; update_value_at()'s in-place path is untouched and keeps
    // running throughout. Every append already in flight when the stop takes effect holds its
    // T2MemoryHandle for the whole reserve-then-write duration (see write_entry_lockfree()'s own
    // comment), so once stop_writers_and_wait() returns, `target` below is guaranteed to be a
    // fully-written frontier, never a merely-reserved one.
    const vmemkv::T2Memory *mem_to_drain = t2_.get_memory();
    // TEST-ONLY: lets a test register a writer's handle to mem_to_drain before the stop flag goes
    // up. No-op in production -- see NoOpPreStopHook.
    pre_stop_hook();
    {
      const auto stop_start = std::chrono::steady_clock::now();
      t2_.stop_writers_and_wait(mem_to_drain);
      stop_writers_duration = std::chrono::steady_clock::now() - stop_start;
    }
    uint64_t target = old_base_boundary;
    {
      WriterResumeGuard resume_guard{&t2_};
      target = t2_.get_memory()->bytes_used.load(std::memory_order_acquire);
    }  // Writers resumed here -- msync() below runs fully concurrently with new writes.

    // Claimed before msync() reads anything -- see this function's own doc comment above. seq_cst:
    // paired with in_place_update_barrier_'s own seq_cst registration/scan and
    // try_in_place_update()'s seq_cst read of this same field below -- see
    // ThreadReferenceTracker::acquire()'s comment for the independent-atomics race this closes
    // (capture_watermark_ and the barrier's slot array are two separate atomics touched by both
    // sides, same shape as the writer_stop_/slot-registration hazard that comment describes).
    capture_watermark_.store(target, std::memory_order_seq_cst);
    // Drains any in-place write that had already passed its allow_in_place check against the old
    // (pre-claim) capture_watermark_ and is still physically writing -- see
    // InPlaceUpdateBarrier's own contract and try_in_place_update()'s enter() call. Without this,
    // such a write could still be in flight when msync() reads this range, or when base_boundary
    // publishes past it below.
    {
      const auto barrier_start = std::chrono::steady_clock::now();
      in_place_update_barrier_.wait_until_retired(target);
      barrier_drain_duration = std::chrono::steady_clock::now() - barrier_start;
    }

    try {
      if (target > old_base_boundary) {
        // msync()'s addr must be page-aligned; old_base_boundary is only kBlockAlignment-aligned.
        // Rounding the start down to the containing page and re-syncing that small overlap from
        // the previous cycle is harmless -- msync() is idempotent over bytes already durable.
        static const uint64_t kPageSize = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        const uint64_t aligned_start = old_base_boundary - (old_base_boundary % kPageSize);
        const vmemkv::T2Memory *mem = t2_.get_memory();
        const auto msync_start = std::chrono::steady_clock::now();
        const int msync_rc = ::msync(mem->base + aligned_start, target - aligned_start, MS_SYNC);
        msync_duration = std::chrono::steady_clock::now() - msync_start;
        if (msync_rc != 0) {
          throw std::system_error(errno, std::generic_category(), "msync t2 checkpoint");
        }
      }

      // TEST-ONLY: lets a test throw here, strictly before T1 is ever touched. No-op in
      // production -- see NoOpPreFinishHook.
      pre_finish_hook();

      // The only place T1 gets published this cycle. No record's payload_bits ever changes here --
      // checkpoint never relocates a record, so there is nothing for the offset_mapper to
      // restamp; T1's own reorganize() still merges append_region into sorted_region and writes
      // the T1 checkpoint file (5.4 節) regardless.
      auto offset_mapper_fn = [](std::span<EntrySnapshot> /*merged*/) {};
      auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) {
        vmemkv::write_t1_checkpoint(vmemkv::derive_t1_chk_path(t2_path()), merged);
      };
      const auto t1_reorganize_start = std::chrono::steady_clock::now();
      t1_.reorganize(offset_mapper_fn, chk_writer_fn);
      t1_reorganize_duration = std::chrono::steady_clock::now() - t1_reorganize_start;

      vmemkv::write_manifest(vmemkv::derive_manifest_path(t2_path()), checkpoint_lsn, target);
    } catch (...) {
      // Nothing durable actually happened (base_boundary hasn't advanced yet) -- un-claim the
      // range so in-place updates aren't blocked forever for a cycle that never published.
      // seq_cst for the same reason as the claim above -- keeps this field's ordering uniform
      // rather than requiring a separate argument for why a lowering store is exempt.
      capture_watermark_.store(old_base_boundary, std::memory_order_seq_cst);
      throw;
    }

    t2_.get_memory()->base_boundary.store(target, std::memory_order_release);
    // No checkpoint_lsn needed here (low_level_design.md 5.5): rotate_segment() only ever retires
    // the generation active during the *previous* cycle, which this cycle's manifest already
    // covers by construction -- see that function's own doc comment for the retention argument.
    {
      const auto rotate_start = std::chrono::steady_clock::now();
      wal_.rotate_segment();
      wal_rotate_duration = std::chrono::steady_clock::now() - rotate_start;
    }

    reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
    checkpoint_count_.fetch_add(1, std::memory_order_relaxed);
    reset_tombstone_counters();
    // Published last (after checkpoint_count_ above), so a poller that wakes on checkpoint_count_
    // changing always sees this cycle's own numbers, never a torn mix with the next cycle's.
    {
      const auto total_us =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - fn_start).count();
      last_checkpoint_duration_us_.store(static_cast<uint64_t>(total_us), std::memory_order_relaxed);
      last_checkpoint_msync_duration_us_.store(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(msync_duration).count()),
          std::memory_order_relaxed);
      last_checkpoint_t1_reorganize_duration_us_.store(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1_reorganize_duration).count()),
          std::memory_order_relaxed);
      last_checkpoint_stop_writers_duration_us_.store(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(stop_writers_duration).count()),
          std::memory_order_relaxed);
      last_checkpoint_barrier_drain_duration_us_.store(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(barrier_drain_duration).count()),
          std::memory_order_relaxed);
      last_checkpoint_wal_rotate_duration_us_.store(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(wal_rotate_duration).count()),
          std::memory_order_relaxed);
      last_checkpoint_wal_rotate_leader_wait_us_.store(wal_.last_rotate_leader_wait_us(), std::memory_order_relaxed);
      last_checkpoint_bytes_synced_.store(target - old_base_boundary, std::memory_order_relaxed);
      last_checkpoint_corpus_bytes_.store(target, std::memory_order_relaxed);
    }
  }

  // update_impl()'s three possible outcomes for a non-inline entry: Aborted means update_impl()
  // itself must return false immediately (the key vanished, or T2FlatFile::update_value_at()
  // failed); FellThrough means the caller must fall back to write_entry_lockfree() (append-region
  // path); Applied means the in-place write already happened and `updated`/`pending` carry its
  // result.
  enum class InPlaceOutcome { Aborted, FellThrough, Applied };
  struct InPlaceUpdateResult {
    InPlaceOutcome outcome;
    Wal::PendingRecord *pending = nullptr;
  };

  // update_impl()'s in-place-update decision for a non-inline entry: either applies the update in
  // place or conclusively decides it must fall through to write_entry_lockfree().
  auto try_in_place_update(std::span<const std::byte> full_key,
                           std::span<const std::byte> value) -> InPlaceUpdateResult {
    const auto res = t1_.get_with_hash(full_key);
    if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
      return {InPlaceOutcome::Aborted};
    }
    if (t1_detail::is_inline(res.raw_hash)) {
      return {InPlaceOutcome::FellThrough};  // Inline entry -- fall through to write_entry_lockfree().
    }

    const T2Memory *mem = t2_.get_memory_handle();

    // Read key/alloc_len fresh under the seqlock -- see read_t2_record_seqlock()'s comment for
    // why an unprotected t2_.at() call isn't safe here.
    bool key_matches = false;
    uint32_t alloc_len = 0;
    read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(res.payload_bits & kOffsetMask, mem); },
                           [&](const T2RecordView &record) -> bool {
                             key_matches = byte_span_equal(record.key, full_key);
                             alloc_len = record.header->alloc_len;
                             return true;
                           });
    const uint64_t offset = res.payload_bits & kOffsetMask;
    // Registered *before* reading capture_watermark_/base_boundary below, and held through the
    // write: whichever of this registration or checkpoint_internal()'s claim+wait happens
    // first, the other side sees it. If the claim lands first, allow_in_place below already
    // observes the raised capture_watermark_ and refuses to write. If this registration lands
    // first, checkpoint_internal()'s in_place_update_barrier_.wait_until_retired() call blocks
    // until this guard releases -- so base_boundary can never publish past an offset this write
    // is still touching. See InPlaceUpdateBarrier's own contract.
    const auto write_guard = in_place_update_barrier_.enter(offset);

    // T2's base region is read through its own seqlock-free mmaps (see
    // T2Memory::base_boundary's comment), which is only safe if base offsets never change after
    // being written -- so an in-place update targeting the base is redirected out-of-place
    // (falls through to write_entry_lockfree()) instead. checkpoint_internal() advances
    // base_boundary in place on this same T2Memory instance (see that field's own declaration),
    // so this read is against a value that only ever grows, never regresses.
    //
    // capture_watermark_ extends the same redirect to a range checkpoint_internal() has already
    // claimed *this cycle*, before base_boundary itself has advanced to cover it (see that
    // member's own comment) -- without this, an in-place update landing between the claim and
    // the cycle's actual durabilizing read of this offset could mutate bytes it's about to (or
    // already did) capture: torn on disk if msync() catches it mid-write, or silently reverted
    // live once base_boundary publishes past it. seq_cst: paired with checkpoint_internal()'s
    // seq_cst store to this same field and with in_place_update_barrier_'s own seq_cst
    // registration/scan -- see that store's comment for the independent-atomics race this closes.
    const bool allow_in_place = offset >= mem->base_boundary.load(std::memory_order_acquire) &&
                                offset >= capture_watermark_.load(std::memory_order_seq_cst);
    if (key_matches && value.size() <= alloc_len && allow_in_place) {
      if (!t2_.update_value_at(offset, value, mem)) {
        return {InPlaceOutcome::Aborted};
      }
      return {InPlaceOutcome::Applied, wal_.reserve_update(full_key, value)};
    }
    return {InPlaceOutcome::FellThrough};  // Base-resident, doesn't fit alloc_len, or key mismatch.
  }

  // Bounded poll, not an unconditional atomic::wait(): same rationale as reorg_worker_loop()'s
  // idle wait (see its comment) -- std::atomic<bool>::wait/notify's real-world guarantee doesn't
  // rule out a missed wakeup, and this has no timed overload to bound it directly. Both call
  // sites below only reach this while an actual reorganize is already in flight (either a manual
  // reorganize()/checkpoint() call found one running, or insert/update/delete hit
  // the hard backpressure limit), so the wait is inherently on the order of a reorganize's own
  // duration (milliseconds to seconds) already -- kIdlePollInterval's latency is not perceptible
  // against that, unlike a genuinely hot per-call path.
  void wait_until_reorg_not_running() const {
    constexpr auto kIdlePollInterval = std::chrono::milliseconds(10);
    const auto wait_start = std::chrono::steady_clock::now();
    while (reorg_running_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(kIdlePollInterval);
    }
    // Total wall-clock time writer threads spend genuinely blocked here, summed across all
    // callers -- the actual QPS cost a checkpoint/reorg cycle imposes on writers, as opposed to
    // checkpoint_internal()'s own wall-clock duration (most of which overlaps unblocked writer
    // progress -- checkpoint_internal() resumes writers via WriterResumeGuard immediately after
    // capturing its target, well before msync()/t1_.reorganize()/wal_.rotate_segment() run).
    const auto waited_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count();
    total_hard_stall_duration_us_.fetch_add(static_cast<uint64_t>(waited_us), std::memory_order_relaxed);
  }

  // Shared wait/CAS/run/retry loop for the two public methods below. `mode` also decides
  // `force_run`: Checkpoint always forces at least one reorganize_internal() call to actually
  // complete even if T1's append region is already empty (e.g. bulk_load()'s periodic internal
  // reorganizes already drained it), since a caller of checkpoint() needs a durabilized manifest,
  // not merely an empty append region; T1Only skips when there's nothing to merge.
  //
  // Performs (or waits for a concurrently-running) exactly one reorganize_internal() cycle, then
  // returns -- never loops back to check whether T1's append region has become fully empty. Under
  // sustained concurrent writes the append region is essentially never momentarily empty, so a
  // caller re-checking it after every completed cycle could win the CAS against itself
  // indefinitely and never return.
  void run_reorganize(ReorgMode mode) {
    const bool force_run = (mode == ReorgMode::Checkpoint);
    while (true) {
      // 1. Wait for any concurrent background/manual reorganize to complete
      wait_until_reorg_not_running();

      // 2. Nothing to do, and no cycle was unconditionally requested.
      if (!force_run && t1_.append_size() == 0) {
        return;
      }

      // 3. Try to acquire the execution lock
      bool expected_running = false;
      if (reorg_running_.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel)) {
        try {
          reorganize_internal(mode);
        } catch (...) {
          reorg_running_.store(false, std::memory_order_release);
          throw;
        }
        reorg_running_.store(false, std::memory_order_release);
        return;
      }
      // Lost the race: someone else (background reorg_worker_, or another concurrent caller) is
      // already running a cycle. Loop back and wait for it, then try again -- needed for
      // force_run=true, since the winner's own cycle might not be ours (e.g. we wanted
      // checkpoint() but a plain reorganize() won the race), so only a cycle *we* ran ourselves
      // satisfies our caller's request. For force_run=false this can also retry, but is bounded
      // by how many cycles other callers actually run, not by our own append_size() target
      // continually moving.
    }
  }

 public:
  // T1-only in-memory merge. Never touches T2, never persists a checkpoint. Safe to call anytime.
  void reorganize() { run_reorganize(ReorgMode::T1Only); }

  // Forces a checkpoint_internal() cycle: durabilizes T2's live tail in place and persists the
  // result as a manifest-committed checkpoint.
  void checkpoint() { run_reorganize(ReorgMode::Checkpoint); }

  // Accessors for T1 (Index) and T2 (Flat File) layers (mainly for testing).
  auto t1() noexcept -> T1IndexT & { return t1_; }
  auto t1() const noexcept -> const T1IndexT & { return t1_; }
  auto t2() noexcept -> vmemkv::T2FlatFile & { return t2_; }
  auto t2() const noexcept -> const vmemkv::T2FlatFile & { return t2_; }

  auto get_statistics() const noexcept -> vmemkv::VMemKVStatistics {
    return vmemkv::VMemKVStatistics{
        .t1_reorg_count = reorg_t1_count_.load(std::memory_order_relaxed),
        .checkpoint_count = checkpoint_count_.load(std::memory_order_relaxed),
        .hard_stall_count = hard_stall_count_.load(std::memory_order_relaxed),
        .last_checkpoint_duration_us = last_checkpoint_duration_us_.load(std::memory_order_relaxed),
        .last_checkpoint_msync_duration_us = last_checkpoint_msync_duration_us_.load(std::memory_order_relaxed),
        .last_checkpoint_t1_reorganize_duration_us =
            last_checkpoint_t1_reorganize_duration_us_.load(std::memory_order_relaxed),
        .last_checkpoint_stop_writers_duration_us =
            last_checkpoint_stop_writers_duration_us_.load(std::memory_order_relaxed),
        .last_checkpoint_barrier_drain_duration_us =
            last_checkpoint_barrier_drain_duration_us_.load(std::memory_order_relaxed),
        .last_checkpoint_wal_rotate_duration_us =
            last_checkpoint_wal_rotate_duration_us_.load(std::memory_order_relaxed),
        .last_checkpoint_wal_rotate_leader_wait_us =
            last_checkpoint_wal_rotate_leader_wait_us_.load(std::memory_order_relaxed),
        .last_checkpoint_bytes_synced = last_checkpoint_bytes_synced_.load(std::memory_order_relaxed),
        .last_checkpoint_corpus_bytes = last_checkpoint_corpus_bytes_.load(std::memory_order_relaxed),
        .total_hard_stall_duration_us = total_hard_stall_duration_us_.load(std::memory_order_relaxed),
        .append_region_live_count = t1_.append_region_live_count(),
        .append_region_peak_count = t1_.append_region_peak_count()};
  }

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  auto write_entry_lockfree(std::span<const std::byte> full_key, std::span<const std::byte> value) -> bool {
    while (true) {
      uint8_t inline_size = 0;
      if (const auto inline_payload = try_make_inline_payload(full_key, value, inline_size)) {
        const auto put_result = t1_.put(full_key, *inline_payload, true, inline_size);
        if (put_result == T1IndexT::PutResult::Applied) {
          return true;
        }
        // AppendRegionFull: fall through and retry from the top.
        maybe_reorganize_if_needed();
        continue;
      }

      // Checked before touching T2 at all (not just asserted post-append): block_count must fit
      // the 16 bits kSizeEmbeddingShift reserves for it in the payload, or it would silently wrap,
      // corrupting the embedded size hint try_read_base_record() uses for its fast-path reads (see
      // that function's own comment) -- release builds have no assert to catch this, and a
      // wrapped hint degrades to the always-correct-but-slower seqlock/pread fallback rather than
      // returning wrong data, but that's not a contract worth leaving unenforced when
      // T2FlatFile::append_default() already throws on capacity overrun for the same class of
      // "this write cannot be represented" failure.
      uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + full_key.size() + value.size());
      uint64_t block_count = aligned_len / kBlockAlignment;
      if (block_count >= 65536) {
        throw std::runtime_error("Record size exceeds 1.04MB limit");
      }

      // acquire_write_handle() (not a plain get_memory_handle()) defers while checkpoint_internal()
      // has new writers stopped for its target-capture window, and -- critically -- `mem` is held
      // alive across both the T2 append below and the T1 publish attempt, not released in
      // between. That pairing is what closes checkpoint_internal()'s residual-window race: as
      // long as this handle is live, stop_writers_and_wait() can't conclude this write is
      // quiesced, so a straggler entry can never land past the frontier the currently-running
      // checkpoint captures.
      typename T1IndexT::PutResult put_result;
      {
        T2FlatFile::T2MemoryHandle mem = t2_.acquire_write_handle();
        uint64_t offset = vmemkv::T2FlatFile::append_default(mem, full_key, value);
        uint64_t encoded_payload = offset | (block_count << kSizeEmbeddingShift);
        put_result = t1_.put(full_key, encoded_payload, false, 0);
      }
      // `mem` is released here, strictly before maybe_reorganize_if_needed() below: that call can
      // block this thread waiting for a concurrent reorganize() to finish (hard-threshold
      // backpressure), and that same reorganize() may in turn be blocked waiting for *this*
      // handle to drain -- holding it any longer would deadlock the two threads against each
      // other. Nothing past this point needs `mem` alive: t1_.put() has already returned, so the
      // entry (if Applied) is already visible to any concurrent append_size() check.
      if (put_result == T1IndexT::PutResult::Applied) {
        return true;
      }
      // AppendRegionFull: the appended T2 record becomes unreachable garbage, reclaimed by a
      // future reorganize() once nothing references it -- retry from the top.
      maybe_reorganize_if_needed();
    }
  }

  // Inserts a new key-value pair.
  // - Ordering: T2 write must strictly precede the T1 write, so concurrent readers never see a
  //   dangling offset in T1. The WAL append comes last, after the mutation succeeds: logging
  //   first would durably persist a record for a write that never happened, and replaying it
  //   would hit the same throw on next restart, permanently bricking the store. Since T1/T2 are
  //   volatile and rebuilt from the WAL, a failed op is safe to simply not log.
  // - Thread-safety: guarded by slot-level spinlocks.
  // - WAL durability wait is deliberately outside key_lock: only reserve_insert() (LSN + ring
  //   publish) needs key_lock, to guarantee a later call for the same key gets a strictly later
  //   LSN. The multi-millisecond await_durable() wait doesn't need that ordering, and holding
  //   key_lock through it would needlessly serialize other writers on the same stripe.
  auto insert_impl(std::span<const std::byte> full_key, std::span<const std::byte> value) -> bool {
    bool inserted = false;
    Wal::PendingRecord *pending = nullptr;
    {
      std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

      if (t1_.get(full_key) != vmemkv::STORE_NOT_FOUND) {
        return false;
      }

      if (write_entry_lockfree(full_key, value)) {
        pending = wal_.reserve_insert(full_key, value);
        stripe_state(full_key).live_count.fetch_add(1, std::memory_order_relaxed);
        inserted = true;
      }
    }
    if (pending != nullptr) {
      wal_.await_durable(pending);
    }
    // maybe_reorganize_if_needed() only touches state global to t1_, not this key's stripe lock,
    // so running it after releasing key_lock is correct and shrinks how long the stripe is held
    // (matters under write concurrency). write_entry_lockfree() still calls this itself under
    // key_lock on the AppendRegionFull retry path, so liveness stays unaffected.
    maybe_reorganize_if_needed();
    return inserted;
  }

  // -------------------------------------------------------------------------------------------
  // T2 base-region reads. Once a record's offset is below `base_boundary`, its bytes are
  // immutable forever (update_impl() redirects in-place updates targeting that range
  // out-of-place instead -- see try_in_place_update()'s allow_in_place check), so a reader
  // doesn't need the seqlock protecting the mutable tail and can read straight out of a mapping.
  // Get and Scan want *different* readahead policies for that read though (madvise is a property
  // of the whole mapping, not of one read), so three distinct mappings/handles of the identical
  // bytes exist (`base_mmap_scan_seq`, `base_mmap_scan`, `read_fd` -- see T2FlatFile's constructor
  // for how each is set up) -- which one a given call should use is decided per record (a real
  // corpus isn't guaranteed uniform record sizes even though this project's benchmarks happen to
  // use one size per run) by try_read_base_record()'s switch below. That switch is the single
  // place this decision is made, and callers never choose a mapping themselves.
  // -------------------------------------------------------------------------------------------

  // Builds the (key, value) spans that follow a ValueRecordHeader in memory -- the common tail
  // end of every base-region read path below, once each has independently validated `header`'s
  // bounds against whatever it read the bytes from (base_boundary or a pread()'d buffer's actual
  // length; the checks differ, so callers do them, not this helper).
  static auto make_record_view(const ValueRecordHeader *header) noexcept -> T2RecordView {
    const auto *key_begin = reinterpret_cast<const std::byte *>(header + 1);
    std::span<const std::byte> key(key_begin, header->key_len);
    std::span<const std::byte> value(key.data() + header->key_len, header->value_len);
    return T2RecordView{header, key, value};
  }

  // Blind (no mincore, no fallback) direct read through a given base-region mapping -- the
  // common tail end of every path in try_read_base_record() below.
  auto read_base_record_via(std::byte *mapping,
                            uint64_t offset,
                            uint64_t base_boundary) const -> std::optional<T2RecordView> {
    if (mapping == nullptr) {
      return std::nullopt;
    }
    const std::byte *record_base = mapping + offset;
    const auto *header = reinterpret_cast<const ValueRecordHeader *>(record_base);
    const uint64_t needed = sizeof(ValueRecordHeader) + header->key_len + header->value_len;
    // Should always hold for a record reorganize() actually wrote here -- kept as a
    // defense-in-depth backstop, not a routine path: falls through to the always-correct
    // mmap+seqlock path when it doesn't.
    if (offset + needed > base_boundary) {
      return std::nullopt;
    }
    return make_record_view(header);
  }

  // Warm-path half of BaseReader::kGet's large-record case below: if `[offset, offset+read_len)`
  // in `base_mmap_scan` is entirely page-cache resident (checked via mincore(), which -- unlike
  // an actual read -- never blocks on a fault itself), reads a live span straight out of the
  // mmap for free (no syscall, no copy), exactly matching what a plain mmap-based Get would cost.
  // Returns std::nullopt on any doubt (mincore()
  // unavailable/failed, or any page not resident) -- caller falls back to a bounded pread()
  // instead of risking a page-fault-driven block here. A page could in theory be evicted between
  // this check and the caller reading through the returned span (base_mmap_scan isn't pinned),
  // but base-region bytes are immutable, so that only costs an ordinary page fault on the read --
  // never wrong data.
  auto try_read_resident_base_record(std::byte *mapping,
                                     uint64_t offset,
                                     uint64_t read_len,
                                     uint64_t base_boundary) const -> std::optional<T2RecordView> {
    constexpr uintptr_t kPageSize = 4096;
    constexpr uintptr_t kPageMask = kPageSize - 1;
    std::byte *const record_base = mapping + offset;
    const auto start = reinterpret_cast<uintptr_t>(record_base);
    const auto aligned_start = start & ~kPageMask;
    const auto aligned_len = ((start + read_len + kPageMask) & ~kPageMask) - aligned_start;

    thread_local static std::vector<unsigned char> tl_mincore_vec;
    tl_mincore_vec.resize(aligned_len / kPageSize);
    if (::mincore(reinterpret_cast<void *>(aligned_start), aligned_len, tl_mincore_vec.data()) != 0) {
      return std::nullopt;
    }
    for (unsigned char page_status : tl_mincore_vec) {
      if ((page_status & 1) == 0) {
        return std::nullopt;  // Not resident -- let the caller's pread() fetch it instead.
      }
    }

    const auto *header = reinterpret_cast<const ValueRecordHeader *>(record_base);
    const uint64_t needed = sizeof(ValueRecordHeader) + header->key_len + header->value_len;
    // Defense-in-depth, same role as read_base_record_via()'s identical check.
    if (offset + needed > base_boundary) {
      return std::nullopt;
    }
    return make_record_view(header);
  }

  // Which caller is asking try_read_base_record() below for a base-region record. The only
  // thing that determines the mapping/strategy choice in that function's switch.
  enum class BaseReader : uint8_t {
    kGet,   // One record per call, effectively random access -- never wants readahead.
    kScan,  // ~100 records per call in roughly ascending offset order -- benefits from it.
  };

  // Single entry point for every T2 base-region read. Computes the shared preamble (offset,
  // base_boundary check, embedded size hint) once, then the switch below picks the mapping and
  // read strategy for the given reader -- see each case for why it picks what it does.
  //
  // Deliberately ONE function with the reader/size decision laid out explicitly in one switch,
  // not two similar functions that happen to share a helper: Get and Scan need genuinely
  // different mapping policies (see each BaseReader case below for why), so changing a reader's
  // policy, or adding a new reader, only ever touches this one switch, not two functions that
  // would otherwise need to be kept in sync by hand.
  //
  // `payload_bits` (not pre-masked to an offset) is required so the T1 index's embedded
  // block-count size hint (kSizeEmbeddingShift) can size the read -- see the margin comment
  // below for why that hint alone isn't quite enough. `cold_buf`: only used by BaseReader::kGet's
  // large, non-resident case (a bounded pread() destination, resized as needed; caller must keep
  // it alive as long as the returned view is used) -- pass nullptr for BaseReader::kScan, which
  // never needs it. Returns std::nullopt whenever this fast path isn't available (offset still
  // in the mutable tail, a mapping/fd wasn't created, mincore()/pread() failed, or a
  // defense-in-depth bounds check fails) -- callers fall back to t2_.at() +
  // read_t2_record_seqlock() in that case. Picking the "wrong" mapping for a record's size is
  // only ever a readahead-policy mismatch, never incorrect data: every mapping here covers the
  // identical underlying bytes.
  auto try_read_base_record(const T2Memory *mem,
                            uint64_t payload_bits,
                            BaseReader reader,
                            std::vector<std::byte> *cold_buf) const -> std::optional<T2RecordView> {
    const uint64_t offset = payload_bits & kOffsetMask;
    const uint64_t base_boundary = mem->base_boundary.load(std::memory_order_acquire);
    if (offset >= base_boundary) {
      return std::nullopt;
    }

    // The embedded block-count size hint is 16-byte-granular while records are only 8-byte
    // aligned (align_up()), so it can undershoot the true aligned length by up to 8 bytes. A read
    // sized to (hint + margin) covers every record this store can produce, whether resident (mmap
    // path) or not (pread path).
    constexpr uint64_t kSizeHintMargin = 16;
    const uint64_t size_hint = ((payload_bits >> kSizeEmbeddingShift) * kBlockAlignment) + kSizeHintMargin;
    constexpr uint64_t kPageSize = 4096;
    const bool is_small = size_hint <= kPageSize;

    switch (reader) {
      case BaseReader::kScan:
        // Scan reads ~100 records per call in roughly ascending offset order, so unlike Get it
        // benefits from kernel readahead: small records (many faults per call) want
        // MADV_SEQUENTIAL's wider window; large records would have one call's readahead
        // overshoot into unrelated neighboring records, so they use the plain/no-advise mapping
        // instead.
        return read_base_record_via(is_small ? mem->base_mmap_scan_seq : mem->base_mmap_scan, offset, base_boundary);

      case BaseReader::kGet:
        if (is_small) {
          // Get reads one record per call and is always effectively random access, regardless
          // of size -- speculative readahead never pays off for it. `base` (the primary
          // mapping, already MADV_RANDOM everywhere -- see its own doc comment) already carries
          // exactly that policy, so small records read it directly and skip the seqlock too
          // (safe: offset < base_boundary already proves these bytes are immutable). Also skips
          // mincore()/pread(): both cost one syscall regardless of record size, negligible next
          // to a 64KB record's copy but dominating a 1KB one's ~2us *total* cost -- routing this
          // through base_mmap_scan (no madvise, some readahead) costs ~4.4x more kernel time per
          // major fault than `base` does, and through base_mmap_scan_seq (Scan's mapping,
          // MADV_SEQUENTIAL) ~10x more, LTM/1KB Get/Hit/Zipf/threads:32.
          return read_base_record_via(mem->base, offset, base_boundary);
        }
        {
          // Large records: check residency first (mincore(), which never blocks on a fault
          // itself) before committing to a read -- if resident, base_mmap_scan gives a free
          // mmap read costing nothing beyond what a plain mmap-based Get would have paid
          // anyway; if not, one bounded pread() beats the N separate page faults an mmap read
          // of a multi-page cold record would trigger.
          const uint64_t read_len = std::min(size_hint, base_boundary - offset);
          if (mem->base_mmap_scan != nullptr) {
            if (auto resident = try_read_resident_base_record(mem->base_mmap_scan, offset, read_len, base_boundary);
                resident.has_value()) {
              return resident;
            }
          }
          if (mem->read_fd < 0 || cold_buf == nullptr) {
            return std::nullopt;
          }
          cold_buf->resize(read_len);
          const ssize_t bytes_read = ::pread(mem->read_fd, cold_buf->data(), read_len, static_cast<off_t>(offset));
          if (bytes_read < static_cast<ssize_t>(sizeof(ValueRecordHeader))) {
            return std::nullopt;  // Short read or error -- fall back to the always-correct seqlock path.
          }
          const auto *header = reinterpret_cast<const ValueRecordHeader *>(cold_buf->data());
          const uint64_t needed = sizeof(ValueRecordHeader) + header->key_len + header->value_len;
          // Defense-in-depth, same role as read_base_record_via()'s identical check.
          if (needed > static_cast<uint64_t>(bytes_read)) {
            return std::nullopt;
          }
          return make_record_view(header);
        }
    }
    assert(false && "unhandled BaseReader");
    return std::nullopt;
  }

  // Retrieves a value and invokes callback with its raw bytes.
  // - Thread-safety: lock-free, concurrently readable during reorganization.
  // - Concurrency note (canonical explanation; other methods below point here): a T1 lookup names
  //   a T2 offset; that offset's bytes are protected by whichever of two independent mechanisms
  //   applies. Below base_boundary, the offset is immutable forever (see
  //   T2Memory::base_boundary's comment) and try_read_base_record() reads it seqlock-free. At or
  //   above it, the offset can be concurrently in-place-updated, so read_t2_record_seqlock()
  //   below re-checks a version counter around the read and retries on a torn observation.
  //   get_memory_handle() is a plain pointer read: T2 never remaps to a different T2Memory
  //   instance for a store's whole process lifetime, so no reference-counted handle is needed to
  //   keep it alive across the call.
  template <typename Callback>
  auto get_impl(std::span<const std::byte> full_key, Callback callback) const -> bool {
    const auto res = t1_.get_with_hash(full_key);
    if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
      return false;
    }

    if constexpr (ConfigT::UseT1InlineValue) {
      if (t1_detail::is_inline(res.raw_hash)) {
        size_t size = t1_detail::decode_size(res.raw_hash);
        std::array<std::byte, kInlineScalarValueBytes> stack_buf;
        std::memcpy(stack_buf.data(), &res.payload_bits, size);
        callback(std::span<const std::byte>(stack_buf.data(), size));
        return true;
      }
    }

    const T2Memory *mem = t2_.get_memory_handle();
    const uint64_t offset = res.payload_bits & kOffsetMask;

    // Base-region fast path: see try_read_base_record()'s doc comment. Bytes are immutable
    // once written, so callback can safely receive a span straight into the pread'd buffer --
    // no torn-read risk, no extra copy beyond what pread() itself did. thread_local/static for
    // the same reason as tl_get_value_buf below (per-thread reuse, no per-call heap
    // allocation).
    thread_local static std::vector<std::byte> tl_get_base_buf;
    if (const auto base_record = try_read_base_record(mem, res.payload_bits, BaseReader::kGet, &tl_get_base_buf);
        base_record.has_value()) {
      if (byte_span_equal(base_record->key, full_key)) {
        callback(base_record->value);
        return true;
      }
      // Defensive mismatch (should not happen -- try_read_base_record()'s own bounds check
      // already guards against reading garbage): fall through to the always-correct seqlock
      // path below instead of trusting this read.
    }

    // Torn-read fix: copy_func below must only *copy* the record's bytes into an owned buffer
    // and return -- never invoke `callback` from inside it. `record.value` points live into
    // T2Memory::base (MAP_SHARED, concurrently update_value_at()-writable); the seqlock's
    // before/after version check only bounds what happens *around* copy_func's call, not what
    // callback itself might do or how long it might run if invoked from inside that window --
    // calling back from inside would let a concurrent in-place update tear the bytes the
    // caller sees mid-read. thread_local (not a plain local, unlike reorganize_internal()'s
    // offset_mapper which runs single-threaded) since concurrent callers on different threads
    // must not share one buffer; static so repeated calls on the same thread reuse
    // already-grown capacity instead of reallocating.
    thread_local static std::vector<std::byte> tl_get_value_buf;
    bool key_matches = read_t2_record_seqlock(
        [&]() -> T2RecordView { return t2_.at(offset, mem); },
        [&](const T2RecordView &record) -> bool {
          if (!byte_span_equal(record.key, full_key)) {
            return false;
          }
          if constexpr (ConfigT::UseGetPopulateRead) {
            // Prototype: batch-fault the value's full page range with one
            // syscall instead of letting each page fault in one at a time
            // as the copy below touches it -- see GetPopulateRead's doc
            // comment in config.hpp. Below one page this is a no-op
            // (the implicit fault from the copy already covers it in one
            // shot), so only values spanning more than one page pay for it.
            constexpr uintptr_t kPageSize = 4096;
            constexpr uintptr_t kPageMask = kPageSize - 1;
            if (record.value.size() > kPageSize) {
              const auto start = reinterpret_cast<uintptr_t>(record.value.data());
              const auto end = start + record.value.size();
              const auto aligned_start = start & ~kPageMask;
              const auto aligned_len = ((end + kPageMask) & ~kPageMask) - aligned_start;
              ::madvise(reinterpret_cast<void *>(aligned_start), aligned_len, MADV_POPULATE_READ);
            }
          }
          tl_get_value_buf.assign(record.value.begin(), record.value.end());
          return true;
        });

    if (key_matches) {
      callback(std::span<const std::byte>(tl_get_value_buf));
    }
    return key_matches;
  }

  // Updates the value of an existing key.
  // - Ordering: in-place update on T2 if allocation size matches, otherwise appends to T2 first
  //   then updates T1's pointer. WAL append happens only after the mutation applies -- see
  //   insert_impl()'s comment for why logging first is unsafe.
  // - Thread-safety: guarded by key hash locks.
  auto update_impl(std::span<const std::byte> full_key, std::span<const std::byte> value) -> bool {
    bool updated = false;
    // Set when this call goes through write_entry_lockfree() (append-region path), the only case
    // that can push the append region toward its threshold. Checked after releasing key_lock --
    // same rationale as insert_impl()'s maybe_reorganize_if_needed() comment.
    // WAL durability wait is deliberately outside key_lock -- see insert_impl()'s comment.
    bool need_reorg_check = false;
    Wal::PendingRecord *pending = nullptr;
    {
      std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

      // Fast path: an ungated lookup to see whether the *current* value is inline -- if so, this
      // call never touches T2 (write_entry_lockfree() re-decides inline-ness for the new value
      // fresh), so try_in_place_update() below has nothing to protect here.
      const auto quick = t1_.get_with_hash(full_key);
      if (quick.payload_bits == vmemkv::STORE_NOT_FOUND) {
        return false;
      }
      if (t1_detail::is_inline(quick.raw_hash)) {
        need_reorg_check = true;
        if (write_entry_lockfree(full_key, value)) {
          pending = wal_.reserve_update(full_key, value);
          updated = true;
        }
      } else {
        const InPlaceUpdateResult result = try_in_place_update(full_key, value);
        if (result.outcome == InPlaceOutcome::Aborted) {
          return false;
        }
        if (result.outcome == InPlaceOutcome::Applied) {
          pending = result.pending;
          updated = true;
        } else {
          need_reorg_check = true;
          if (write_entry_lockfree(full_key, value)) {
            pending = wal_.reserve_update(full_key, value);
            updated = true;
          }
        }
      }
    }
    if (pending != nullptr) {
      wal_.await_durable(pending);
    }
    if (need_reorg_check) {
      maybe_reorganize_if_needed();
    }
    return updated;
  }

  // Logically removes a key from the store.
  // - Guarantees: Marks the key offset as STORE_NOT_FOUND in T1 (physical space reclamation is deferred to reorganize).
  // - Thread-safety: Thread-safe (guarded by key hash locks).
  // WAL durability wait is deliberately outside key_lock -- see insert_impl()'s comment on the
  // same fix for why only reserve_delete() (not await_durable()) needs to happen under it.
  auto remove_impl(std::span<const std::byte> full_key) -> bool {
    auto &stripe = stripe_state(full_key);
    Wal::PendingRecord *pending = nullptr;
    {
      std::lock_guard<std::mutex> key_lock(stripe.mu);

      const auto res = t1_.get_with_hash(full_key);
      if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
        return false;
      }

      if (t1_.put(full_key, vmemkv::STORE_NOT_FOUND) == T1IndexT::PutResult::Applied) {
        pending = wal_.reserve_delete(full_key);
        stripe.live_count.fetch_sub(1, std::memory_order_relaxed);
        stripe.delete_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        return false;
      }
    }

    wal_.await_durable(pending);
    maybe_reorganize_if_needed_for_delete(stripe);
    return true;
  }

  // Bulk-loads `count` entries via write_entry_lockfree(), bypassing the WAL for higher
  // throughput than individual insert_impl() calls. No durability guarantee: skipping the WAL
  // means a crash after this returns can lose everything loaded, unless the caller separately
  // commits a checkpoint() afterward. Still triggers ordinary T1-only
  // reorganizes via maybe_reorganize_if_needed() once the append region crosses its soft
  // threshold. Not safe to call concurrently with other writers.
  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t count, KeyFn &&make_key, ValueFn &&make_value) {
    for (std::size_t index = 0; index < count; ++index) {
      maybe_reorganize_if_needed();
      const std::string key = make_key(index);
      const std::string value = make_value(index);
      write_entry_lockfree(std::span<const std::byte>(reinterpret_cast<const std::byte *>(key.data()), key.size()),
                           std::span<const std::byte>(reinterpret_cast<const std::byte *>(value.data()), value.size()));
    }
  }

  // Performs a range scan, invoking callback(key, val) for each matching live entry.
  // - Thread-safety: concurrently readable. t1_.scan() takes its own consistent T1 snapshot
  //   internally (epoch-guarded for its whole duration, including this callback), so every key it
  //   hands to the callback below is resolved against a single, self-consistent T1 state.
  template <typename Callback>
  auto scan_impl(std::span<const std::byte> lower_bound,
                 std::span<const std::byte> upper_bound,
                 Callback callback) const -> size_t {
    if (!scan_active_.load(std::memory_order_relaxed)) {
      scan_active_.store(true, std::memory_order_relaxed);
    }

    size_t total_count = 0;

    t1_.scan(lower_bound,
             upper_bound,
             // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
             [&](std::span<const std::byte> index_key, uint64_t payload, uint64_t hash) {
               if (payload == vmemkv::STORE_NOT_FOUND) {
                 return;
               }

               if constexpr (ConfigT::UseT1InlineValue) {
                 if (t1_detail::is_inline(hash)) {
                   size_t size = t1_detail::decode_size(hash);
                   std::array<std::byte, kInlineScalarValueBytes> stack_value;
                   std::memcpy(stack_value.data(), &payload, size);

                   // Trailing-zero-byte trim via bit_width instead of a byte-by-byte loop: index_key
                   // is exactly two uint64_t's worth of bytes, so the last non-zero byte's position
                   // comes directly from whichever half is nonzero, no per-byte branching needed.
                   // Requires little-endian (memcpy'd byte 0 must land in the least-significant
                   // position) -- true for every platform this codebase targets, but asserted here
                   // since it's not obvious from the arithmetic alone. Only unambiguous because
                   // try_make_inline_payload() refuses to inline a key whose own last byte is 0x00
                   // -- see that function's comment for why trimming would otherwise silently
                   // truncate such a key's trailing zero byte(s) along with the real padding.
                   static_assert(kStoreKeyBytes == 2 * sizeof(uint64_t));
                   static_assert(std::endian::native == std::endian::little);
                   uint64_t lo_word;
                   uint64_t hi_word;
                   std::memcpy(&lo_word, index_key.data(), sizeof(lo_word));
                   std::memcpy(&hi_word, index_key.data() + sizeof(lo_word), sizeof(hi_word));
                   const size_t len = hi_word != 0 ? sizeof(lo_word) + (std::bit_width(hi_word) + 7) / 8
                                                   : (std::bit_width(lo_word) + 7) / 8;

                   const std::span<const std::byte> key_view(index_key.data(), len);
                   if (!key_in_range(key_view, lower_bound, upper_bound)) {
                     return;
                   }
                   callback(key_view, std::span<const std::byte>(stack_value.data(), size));
                   ++total_count;
                   return;
                 }
               }

               const T2Memory *mem = t2_.get_memory_handle();

               // Base-region fast path: see try_read_base_record()'s doc comment. No seqlock
               // needed -- the base mappings' bytes are immutable once written, so callback can
               // safely receive live spans straight into them.
               if (const auto base_record = try_read_base_record(mem, payload, BaseReader::kScan, nullptr);
                   base_record.has_value()) {
                 if (key_in_range(base_record->key, lower_bound, upper_bound)) {
                   callback(base_record->key, base_record->value);
                 }
                 ++total_count;
                 return;
               }

               // t2_.at() called inside read_t2_record_seqlock() (as AtFunc), matching get_impl() --
               // see read_t2_record_seqlock()'s comment.
               //
               // Torn-read fix: copy_func below must only *copy* into an owned buffer and
               // return, never invoke `callback` from inside it -- see get_impl()'s identical
               // fix and comment for the full rationale. thread_local since this is called
               // concurrently from many threads; static so repeated calls (once per matching
               // record, possibly many per scan()) reuse already-grown capacity instead of
               // reallocating.
               thread_local static std::vector<std::byte> tl_scan_key_buf;
               thread_local static std::vector<std::byte> tl_scan_value_buf;
               bool in_range =
                   read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(payload & kOffsetMask, mem); },
                                          [&](const T2RecordView &record) -> bool {
                                            if (!key_in_range(record.key, lower_bound, upper_bound)) {
                                              return false;
                                            }
                                            tl_scan_key_buf.assign(record.key.begin(), record.key.end());
                                            tl_scan_value_buf.assign(record.value.begin(), record.value.end());
                                            return true;
                                          });
               if (in_range) {
                 callback(std::span<const std::byte>(tl_scan_key_buf), std::span<const std::byte>(tl_scan_value_buf));
               }
               ++total_count;
             });

    return total_count;
  }

 private:
  auto t2_path() const -> std::filesystem::path { return t2_.path(); }

  // Whether a checkpoint was ever committed for `t2_path`, and if so how many bytes of its T2
  // data file are live -- the caller-supplied trust T2FlatFile's adopt-capable constructor
  // requires (see its doc comment). Called from the constructor's initializer list before t2_
  // exists, so this must be static; only the manifest is consulted here -- t2_'s own construction
  // is what actually validates the T2 data file itself against this value.
  static auto adopted_t2_bytes_used(const std::filesystem::path &t2_path) -> std::optional<uint64_t> {
    const auto manifest = vmemkv::read_manifest(vmemkv::derive_manifest_path(t2_path));
    if (!manifest.has_value()) {
      return std::nullopt;
    }
    return manifest->t2_bytes_used;
  }

  // Whether enough WAL has accumulated since the last checkpoint to need truncating. Sampled by
  // maybe_reorganize_if_needed() (the per-write path, strided -- see that call site's own comment
  // for why) to set reorg_requested_, and checked directly by reorg_worker_loop() to decide
  // between a Checkpoint and a plain T1Only cycle.
  auto wal_over_threshold() const -> bool { return wal_.size_bytes() >= ConfigT::WalMaxBytesSinceCheckpoint; }

  // Fast-boot path (low_level_design.md 5.3): if a checkpoint was ever committed, adopts its T1
  // sorted_region to match the T2 data T2FlatFile's constructor already adopted (see
  // adopted_t2_bytes_used(), consulted from the constructor's initializer list before t1_/t2_
  // exist). Leaves t1_ untouched if no manifest exists yet -- the ordinary fresh-start case, where
  // recover_from_wal()'s full replay from LSN 1 is already correct.
  //
  // Deliberately does NOT catch failures once the manifest is confirmed valid: rotate() means
  // the WAL only holds the tail since the last checkpoint, so once a manifest exists it's the
  // only route to everything before checkpoint_lsn -- silently falling back to "replay from
  // scratch" would quietly lose earlier records instead of loudly failing construction. A
  // missing/corrupt manifest is the ordinary fresh-start case and is handled as such.
  void load_checkpoint_if_present(const std::filesystem::path &t2_path) {
    const auto manifest = vmemkv::read_manifest(vmemkv::derive_manifest_path(t2_path));
    if (!manifest.has_value()) {
      return;
    }

    vmemkv::T1CheckpointFile t1_chk(vmemkv::derive_t1_chk_path(t2_path));

    // O(N) memcpy-shaped conversion (on-disk order -> EntrySnapshot order), no hashing or
    // per-key insertion -- what makes fast boot fast (low_level_design.md 5.4).
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    std::vector<EntrySnapshot> entries;
    entries.reserve(t1_chk.entries().size());
    for (const auto &on_disk : t1_chk.entries()) {
      entries.push_back(EntrySnapshot{on_disk.key_prefix, on_disk.payload_bits, on_disk.hash});
    }

    t1_.load_sorted_region_from_checkpoint(entries);
  }

  // Replays the current contents of wal_ into T1 (and, via write_entry_lockfree, T2) -- whether
  // that's the full history or just the post-checkpoint tail is transparent here. Runs before
  // reorg_worker_ is started (constructor order: recovering_=true; ...; recover_from_wal();
  // recovering_=false; *then* reorg_worker_ is move-assigned a real thread), so no other thread
  // can be touching reorg_running_/t1_/t2_ yet -- calls reorganize_internal() directly rather than
  // through the public reorg_running_ CAS/wait wrappers, which would be redundant synchronization
  // against a competitor that cannot exist at this point. Always ReorgMode::T1Only (checkpointing
  // mid-replay would deadlock, see reorganize_internal()'s own assert): this is a T1-only merge,
  // purely to reclaim T1 append-region capacity. Without the explicit capacity check below, a WAL
  // with more live distinct keys than one append region holds would livelock inside
  // write_entry_lockfree, which can only escape AppendRegionFull by waiting on a worker that
  // doesn't exist yet.
  void recover_from_wal() {
    wal_.replay([&](vmemkv::WalRecordType type,
                    std::span<const std::byte> key,
                    std::span<const std::byte> value,
                    uint64_t /*lsn*/) {
      if (t1_.append_size() + 1 >= T1IndexT::APPEND_CAP) {
        reorganize_internal(ReorgMode::T1Only);
      }
      switch (type) {
        case vmemkv::WalRecordType::Insert:
        case vmemkv::WalRecordType::Update:
          // T1Index::put() overwrites in place if the key exists, so Insert and Update replay
          // identically -- last-writer-wins falls out of existing T1 semantics for free.
          write_entry_lockfree(key, value);
          break;
        case vmemkv::WalRecordType::Delete:
          t1_.put(key, vmemkv::STORE_NOT_FOUND);  // Mirrors remove_impl's tombstone write.
          break;
      }
    });
  }

  // Restricted to keys <= 16 bytes: T1 only stores a 16-byte prefix (StoreKey), so for longer
  // keys the full key must live in T2 to resolve conflicts. For keys <= 16 bytes the prefix is
  // the entire key, so T2 can safely be bypassed.
  //
  // Also restricted to keys that don't end in a 0x00 byte: an inline entry has no T2 record, so
  // scan_impl() must recover the original key length from the zero-padded 16-byte prefix alone,
  // by trimming trailing zero bytes (see its own comment). That trim is only unambiguous when
  // every trailing zero byte in the stored prefix is padding -- a key whose own last byte is 0x00
  // (e.g. any multiple of 256 encoded as a big-endian integer key, or a key entirely of zero
  // bytes) would have a genuine zero trimmed away as if it were padding, silently truncating the
  // key scan_impl() hands back (and, via key_in_range()'s use of that truncated key, potentially
  // excluding the entry from scan results entirely). Excluding such keys from inlining here routes
  // them through the normal T2-record path instead, where the full key is stored verbatim and
  // scan_impl() reads it directly -- no recovery, no ambiguity. get_impl()/insert_impl()/
  // try_in_place_update() never need this recovery (the caller already supplies the full key), so
  // they're unaffected either way.
  // kInlineScalarValueBytes (this class's own stack-buffer size, used by get_impl()/scan_impl() to
  // hold a decoded inline value) and t1_detail::kInlineValueByteCount (T1Index's own inline-value
  // byte cap, checked below) encode the same invariant in two separate files with nothing else
  // tying them together -- if they ever drifted apart, T1Index could accept a payload wider than
  // this class's buffer, silently corrupting whatever memory follows it.
  static_assert(kInlineScalarValueBytes == t1_detail::kInlineValueByteCount);

  auto try_make_inline_payload(std::span<const std::byte> full_key,
                               std::span<const std::byte> value,
                               uint8_t &out_size) const noexcept -> std::optional<uint64_t> {
    if constexpr (ConfigT::UseT1InlineValue) {
      if (full_key.size() <= t1_detail::kPrefixBytes && (full_key.empty() || full_key.back() != std::byte{0})) {
        if (!value.empty() && value.size() <= t1_detail::kInlineValueByteCount) {
          uint64_t payload = 0;
          std::memcpy(&payload, value.data(), value.size());
          // A full-width (8-byte) value that happens to be all-1-bits is bit-for-bit identical to
          // T1's STORE_NOT_FOUND sentinel (t1_index.hpp) -- inlining it would make every read path
          // (get_impl()/scan_impl(), which both short-circuit to "not found" purely from seeing
          // that bit pattern in payload_bits) treat this live entry as permanently absent. Only
          // possible when value.size() == kInlineValueByteCount: shorter values leave payload's
          // upper bytes zeroed by the initializer above, so they can never reach ~0ULL. Falling
          // through to the normal T2-record path instead is safe: that path's payload is an
          // offset into T2, not the raw value bytes, so this exact collision cannot recur there.
          if (payload == vmemkv::STORE_NOT_FOUND) {
            return std::nullopt;
          }
          out_size = static_cast<uint8_t>(value.size());
          return payload;
        }
      }
    }
    return std::nullopt;
  }

  static auto byte_span_equal(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept -> bool {
    return std::ranges::equal(lhs, rhs);
  }

  static auto byte_span_less(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept -> bool {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  }

  static auto key_in_range(std::span<const std::byte> key,
                           std::span<const std::byte> lower_bound,
                           std::span<const std::byte> upper_bound) noexcept -> bool {
    return !byte_span_less(key, lower_bound) && !byte_span_less(upper_bound, key);
  }

  // Lets a caller (via setenv/unsetenv) suppress reorg_worker_loop()'s auto-triggered Checkpoint
  // for a bounded window, without affecting a manually requested reorganize()/checkpoint() call or
  // T1Only auto-triggering. Checked fresh every wakeup, not cached.
  static auto auto_reorg_suppressed() noexcept -> bool { return std::getenv("VMEMKV_SUPPRESS_AUTO_REORG") != nullptr; }

  void reorg_worker_loop(std::stop_token stop_token) {
    // Polls on a short, fixed interval rather than blocking on a condition variable: bounds both
    // shutdown latency and reaction time to a real reorganize request without needing a wakeup
    // signal. Short enough to be imperceptible against reorganize's own multi-millisecond-plus
    // duration.
    constexpr auto kIdlePollInterval = std::chrono::milliseconds(10);
    while (!stop_token.stop_requested()) {
      if (!reorg_requested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kIdlePollInterval);
        continue;
      }
      if (stop_token.stop_requested()) {
        break;
      }
      reorg_requested_.store(false, std::memory_order_release);

      // CAS, not an unconditional store: reorg_running_ is also claimed by the public
      // reorganize() method, and the two must never both believe they hold it at once (an
      // unconditional store here would let both end up inside reorganize_internal()
      // concurrently, corrupting state the manual caller's wait_until_reorg_not_running() assumed
      // was single-flight). If a manual reorganize() already holds it, this request is redundant
      // -- skip this round.
      bool expected_running = false;
      if (!reorg_running_.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel)) {
        continue;
      }
      try {
        // Runs a Checkpoint cycle if WAL pressure demands it, otherwise a plain T1Only merge.
        // Note reorg_requested_ (this wakeup's trigger) only ever fires on append-region/delete
        // pressure (see maybe_reorganize_if_needed()) -- WAL-size changes never themselves cause a
        // wakeup, so this only checks it "while already awake anyway."
        //
        // auto_reorg_suppressed() gates only this Checkpoint branch, never T1Only: T1's append
        // region has a hard capacity, so some reorg must keep draining it or
        // write_entry_lockfree()'s AppendRegionFull retry loop spins forever.
        if (wal_over_threshold() && !auto_reorg_suppressed()) {
          reorganize_internal(ReorgMode::Checkpoint);
        } else {
          reorganize_internal(ReorgMode::T1Only);
        }
      } catch (...) {
        // safe recovery in background
      }
      reorg_running_.store(false, std::memory_order_release);
    }
  }

  void maybe_reorganize_if_needed() {
    // reorg_requested_ is the only thing that wakes reorg_worker_loop() out of its idle poll to
    // evaluate wal_over_threshold(); the two entry-count thresholds below set it independently of
    // WAL bytes accumulated, so a large-value workload that rarely crosses either entry-count
    // threshold still gets a periodic wakeup to check the byte-based trigger.
    //
    // wal_over_threshold() -> Wal::size_bytes() does an fstat() -- unlike append_size/tail_size
    // below (plain atomic loads), that's a real syscall, too expensive to pay on every single
    // write. Sampled once every kWalCheckStride writes instead: the resulting delay past the
    // intended byte threshold is bounded by kWalCheckStride writes' worth of bytes, a rounding
    // error against the threshold itself.
    constexpr uint64_t kWalCheckStride = 64;
    if (wal_check_counter_.fetch_add(1, std::memory_order_relaxed) % kWalCheckStride == 0) {
      if (wal_over_threshold()) {
        reorg_requested_.store(true, std::memory_order_release);
      }
    }

    const size_t append_size = t1_.append_size();
    const size_t append_capacity = T1IndexT::APPEND_CAP;

    // Dynamically calculate the soft threshold based on workload state
    size_t soft_limit;
    if (scan_active_.load(std::memory_order_relaxed)) {
      // Scale-derived L2 cache capacity threshold (1MB / APPEND_SLOT_SIZE)
      // This keeps linear scanning bounded within private L2 caches.
      constexpr size_t kL2CacheSizeBytes = 1024 * 1024;  // 1MB
      constexpr size_t kL2SlotCapacity = kL2CacheSizeBytes / T1IndexT::APPEND_SLOT_SIZE;

      soft_limit = std::min(kL2SlotCapacity, (append_capacity * ConfigT::T1ReorganizeSoftThresholdPercent) / 100);
    } else {
      // Pure insert mode: allow append region to scale up to conservative capacity threshold
      soft_limit = (append_capacity * ConfigT::T1ReorganizeSoftThresholdPercent) / 100;
    }
    const size_t hard_limit = (append_capacity * ConfigT::T1ReorganizeHardThresholdPercent) / 100;

    if (append_size >= soft_limit) {
      reorg_requested_.store(true, std::memory_order_release);
    }

    if (append_size >= hard_limit || append_size >= append_capacity) {
      if (reorg_running_.load(std::memory_order_acquire)) {
        hard_stall_count_.fetch_add(1, std::memory_order_relaxed);
      }
      wait_until_reorg_not_running();
    }
  }

  // Stripe-local delete pressure, not a global tombstone ratio -- kept lightweight and read
  // outside the critical section so Delete stays short and contention-friendly.
  void maybe_reorganize_if_needed_for_delete(const AlignedMutex &stripe) {
    const uint64_t live_count = stripe.live_count.load(std::memory_order_relaxed);
    const uint64_t delete_count = stripe.delete_count.load(std::memory_order_relaxed);
    if (live_count == 0) {
      return;
    }

    constexpr uint64_t kMinLiveCount =
        T1IndexT::APPEND_CAP / kKeyStripeCount / 16;  // Scale-derived lower bound to avoid tiny-sample thrash.
    if (live_count < kMinLiveCount) {
      return;
    }

    if (delete_count >= live_count) {
      if (!reorg_running_.load(std::memory_order_acquire)) {
        reorg_requested_.store(true, std::memory_order_release);
      }
    }
  }

  std::jthread reorg_worker_;
  std::atomic<bool> reorg_requested_{false};
  std::atomic<bool> reorg_running_{false};
  mutable std::atomic<bool> scan_active_{false};

  // Mutex striping based on key hash. Splitting into 256 stripes prevents lock contention
  // on concurrent writes without the overhead of dynamic allocation for individual key locks.
  // Aligned to cache line size to prevent false sharing.
  static constexpr size_t kKeyStripeCount = 256;
  mutable std::array<AlignedMutex, kKeyStripeCount> write_stripes_;

  auto key_mutex(std::span<const std::byte> key) const noexcept -> std::mutex & { return stripe_state(key).mu; }

  auto stripe_state(std::span<const std::byte> key) const noexcept -> AlignedMutex & {
    const uint64_t hash = t1_detail::hash_full_key(key);
    return write_stripes_[hash & (kKeyStripeCount - 1)];
  }

  void reset_tombstone_counters() noexcept {
    for (auto &stripe : write_stripes_) {
      stripe.live_count.store(0, std::memory_order_relaxed);
      stripe.delete_count.store(0, std::memory_order_relaxed);
    }
  }

  T1IndexT t1_;
  vmemkv::T2FlatFile t2_;
  vmemkv::Wal wal_;
  std::atomic<uint64_t> reorg_t1_count_{0};
  std::atomic<uint64_t> checkpoint_count_{0};
  std::atomic<uint64_t> hard_stall_count_{0};
  // See wait_until_reorg_not_running()'s own comment.
  mutable std::atomic<uint64_t> total_hard_stall_duration_us_{0};

  bool recovering_ = false;  // True only during the constructor's initial WAL replay.

  // Write-side barrier claimed by checkpoint_internal() (see that function's own comment and
  // try_in_place_update()'s allow_in_place check): any in-place update whose allow_in_place check
  // observes offset < capture_watermark_ is guaranteed to be redirected out-of-place instead of
  // racing checkpoint's durabilizing read of that offset. Set once per cycle to `target` (msync()
  // covers the whole range in one shot, so the claim must too) and never moved again until the
  // cycle commits (reverted to old_base_boundary on abort, since nothing durable happened).
  // Conservative by construction: only ever needs to be *at least* as far along as what's
  // genuinely being read this cycle, never exactly so.
  mutable std::atomic<uint64_t> capture_watermark_{0};

  // Closes the gap capture_watermark_ alone leaves open: that field stops *new* in-place writes
  // from starting below a claimed boundary, but says nothing about one that had already passed
  // its allow_in_place check and started writing a moment earlier. checkpoint_internal() waits on
  // this (see try_in_place_update()'s enter() call) right after claiming capture_watermark_, so
  // base_boundary never publishes past an offset whose in-place write is still physically in
  // flight -- see InPlaceUpdateBarrier's own contract.
  mutable InPlaceUpdateBarrier in_place_update_barrier_;

  // Strides maybe_reorganize_if_needed()'s wal_over_threshold() sampling (see that call site's
  // own comment) -- wal_over_threshold() -> Wal::size_bytes() is an fstat(), too expensive to
  // call on every write.
  mutable std::atomic<uint64_t> wal_check_counter_{0};

  // Phase-breakdown timing published by checkpoint_internal() -- see VMemKVStatistics::
  // last_checkpoint_* and this file's own comment at that function's timing instrumentation.
  mutable std::atomic<uint64_t> last_checkpoint_duration_us_{0};
  mutable std::atomic<uint64_t> last_checkpoint_msync_duration_us_{0};
  mutable std::atomic<uint64_t> last_checkpoint_t1_reorganize_duration_us_{0};
  mutable std::atomic<uint64_t> last_checkpoint_stop_writers_duration_us_{0};
  mutable std::atomic<uint64_t> last_checkpoint_barrier_drain_duration_us_{0};
  mutable std::atomic<uint64_t> last_checkpoint_wal_rotate_duration_us_{0};
  mutable std::atomic<uint64_t> last_checkpoint_wal_rotate_leader_wait_us_{0};
  mutable std::atomic<uint64_t> last_checkpoint_bytes_synced_{0};
  mutable std::atomic<uint64_t> last_checkpoint_corpus_bytes_{0};
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

}  // namespace vmemkv
