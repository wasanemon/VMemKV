// vmemkv_impl.hpp - Checkpoint-free VMemKV coordinator implementation.
//
// ─── CONCURRENCY SPECIFICATION & MATRIX ──────────────────────────────────────
//
// The VMemKV architecture enforces thread-safety at the StoreImpl level,
// coordinating and routing operations across two underlying structural layers:
// 1. T1Index (In-memory Index)
// 2. T2FlatFile (Binary Disk Log File)
//
// +--------------------+-------------------+---------------------------------------------------+
// | Component          | Read Operations   | Write Operations (Insert/Update/Delete)           |
// +--------------------+-------------------+---------------------------------------------------+
// | vmemkv::T1Index    | Thread-Safe       | Thread-Unsafe (relies on StoreImpl serialization) |
// | vmemkv::T2FlatFile  | Thread-Safe       | Thread-Unsafe (relies on StoreImpl serialization) |
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
#include "core/spin_backoff.hpp"
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
// T2FlatFile::stop_writers_and_wait() is called (i.e. while the about-to-be-retired generation is
// still handed out normally by acquire_write_handle()). Lets a test deterministically get a
// writer's T2MemoryHandle registered *before* the stop flag goes up, so the subsequent
// stop-and-wait has a real, still-in-flight writer to wait for -- exercising the exact handshake
// that closes the residual-window race (see T2FlatFile::stop_writers_and_wait()'s declaration).
// No-op in production.
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
// * get(key)    : Look up Payload in T1. If inline, decode. If offset, resolve T2.
// * insert(key) : Write value to T2 -> Get Offset -> Insert (Key, Offset) into T1.
// * reorganize(): Rebuild T2 to a temp file containing only live records,
//                 remap T2, and republish T1 sorted_region (Bypassing concurrent runs).
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
      : initial_generation_(vmemkv::T2Memory::allocate_generation()),
        t1_(initial_generation_),
        t2_(t2_path, t2_bytes_capacity, initial_generation_, adopted_t2_bytes_used(t2_path)),
        wal_(vmemkv::derive_wal_path(t2_path)) {
    recovering_ = true;
    load_checkpoint_if_present(t2_path);
    recover_from_wal();
    recovering_ = false;
    reorg_worker_ = std::jthread(&VMemKVImpl::reorg_worker_loop, this);  // Started after recovery completes.
  }

  ~VMemKVImpl() noexcept {
    reorg_worker_.request_stop();
    reorg_requested_.store(true, std::memory_order_release);
    reorg_requested_.notify_all();
    if (reorg_worker_.joinable()) {
      reorg_worker_.join();
    }
  }

  static constexpr uint64_t kSizeEmbeddingShift = 48;
  static constexpr uint64_t kOffsetMask = (1ULL << kSizeEmbeddingShift) - 1;
  static constexpr uint64_t kBlockAlignment = 16;

  // T1Only: in-memory merge, zero I/O, T2 untouched. Checkpoint: durabilizes T2's live tail
  // in-place (checkpoint_internal()) -- no record ever moves. Defragment: relocates every live
  // record to fresh, sequentially-assigned offsets in a brand-new T2 file (defragment_internal())
  // -- restores key-order/physical-offset correlation and reclaims 100% of dead space, at O(live
  // corpus) cost instead of checkpoint's O(tail-since-last-cycle).
  enum class ReorgMode { T1Only, Checkpoint, Defragment };

  VMemKVImpl(const VMemKVImpl &) = delete;
  auto operator=(const VMemKVImpl &) -> VMemKVImpl & = delete;
  VMemKVImpl(VMemKVImpl &&) = delete;
  auto operator=(VMemKVImpl &&) -> VMemKVImpl & = delete;

  // Merges T1's sorted+append regions and, depending on `mode`, also durabilizes or relocates T2's
  // live data and persists the result as a manifest-committed checkpoint (low_level_design.md 5.2,
  // 5.6). Called under reorg_running_'s CAS guard (see reorganize()).
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void reorganize_internal(ReorgMode mode,
                           PreStopHook pre_stop_hook = PreStopHook{},
                           PreFinishHook pre_finish_hook = PreFinishHook{}) {
    // Checkpointing/defragmenting during WAL replay is unsafe: recover_from_wal() runs inside
    // wal_.replay()'s callback, and a checkpoint/defragment cycle expects to be the sole writer of
    // T1/T2/manifest/WAL state for its duration -- recursing into one mid-replay would let it
    // observe a T1/T2 still being reconstructed and publish a manifest against that incomplete
    // state. recover_from_wal() is the only caller that can run while recovering_ is true, and it
    // always passes ReorgMode::T1Only explicitly -- assert rather than silently override, so a
    // future caller bug surfaces instead of being papered over.
    assert((!recovering_ || mode == ReorgMode::T1Only) &&
           "must not request a checkpoint/defragment while recovering_ -- see recover_from_wal()'s call site");

    switch (mode) {
      case ReorgMode::Checkpoint:
        checkpoint_internal(pre_stop_hook, pre_finish_hook);
        break;
      case ReorgMode::Defragment:
        defragment_internal(pre_stop_hook, pre_finish_hook);
        break;
      case ReorgMode::T1Only:
        // T1-only reorganize (zero I/O): T2 isn't touched, so the mapper leaves every entry's
        // payload/generation untouched. Overwriting every generation to "current T2" here would
        // silently erase a stale generation left by a past race, with no way to detect it later.
        t1_.reorganize([](std::span<typename T1IndexT::EntrySnapshot> /*merged*/) {},
                       typename T1IndexT::NoOpChkWriter{},
                       t2_.get_memory()->generation);
        reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
        reset_tombstone_counters();
        break;
    }
    scan_active_.store(false, std::memory_order_relaxed);
  }

 private:
  // Shared by checkpoint_internal() and defragment_internal(): both open a T2 output file up
  // front and must close it on every exit path, including exceptions thrown partway through.
  struct FDGuard {
    int fd;
    ~FDGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  };

  // Resolves (prefix, hash) against T1's *current* state, or std::nullopt if the key was deleted
  // since it was observed, or its value has since shrunk into T1's inline threshold (an inline
  // entry never touches T2, so neither checkpoint_internal() nor defragment_internal() has
  // anything to do with it). Shared by both: each must re-resolve fresh right before acting on a
  // candidate rather than trust a payload cached at observation time, since the same key can be
  // recorded multiple times, or deleted, between being observed and being processed.
  auto resolve_live_non_inline(const StoreKey &prefix,
                               uint64_t hash) const -> std::optional<typename T1IndexT::LookupResult> {
    const auto res = t1_.get_by_prefix_hash(prefix, hash);
    if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
      return std::nullopt;
    }
    if constexpr (ConfigT::UseT1InlineValue) {
      if (t1_detail::is_inline(res.raw_hash)) {
        return std::nullopt;
      }
    }
    return res;
  }

  // Shared by checkpoint_internal() and defragment_internal(): both briefly stop writers
  // (T2FlatFile::stop_writers_and_wait()) while finishing a cycle and must resume them on every
  // exit path, including exceptions thrown partway through.
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
  // This is not a new hazard invented by the msync() design: it's the same one
  // try_in_place_update()'s allow_in_place check has always had to guard against (see that
  // function's own comment and the crash-recovery regression test for this) -- msync() durabilizing
  // the whole range in one shot instead of per-record just means the claim has to cover the whole
  // range at once too, rather than advancing record-by-record the way the old copy-loop design did.
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void checkpoint_internal(PreStopHook pre_stop_hook = PreStopHook{}, PreFinishHook pre_finish_hook = PreFinishHook{}) {
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
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
    t2_.stop_writers_and_wait(mem_to_drain);
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
    in_place_update_barrier_.wait_until_retired(target);

    try {
      if (target > old_base_boundary) {
        // msync()'s addr must be page-aligned; old_base_boundary is only kBlockAlignment-aligned.
        // Rounding the start down to the containing page and re-syncing that small overlap from
        // the previous cycle is harmless -- msync() is idempotent over bytes already durable.
        static const uint64_t kPageSize = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        const uint64_t aligned_start = old_base_boundary - (old_base_boundary % kPageSize);
        const vmemkv::T2Memory *mem = t2_.get_memory();
        if (::msync(mem->base + aligned_start, target - aligned_start, MS_SYNC) != 0) {
          throw std::system_error(errno, std::generic_category(), "msync t2 checkpoint");
        }
      }

      // TEST-ONLY: lets a test throw here, strictly before T1 is ever touched. No-op in
      // production -- see NoOpPreFinishHook.
      pre_finish_hook();

      // The only place T1 gets published this cycle. No record's payload_bits or generation ever
      // changes here -- checkpoint never relocates a record and never swaps to a new T2Memory, so
      // there is nothing for the offset_mapper to restamp; T1's own reorganize() still merges
      // append_region into sorted_region and writes the T1 checkpoint file (5.4 節) regardless.
      auto offset_mapper_fn = [](std::span<EntrySnapshot> /*merged*/) {};
      auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) {
        vmemkv::write_t1_checkpoint(vmemkv::derive_t1_chk_path(t2_path()), merged);
      };
      t1_.reorganize(offset_mapper_fn, chk_writer_fn, t2_.get_memory()->generation);

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
    wal_.rotate_segment();

    reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
    reorg_t2_count_.fetch_add(1, std::memory_order_relaxed);
    reset_tombstone_counters();
  }

  // Relocates every live T2 record (base- and tail-resident alike) to fresh, sequentially
  // assigned offsets in a brand-new T2 file -- restoring the key-order / physical-offset
  // correlation Scan depends on (low_level_design.md 4.1's "Ordering Fragmentation") and
  // reclaiming 100% of dead space, since the *entire* old file becomes unreferenced and gets
  // replaced wholesale. Deliberately not FALLOC_FL_PUNCH_HOLE-based: reclaiming individual dead
  // ranges within a still-live file only succeeds at filesystem block granularity, which this
  // project's typical small-value workloads rarely reach.
  //
  // Base is always key-sorted by induction: this cycle's own output is built key-sorted (see
  // below), so the *next* cycle can once again treat "read the base region in ascending physical
  // offset order" and "read it in key order" as the same operation.
  //
  // The base region is not a dense, gap-free sequence of records a byte-by-byte walk could safely
  // parse: a record that was live once, got superseded, and was never relocated by an intervening
  // defragment_internal() cycle keeps a well-formed but orphaned copy of its old bytes at its old
  // slot -- checkpoint_internal() durabilizes the tail unconditionally, dead records included.
  // Every record Phase 0 below touches is therefore addressed via an offset T1 itself vouches for
  // right now, never inferred from a preceding record's length.
  //
  //  - Phase 0 (concurrent with writers): one t1_.scan() pass collects every live, non-inline
  //    entry's full payload_bits (offset plus the embedded block-count size hint) below
  //    old_base_boundary into `live_payloads`, sorted ascending by offset -- purely in-memory,
  //    zero T2 I/O. Each is then read via try_read_base_record() (BaseReader::kScan, tuned for
  //    exactly this ascending-order access pattern -- readahead-friendly for small records,
  //    residency-checked pread() for large ones, avoiding the MADV_RANDOM primary mapping's
  //    per-page fault cost that dominates a naive walk), falling back to an unsynchronized
  //    t2_.at() read on the rare case that declines (offset < old_base_boundary is provably
  //    immutable for this whole phase, per the watermark freeze below, so no seqlock is needed
  //    either way). Live records are copied, still in ascending-offset (and therefore, by the
  //    induction above, ascending-key) order, into a scratch file -- not the final output yet,
  //    see Phase 1 below for why.
  //  - capture_watermark_ is frozen *once*, to old_base_boundary, before Phase 0 starts, and never
  //    moves again until the cycle commits or aborts. This uniformly protects every base offset
  //    from the very first instant, so every record Phase 0 reads is guaranteed immutable for the
  //    phase's whole duration (no seqlock needed there) -- one blanket freeze suffices here because
  //    base-resident records need no *individual* claiming: try_in_place_update()'s `offset >= mem->base_boundary`
  //    check already excludes all of them unconditionally. A key updated during Phase 0 is forced
  //    out-of-place instead, landing a fresh tail_entries_-tracked write >= old_base_boundary --
  //    not itself protected by this freeze, but that's fine: Phase 1 below only ever begins after
  //    stop_writers_and_wait() has returned, so by the time any tail candidate is read, no writer
  //    remains that could still be racing it. (The previous version of this function froze
  //    capture_watermark_ to the *current tail high-water mark* instead of old_base_boundary,
  //    reasoning that "however many times a key bounces between offsets in between is irrelevant
  //    since the catch-up pass re-reads whatever is current at drain time" -- true only if that
  //    catch-up pass cannot itself race a further in-place update, which its old pre-stop
  //    peek_live() call could: a second update landing on an already-redirected offset while
  //    writers were still live could mutate it in place, unrecorded by tail_entries_ (see
  //    try_in_place_update()'s comment), silently publishing a stale value. Doing all tail
  //    catch-up strictly after stop_writers_and_wait() -- as Phase 1 does -- removes the race
  //    instead of needing to out-run it.)
  //  - Phase 1 (writers stopped): tail_entries_.drain_and_clear() gives the complete, final set of
  //    tail-resident writes since the last cycle. Each is re-resolved against T1 fresh (never
  //    trusted from record() time) and its current key/value read once -- safe unsynchronized
  //    (via the seqlock, defensively) since no writer remains to race it. Deduplicated by offset
  //    (the same key can appear multiple times if it was written more than once) and sorted by
  //    *key*, not offset -- Phase 1 needs key order for the merge below, and there is no ordering
  //    hazard left to guard against once writers are stopped.
  //  - Final merge: the scratch file from Phase 0 (key-sorted) and the sorted tail candidates from
  //    Phase 1 are merged like the merge step of a mergesort, by key, into the real output file --
  //    on a key collision the tail candidate wins (it is always the more recent value; Phase 0
  //    can only have captured a since-superseded copy). This merge is what keeps the "base is
  //    always key-sorted" invariant intact across cycles: appending tail entries unsorted after
  //    Phase 0's output instead would permanently break the assumption Phase 0's own merge-cursor
  //    relies on for every future cycle.
  //  - t2_.acquire_write_handle()/stop_writers_and_wait()/resume_writers(), t1_.reorganize()'s
  //    single atomic publish point, T2Memory generation allocation/swap_memory()/retire_memory():
  //    all reused unmodified from checkpoint_internal().
  //
  // `relocated`: hash -> new-payload_bits map built by the final merge, consulted once by
  // offset_mapper_fn at publish time. Keyed by the same (prefix, hash) identity tail_entries_/
  // copy_live_entries() already use elsewhere in this file; a 64-bit full-key hash collision
  // between two different live keys is the same negligible, already-accepted risk every other
  // (prefix, hash)-keyed structure here carries, not a new one.
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void defragment_internal(PreStopHook pre_stop_hook = PreStopHook{}, PreFinishHook pre_finish_hook = PreFinishHook{}) {
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    const uint64_t checkpoint_lsn = wal_.next_lsn() - 1;

    const uint64_t old_base_boundary = t2_.get_memory()->base_boundary.load(std::memory_order_acquire);
    // Single blanket freeze for the cycle's whole duration -- see this function's own comment
    // above for why old_base_boundary (not the current tail high-water mark) is both correct and
    // sufficient given Phase 1 runs entirely after stop_writers_and_wait(). seq_cst: see
    // checkpoint_internal()'s identical store for why this field is always touched seq_cst.
    capture_watermark_.store(old_base_boundary, std::memory_order_seq_cst);

    const std::filesystem::path t2_chk_path = vmemkv::derive_t2_chk_path(t2_path());
    // Written to a fresh temp file, never the live one -- Wal::rotate()'s own write-then-rename
    // idiom, reused for the same reason: an atomic rename() replaces the canonical path's
    // directory entry in one step, and Linux keeps the *old* inode's blocks alive (via the old
    // T2Memory's still-open mmap/fd) until retire_memory() actually unmaps/closes it -- so no
    // explicit unlink() of the old file is needed at all.
    const std::filesystem::path t2_tmp_path = t2_chk_path.string() + ".defrag_tmp";
    const int t2_fd = ::open(t2_tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (t2_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open t2 defragment temp file");
    }
    FDGuard fd_guard{t2_fd};

    // Phase 0's scratch output -- never published, never renamed into place; merged into
    // fd_guard.fd (the real output) below and removed once the merge has consumed it.
    const std::filesystem::path t2_scratch_path = t2_chk_path.string() + ".defrag_scratch";
    const int scratch_fd = ::open(t2_scratch_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (scratch_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open t2 defragment scratch file");
    }
    FDGuard scratch_guard{scratch_fd};

    uint64_t next_offset = 0;
    uint64_t bytes_synced = 0;
    // hash -> fully-encoded new payload_bits (new offset | new block_count << shift). Populated
    // solely by the final merge below -- Phase 0's scratch write and Phase 1's gather do not touch
    // it, since only the merge knows each key's *final* winning offset.
    std::unordered_map<uint64_t, uint64_t> relocated;
    std::vector<std::pair<StoreKey, uint64_t>> drained_this_cycle;

    // Appends one record in the standard on-disk format to `buf`, tight-packed (alloc_len ==
    // value.size(), no slack) -- defragment()'s whole point is space reclamation, unlike
    // checkpoint_internal()'s copy which must preserve the original alloc_len exactly (see that
    // function's comment). Returns the aligned footprint just appended.
    auto append_record = [](std::vector<std::byte> &buf,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value,
                            uint64_t version) -> uint64_t {
      ValueRecordHeader header;
      header.key_len = static_cast<uint32_t>(key.size());
      header.value_len = static_cast<uint32_t>(value.size());
      header.alloc_len = static_cast<uint32_t>(value.size());
      header.version = version;
      const uint64_t raw_len = sizeof(header) + key.size() + value.size();
      const uint64_t aligned_len = vmemkv::align_up(raw_len);
      const std::size_t start = buf.size();
      buf.resize(start + aligned_len, std::byte{0});
      std::memcpy(buf.data() + start, &header, sizeof(header));
      std::memcpy(buf.data() + start + sizeof(header), key.data(), key.size());
      if (!value.empty()) {
        std::memcpy(buf.data() + start + sizeof(header) + key.size(), value.data(), value.size());
      }
      return aligned_len;
    };

    // Buffered sequential reader over the final merge's source, Phase 0's own scratch output:
    // wraps pread() with a refillable buffer so a record split across two reads is still returned
    // contiguously. Scratch contains only whole, confirmed-live records back to back (Phase 0
    // below only ever appends one via try_read_base_record()/the seqlock fallback, never touches
    // it otherwise), so -- unlike the old T2 file itself -- walking it byte-by-byte is safe: there
    // is no unwritten/never-durabilized gap to misparse (see this function's top comment for why
    // that distinction matters). Max single record size is bounded (~1.04MB, see
    // low_level_design.md 2.2's Embedded Block Count), well under kReadChunkBytes, so a record
    // never needs more than one refill.
    struct SeqReader {
      int fd;
      uint64_t limit;
      std::vector<std::byte> buf;
      std::size_t len = 0;
      std::size_t pos = 0;
      uint64_t file_pos = 0;

      SeqReader(int fd_, uint64_t limit_) : fd(fd_), limit(limit_), buf(64ULL * 1024 * 1024) {}

      auto exhausted() const -> bool { return file_pos >= limit && pos >= len; }

      void ensure_available(std::size_t need) {
        if (pos + need <= len) {
          return;
        }
        const std::size_t remaining = len - pos;
        std::memmove(buf.data(), buf.data() + pos, remaining);
        len = remaining;
        pos = 0;
        while (len < need && file_pos < limit) {
          const std::size_t to_read = std::min(buf.size() - len, static_cast<std::size_t>(limit - file_pos));
          if (to_read == 0) {
            break;
          }
          const ssize_t n = ::pread(fd, buf.data() + len, to_read, static_cast<off_t>(file_pos));
          if (n <= 0) {
            throw std::system_error(errno, std::generic_category(), "pread defragment sequential scan");
          }
          len += static_cast<std::size_t>(n);
          file_pos += static_cast<uint64_t>(n);
        }
      }

      // Valid until the next call to ensure_available() (i.e. the next call to this or
      // advance()).
      auto current() -> const ValueRecordHeader * {
        ensure_available(sizeof(ValueRecordHeader));
        const auto *peek = reinterpret_cast<const ValueRecordHeader *>(buf.data() + pos);
        const uint64_t raw_len = sizeof(ValueRecordHeader) + peek->key_len + peek->alloc_len;
        ensure_available(static_cast<std::size_t>(vmemkv::align_up(raw_len)));
        return reinterpret_cast<const ValueRecordHeader *>(buf.data() + pos);
      }

      void advance() {
        const auto *header = current();
        const uint64_t raw_len = sizeof(ValueRecordHeader) + header->key_len + header->alloc_len;
        pos += static_cast<std::size_t>(vmemkv::align_up(raw_len));
      }
    };

    try {
      // Phase 0: base-region rewrite -- see this function's top comment for why every record here
      // is addressed via an offset T1 vouches for right now, never inferred from a byte scan.
      {
        // One t1_.scan() pass, purely in-memory, collects every live non-inline entry's full
        // payload_bits (not just the offset) below old_base_boundary, sorted ascending by offset.
        // The embedded block-count size hint in the high bits (kSizeEmbeddingShift) is what lets
        // try_read_base_record() below size its read without a separate T1 lookup per record.
        std::vector<uint64_t> live_payloads;
        {
          std::array<std::byte, kStoreKeyBytes> min_key{};
          std::array<std::byte, kStoreKeyBytes> max_key;
          max_key.fill(std::byte{0xFF});
          t1_.scan(std::span<const std::byte>(min_key),
                   std::span<const std::byte>(max_key),
                   [&](std::span<const std::byte> /*index_key*/,
                       uint64_t payload,
                       uint64_t hash,
                       uint64_t /*t2_generation*/) {
                     if (payload == vmemkv::STORE_NOT_FOUND) {
                       return;
                     }
                     if constexpr (ConfigT::UseT1InlineValue) {
                       if (t1_detail::is_inline(hash)) {
                         return;
                       }
                     }
                     if ((payload & kOffsetMask) < old_base_boundary) {
                       live_payloads.push_back(payload);
                     }
                   });
          std::sort(live_payloads.begin(), live_payloads.end(), [](uint64_t a, uint64_t b) {
            return (a & kOffsetMask) < (b & kOffsetMask);
          });
        }

        // Ascending-offset order: readahead-friendly (try_read_base_record()'s BaseReader::kScan
        // mode is specifically tuned for this access pattern) and reproduces T1's key order in
        // the scratch output (base is key-sorted by induction -- see this function's top
        // comment), without ever needing to touch a byte this cycle doesn't already know, via
        // T1, to be live.
        T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
        std::vector<std::byte> cold_buf;

        constexpr uint64_t kFlushThresholdBytes = 64ULL * 1024 * 1024;
        std::vector<std::byte> out_buffer;
        out_buffer.reserve(kFlushThresholdBytes + (2 * 1024 * 1024));
        auto flush_scratch = [&]() {
          if (out_buffer.empty()) {
            return;
          }
          std::size_t written = 0;
          while (written < out_buffer.size()) {
            const ssize_t n = ::write(scratch_guard.fd, out_buffer.data() + written, out_buffer.size() - written);
            if (n < 0) {
              throw std::system_error(errno, std::generic_category(), "write t2 defragment scratch file");
            }
            written += static_cast<std::size_t>(n);
          }
          out_buffer.clear();
        };

        for (uint64_t payload : live_payloads) {
          auto view = try_read_base_record(mem, payload, BaseReader::kScan, &cold_buf);
          if (view) {
            append_record(out_buffer, view->key, view->value, view->header->version);
          } else {
            // Rare fallback (mincore()/pread() failure, or a defense-in-depth bounds check
            // declined) -- t2_.at() needs no seqlock here despite being unsynchronized elsewhere
            // in this file: offset < old_base_boundary is provably immutable for this whole
            // phase, per the watermark freeze above (mirrors try_read_base_record()'s own
            // BaseReader::kGet small-record case, which skips the seqlock for the same reason).
            const T2RecordView record = t2_.at(payload & kOffsetMask, mem);
            append_record(out_buffer, record.key, record.value, record.header->version);
          }
          if (out_buffer.size() >= kFlushThresholdBytes) {
            flush_scratch();
          }
        }
        flush_scratch();
        next_offset = 0;  // Real output (fd_guard.fd) is still empty -- Phase 0 wrote to scratch.
      }

      // Closes the residual window exactly like checkpoint_internal(): writer_stop_ blocks new
      // writers from here on, so tail_entries_.drain_and_clear() below is guaranteed to see the
      // complete, final live set with nothing left to arrive after it.
      const vmemkv::T2Memory *pre_swap_mem = t2_.get_memory();
      pre_stop_hook();
      t2_.stop_writers_and_wait(pre_swap_mem);
      WriterResumeGuard resume_guard{&t2_};

      // Phase 1: complete, final tail catch-up. No writer remains once stop_writers_and_wait()
      // has returned (see this function's top comment for why that removes the race Phase 0's
      // single watermark freeze alone could not close), so every candidate below is read exactly
      // once, unsynchronized-but-defensively-via-seqlock, with no re-validation needed.
      struct TailCandidate {
        std::vector<std::byte> key;
        std::vector<std::byte> value;
        uint64_t version;
        uint64_t hash;
      };
      std::vector<TailCandidate> tail_candidates;
      {
        struct Gathered {
          StoreKey prefix;
          uint64_t hash;
          uint64_t offset;
        };
        std::vector<Gathered> gathered;
        std::unordered_set<uint64_t> seen_offsets;
        tail_entries_.drain_and_clear([&](const StoreKey &prefix, uint64_t hash) {
          drained_this_cycle.emplace_back(prefix, hash);
          const auto res = resolve_live_non_inline(prefix, hash);
          if (!res) {
            return;
          }
          const uint64_t offset = res->payload_bits & kOffsetMask;
          if (!seen_offsets.insert(offset).second) {
            return;  // Same key written more than once since the last cycle -- already captured.
          }
          gathered.push_back({prefix, res->raw_hash, offset});
        });

        T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
        tail_candidates.reserve(gathered.size());
        for (const auto &g : gathered) {
          TailCandidate tc;
          tc.hash = g.hash;
          uint64_t version = 0;
          read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(g.offset, mem); },
                                 [&](const T2RecordView &record) -> bool {
                                   tc.key.assign(record.key.begin(), record.key.end());
                                   tc.value.assign(record.value.begin(), record.value.end());
                                   version = record.header->version;
                                   return true;
                                 });
          tc.version = version;
          tail_candidates.push_back(std::move(tc));
        }
        std::sort(tail_candidates.begin(), tail_candidates.end(), [](const TailCandidate &a, const TailCandidate &b) {
          return byte_span_less(a.key, b.key);
        });
      }

      // Final merge: Phase 0's key-sorted scratch output, merged with Phase 1's key-sorted tail
      // candidates, into the real output file -- see this function's top comment for why this
      // merge (not a plain append) is what keeps the base region key-sorted for the next cycle.
      {
        const uint64_t scratch_len = [&]() -> uint64_t {
          struct ::stat st {};
          if (::fstat(scratch_guard.fd, &st) != 0) {
            throw std::system_error(errno, std::generic_category(), "fstat t2 defragment scratch file");
          }
          return static_cast<uint64_t>(st.st_size);
        }();
        SeqReader scratch_reader(scratch_guard.fd, scratch_len);
        std::size_t tail_idx = 0;

        constexpr uint64_t kFlushThresholdBytes = 64ULL * 1024 * 1024;
        std::vector<std::byte> out_buffer;
        out_buffer.reserve(kFlushThresholdBytes + (2 * 1024 * 1024));
        auto flush_output = [&]() {
          if (out_buffer.empty()) {
            return;
          }
          std::size_t written = 0;
          while (written < out_buffer.size()) {
            const ssize_t n = ::write(fd_guard.fd, out_buffer.data() + written, out_buffer.size() - written);
            if (n < 0) {
              throw std::system_error(errno, std::generic_category(), "write t2 defragment temp file");
            }
            written += static_cast<std::size_t>(n);
          }
          next_offset += out_buffer.size();
          maybe_sync_and_drop_checkpoint_cache(fd_guard.fd, next_offset, bytes_synced);
          out_buffer.clear();
        };
        auto record_relocation =
            [&](uint64_t hash, std::span<const std::byte> key, std::span<const std::byte> value, uint64_t version) {
              const uint64_t out_offset = next_offset + out_buffer.size();
              const uint64_t written_len = append_record(out_buffer, key, value, version);
              const uint64_t block_count = written_len / kBlockAlignment;
              assert(block_count < 65536 && "Record size exceeds 1.04MB limit");
              relocated[hash] = out_offset | (block_count << kSizeEmbeddingShift);
              if (out_buffer.size() >= kFlushThresholdBytes) {
                flush_output();
              }
            };

        while (!scratch_reader.exhausted() || tail_idx < tail_candidates.size()) {
          if (scratch_reader.exhausted()) {
            const auto &tc = tail_candidates[tail_idx++];
            record_relocation(tc.hash, tc.key, tc.value, tc.version);
            continue;
          }
          const auto *header = scratch_reader.current();
          const std::byte *key_begin = reinterpret_cast<const std::byte *>(header + 1);
          const std::span<const std::byte> scratch_key(key_begin, header->key_len);

          if (tail_idx >= tail_candidates.size()) {
            const std::span<const std::byte> scratch_value(key_begin + header->key_len, header->value_len);
            record_relocation(t1_detail::hash_full_key(scratch_key), scratch_key, scratch_value, header->version);
            scratch_reader.advance();
            continue;
          }

          const auto &tc = tail_candidates[tail_idx];
          if (byte_span_less(tc.key, scratch_key)) {
            record_relocation(tc.hash, tc.key, tc.value, tc.version);
            ++tail_idx;
          } else if (byte_span_less(scratch_key, tc.key)) {
            const std::span<const std::byte> scratch_value(key_begin + header->key_len, header->value_len);
            record_relocation(t1_detail::hash_full_key(scratch_key), scratch_key, scratch_value, header->version);
            scratch_reader.advance();
          } else {
            // Same key in both: Phase 0's scratch copy is necessarily a since-superseded snapshot
            // (base-resident, so only reachable here if a redirect created this exact tail
            // candidate) -- the tail candidate always wins. Skip the scratch copy without writing
            // it.
            record_relocation(tc.hash, tc.key, tc.value, tc.version);
            ++tail_idx;
            scratch_reader.advance();
          }
        }
        flush_output();
      }

      {
        std::error_code remove_ec;
        std::filesystem::remove(t2_scratch_path, remove_ec);
      }

      const uint64_t new_base_boundary = next_offset;

      // Same headroom-beyond-what's-currently-used story as checkpoint_internal(): the new file
      // must have room for writes that land *after* this cycle publishes, not just enough to hold
      // what was just relocated.
      uint64_t new_capacity = t2_.bytes_capacity();
      if (new_base_boundary > new_capacity) {
        new_capacity = new_base_boundary;
      }

      pre_finish_hook();

      if (::ftruncate(fd_guard.fd, static_cast<off_t>(new_capacity)) != 0) {
        throw std::system_error(errno, std::generic_category(), "ftruncate t2 defragment temp file");
      }
      if (::fsync(fd_guard.fd) != 0) {
        throw std::system_error(errno, std::generic_category(), "fsync t2 defragment temp file");
      }

      const int map_fd = ::open(t2_tmp_path.c_str(), O_RDWR);
      if (map_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open t2 defragment temp file for mmap");
      }

      const uint64_t t2_pair_generation = vmemkv::T2Memory::allocate_generation();
      std::unique_ptr<vmemkv::T2Memory> new_mem =
          mmap_t2_memory(map_fd, new_capacity, "mmap t2 defragment", t2_pair_generation, new_base_boundary);

      // The only place T1 gets published this cycle. Every live entry's payload_bits is rewritten
      // to the offset/block_count `relocate_one()` assigned it above, and its generation tag to
      // this new T2Memory's -- see t1_index.hpp's reorganize() contract for why this in-place
      // rewrite of `merged` is safe here (not published/visible to any other thread until this
      // call returns).
      auto offset_mapper_fn = [&](std::span<EntrySnapshot> merged) {
        for (auto &entry : merged) {
          if (entry.payload_bits == vmemkv::STORE_NOT_FOUND) {
            continue;
          }
          if constexpr (ConfigT::UseT1InlineValue) {
            if (t1_detail::is_inline(entry.hash)) {
              continue;
            }
          }
          const auto it = relocated.find(entry.hash);
          assert(it != relocated.end() && "defragment_internal(): live entry missing from relocation map");
          entry.payload_bits = it->second;
          entry.generation = t2_pair_generation;
        }
      };
      auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) {
        vmemkv::write_t1_checkpoint(vmemkv::derive_t1_chk_path(t2_path()), merged);
      };
      t1_.reorganize(offset_mapper_fn, chk_writer_fn, t2_pair_generation);

      t2_.swap_memory(std::move(new_mem));
      t2_.resume_writers();

      std::error_code rename_ec;
      std::filesystem::rename(t2_tmp_path, t2_chk_path, rename_ec);
      if (rename_ec) {
        throw std::system_error(rename_ec, "rename t2 defragment temp file into place");
      }

      vmemkv::write_manifest(vmemkv::derive_manifest_path(t2_path()), checkpoint_lsn, new_base_boundary);
      // No checkpoint_lsn needed here -- see Wal::rotate_segment()'s own doc comment.
      wal_.rotate_segment();

      reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
      reorg_t2_count_.fetch_add(1, std::memory_order_relaxed);
      // Baseline for reorg_worker_loop()'s auto-trigger (defrag_growth_over_threshold()): this
      // cycle's own output size, freshly relocated and therefore minimal for the corpus as it
      // stands right now. Set on every successful cycle regardless of trigger source (auto or a
      // caller's direct defragment() call), so the next automatic check always measures growth
      // relative to the most recent actual cleanup.
      bytes_used_at_last_defragment_.store(new_base_boundary, std::memory_order_relaxed);
    } catch (...) {
      // Nothing durable was ever published (T1 is only touched on the success path above), so
      // this cycle's writes to the temp/scratch files are simply orphaned; remove them rather
      // than leaving them to be silently overwritten (or mistaken for stale ones) by a future
      // cycle.
      std::error_code remove_ec;
      std::filesystem::remove(t2_tmp_path, remove_ec);
      std::filesystem::remove(t2_scratch_path, remove_ec);
      for (const auto &[prefix, hash] : drained_this_cycle) {
        tail_entries_.record(prefix, hash);
      }
      // seq_cst: see checkpoint_internal()'s identical revert for why this field is always
      // touched seq_cst.
      capture_watermark_.store(old_base_boundary, std::memory_order_seq_cst);
      throw;
    }

    reset_tombstone_counters();
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

  // update_impl()'s in-place-update decision for a non-inline entry: retries under the same
  // generation-pairing dance as get_impl() -- see get_impl()'s comment for the full "why" -- until
  // it can either apply the update in place or conclusively decide it must fall through to
  // write_entry_lockfree(). Reads T1 (`res`) once per attempt, outside the T2-matching loop: the
  // inner loop re-fetches a real T2MemoryHandle every iteration and compares its generation
  // directly, with no separate peek-then-fetch step reorganize could advance across in between.
  // Re-reading T1 happens only on a directly-observed overshoot.
  auto try_in_place_update(std::span<const std::byte> full_key,
                           std::span<const std::byte> value) -> InPlaceUpdateResult {
    auto res = t1_.get_with_hash(full_key);
    SpinBackoff backoff;
    while (true) {
      if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
        return {InPlaceOutcome::Aborted};
      }
      if (t1_detail::is_inline(res.raw_hash)) {
        return {InPlaceOutcome::FellThrough};  // Inline entry -- fall through to write_entry_lockfree().
      }

      T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
      if (mem->generation < res.generation) {
        backoff.wait();  // T2 hasn't caught up to what T1 already reflects -- retry (no T1 re-read).
        continue;
      }
      backoff.reset();
      if (mem->generation > res.generation) {
        res = t1_.get_with_hash(full_key);  // Overshot -- res is stale, take a fresh T1 read.
        continue;
      }

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
      // capture_watermark_ extends the same redirect to a range checkpoint_internal()/
      // defragment_internal() has already claimed *this cycle*, before base_boundary itself has
      // advanced to cover it (see that member's own comment) -- without this, an in-place update
      // landing between the claim and the cycle's actual durabilizing/relocating read of this
      // offset could mutate bytes it's about to (or already did) capture: torn on disk if msync()
      // catches it mid-write, or silently reverted live once base_boundary publishes past it.
      // seq_cst: paired with checkpoint_internal()'s seq_cst store to this same field and with
      // in_place_update_barrier_'s own seq_cst registration/scan -- see that store's comment for
      // the independent-atomics race this closes.
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
  }

  // Bounded poll, not an unconditional atomic::wait(): same rationale as reorg_worker_loop()'s
  // idle wait (see its comment) -- std::atomic<bool>::wait/notify's real-world guarantee doesn't
  // rule out a missed wakeup, and this has no timed overload to bound it directly. Both call
  // sites below only reach this while an actual reorganize is already in flight (either a manual
  // reorganize()/checkpoint()/defragment() call found one running, or insert/update/delete hit
  // the hard backpressure limit), so the wait is inherently on the order of a reorganize's own
  // duration (milliseconds to seconds) already -- kIdlePollInterval's latency is not perceptible
  // against that, unlike a genuinely hot per-call path.
  void wait_until_reorg_not_running() const {
    constexpr auto kIdlePollInterval = std::chrono::milliseconds(10);
    while (reorg_running_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(kIdlePollInterval);
    }
  }

  // Shared wait/CAS/run/retry loop for the three public methods below. `decide` is invoked fresh
  // on every successful CAS (i.e. strictly after acquiring reorg_running_'s single-flight
  // guarantee, never before). `force_run` mirrors the old force_t2_gc=true contract: guarantees at
  // least one reorganize_internal() call actually completes even if T1's append region is already
  // empty (e.g. bulk_load()'s periodic internal reorganizes already drained it), rather than
  // skipping because there was "nothing to do" by the T1-emptiness measure alone.
  //
  // Performs (or waits for a concurrently-running) exactly one reorganize_internal() cycle, then
  // returns -- never loops back to check whether T1's append region has become fully empty.
  // Looping on that condition is what the pre-redesign convergence loop inside
  // reorganize_internal() itself (see TODO.md item 4's resolution) was already found and fixed
  // for: under sustained concurrent writes, the append region is essentially never momentarily
  // empty, so a caller re-checking it after every completed cycle can win the CAS against itself
  // indefinitely, executing cycle after cycle without ever returning. Measured directly (19
  // concurrent insert threads, 8B values): a single reorganize() call executed 265
  // reorganize_internal() cycles in 20s and still hadn't returned, with zero CAS losses to any
  // other caller the whole time -- i.e. it was racing only against its own moving target, not
  // contending with the background reorg_worker_ or anything else.
  template <typename DecideFn>
  void run_reorganize(DecideFn &&decide, bool force_run) {
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
        const ReorgMode mode = decide();
        try {
          reorganize_internal(mode);
        } catch (...) {
          reorg_running_.store(false, std::memory_order_release);
          reorg_running_.notify_all();
          throw;
        }
        reorg_running_.store(false, std::memory_order_release);
        reorg_running_.notify_all();
        return;
      }
      // Lost the race: someone else (background reorg_worker_, or another concurrent caller) is
      // already running a cycle. Loop back and wait for it, then try again -- needed for
      // force_run=true, since the winner's own decide() might not be ours (e.g. we wanted
      // checkpoint() but a plain reorganize() won the race), so only a cycle *we* ran ourselves
      // satisfies our caller's request. For force_run=false this can also retry, but is bounded
      // by how many cycles other callers actually run, not by our own append_size() target
      // continually moving.
    }
  }

 public:
  // T1-only in-memory merge. Never touches T2, never persists a checkpoint. Safe to call anytime.
  void reorganize() {
    run_reorganize([] { return ReorgMode::T1Only; }, /*force_run=*/false);
  }

  // Forces a defragment_internal() cycle: relocates every live T2 record to fresh, sequentially
  // assigned offsets in a brand-new T2 file, restoring key-order/physical-offset correlation
  // (Scan locality) and reclaiming 100% of dead space, then atomically replaces the store's T2
  // file with it. O(live corpus) cost -- see low_level_design.md 5.6 for the throughput argument
  // for why a threshold-triggered background cycle can still keep up with sustained writes.
  void defragment() {
    run_reorganize([] { return ReorgMode::Defragment; }, /*force_run=*/true);
  }

  // Forces a checkpoint_internal() cycle: durabilizes T2's live tail in place and persists the
  // result as a manifest-committed checkpoint.
  void checkpoint() {
    run_reorganize([] { return ReorgMode::Checkpoint; }, /*force_run=*/true);
  }

  // Accessors for T1 (Index) and T2 (Flat File) layers (mainly for testing).
  auto t1() noexcept -> T1IndexT & { return t1_; }
  auto t1() const noexcept -> const T1IndexT & { return t1_; }
  auto t2() noexcept -> vmemkv::T2FlatFile & { return t2_; }
  auto t2() const noexcept -> const vmemkv::T2FlatFile & { return t2_; }

  // TEST-ONLY: records `full_key` into tail_entries_ as write_entry_lockfree() would, for a test
  // that injects a T2 tail write via the low-level acquire_write_handle()/append_default()
  // primitives directly (bypassing write_entry_lockfree()) to simulate a specific outcome.
  void record_tail_entry_for_test(std::span<const std::byte> full_key) {
    tail_entries_.record(t1_detail::prefix_from_bytes(full_key), t1_detail::hash_full_key(full_key));
  }

  auto get_statistics() const noexcept -> vmemkv::VMemKVStatistics {
    return vmemkv::VMemKVStatistics{.t1_reorg_count = reorg_t1_count_.load(std::memory_order_relaxed),
                                    .t2_reorg_count = reorg_t2_count_.load(std::memory_order_relaxed),
                                    .hard_stall_count = hard_stall_count_.load(std::memory_order_relaxed)};
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

      // acquire_write_handle() (not a plain get_memory_handle()) defers while a reorganize() has
      // new writers stopped for its final pre-swap window, and -- critically -- `mem` is held
      // alive across both the T2 append below and the T1 publish attempt, not released in
      // between. That pairing is what closes reorganize_internal()'s residual-window race: as
      // long as this handle is live, stop_writers_and_wait() can't conclude that `mem`'s
      // generation is safe to retire, so a straggler entry naming it can never survive past the
      // reorg that's currently running. write_generation is simply `mem->generation` -- the exact
      // generation this write landed in, since we never let go of `mem` between writing and
      // publishing.
      typename T1IndexT::PutResult put_result;
      {
        T2FlatFile::T2MemoryHandle mem = t2_.acquire_write_handle();
        uint64_t offset = vmemkv::T2FlatFile::append_default(mem, full_key, value);
        uint64_t write_generation = mem->generation;

        uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + full_key.size() + value.size());
        uint64_t block_count = aligned_len / kBlockAlignment;
        assert(block_count < 65536 && "Record size exceeds 1.04MB limit");
        uint64_t encoded_payload = offset | (block_count << kSizeEmbeddingShift);
        put_result = t1_.put(full_key, encoded_payload, false, 0, write_generation);
        if (put_result == T1IndexT::PutResult::Applied) {
          // Recorded while `mem` is still held, not after this block closes: mem's release is
          // exactly the signal stop_writers_and_wait() uses to conclude this writer is done (see
          // this handle's own contract, referenced in the comment below). copy_live_entries()'s
          // post-stop pass only runs after that signal fires, so this must land first or that
          // pass could miss this entry entirely -- its bytes only exist in this (about to retire)
          // generation's tail.
          tail_entries_.record(t1_detail::prefix_from_bytes(full_key), t1_detail::hash_full_key(full_key));
        }
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
  // Get and Scan
  // want *different* readahead policies for that read though (madvise is a property of the
  // whole mapping, not of one read), so three distinct mappings/handles of the identical bytes
  // exist (`base_mmap_scan_seq`, `base_mmap_scan`, `read_fd` -- see mmap_t2_memory() for how
  // each is set up) -- which one a given call should use is decided per record (never once per
  // generation: a real corpus isn't guaranteed uniform record sizes even though this project's
  // benchmarks happen to use one size per run) by try_read_base_record()'s switch below. That
  // switch is the single place this decision is made, and callers never choose a mapping
  // themselves.
  // -------------------------------------------------------------------------------------------

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
    std::span<const std::byte> key(record_base + sizeof(ValueRecordHeader), header->key_len);
    std::span<const std::byte> value(key.data() + header->key_len, header->value_len);
    return T2RecordView{header, key, value};
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
    std::span<const std::byte> key(record_base + sizeof(ValueRecordHeader), header->key_len);
    std::span<const std::byte> value(key.data() + header->key_len, header->value_len);
    return T2RecordView{header, key, value};
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
  auto try_read_base_record(const T2FlatFile::T2MemoryHandle &mem,
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
          std::span<const std::byte> key(cold_buf->data() + sizeof(ValueRecordHeader), header->key_len);
          std::span<const std::byte> value(key.data() + header->key_len, header->value_len);
          return T2RecordView{header, key, value};
        }
    }
    assert(false && "unhandled BaseReader");
    return std::nullopt;
  }

  // Retrieves a value and invokes callback with its raw bytes.
  // - Thread-safety: lock-free, concurrently readable during reorganization.
  // - Concurrency note (canonical explanation; other methods below point here): looks up T1 once,
  //   then loops constructing a real T2MemoryHandle and comparing its generation directly against
  //   the T1 read, with no separate peek-then-fetch step in between for reorganize to advance
  //   across. If T2 hasn't caught up yet, retry cheaply without touching T1 again; only take a
  //   fresh T1 read on a directly-observed overshoot. This matters because
  //   checkpoint_internal() always calls t1_.reorganize() (re-stamping every entry to
  //   the new generation) strictly before t2_.swap_memory() (the point get_memory_handle() starts
  //   returning that generation): for the span between those two calls, T1 already reports
  //   generation N+1 for an entry while T2's live handle still reports N. A T1 offset resolved
  //   against the wrong T2 generation is not just wrong data -- the rebuilt T2 file can be a
  //   different size, so the offset can be out of bounds, and read_t2_record_seqlock() can spin
  //   forever on bytes that never settle into a valid record.
  template <typename Callback>
  auto get_impl(std::span<const std::byte> full_key, Callback callback) const -> bool {
    auto res = t1_.get_with_hash(full_key);
    SpinBackoff backoff;
    while (true) {
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

      T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
      if (mem->generation < res.generation) {
        backoff.wait();  // T2 hasn't caught up to what T1 already reflects -- retry (no T1 re-read).
        continue;
      }
      backoff.reset();
      if (mem->generation > res.generation) {
        res = t1_.get_with_hash(full_key);  // Overshot -- res is stale, take a fresh T1 read.
        continue;
      }

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
      // fresh), so the generation-pairing dance below has nothing to protect here.
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
  // commits a checkpoint (defragment() or checkpoint()) afterward. Still triggers ordinary T1-only
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
  // - Thread-safety: concurrently readable.
  // - Same generation-pairing concern as get_impl(), but a whole-scan retry-from-scratch isn't
  //   viable the way get_impl()'s retry is: t1_.scan() may have already delivered earlier entries
  //   to the callback before a later one mismatches, and restarting from `lower_bound` would
  //   double-deliver them. A single freshly-fetched handle (grabbed once before calling
  //   t1_.scan()) isn't enough either -- t1_.scan() takes its own T1 snapshot internally, after
  //   this function is entered, so that handle can already be one or more reorganize() cycles
  //   stale by the time t1_.scan() captures the entries it hands to the callback below (no writers
  //   needed; back-to-back reorganize() calls alone are enough), and even a handle re-fetched
  //   per-entry can't help once a *later* reorganize() lands mid-callback-loop, since
  //   get_memory_handle() only ever returns whatever's live *now* -- never a specific older
  //   generation an entry happens to be stamped for.
  // - So on a mismatch: abort the rest of this pass (t1_.scan()'s own loop still runs to
  //   completion, but harmlessly, since every subsequent candidate short-circuits below), remember
  //   the last key this pass actually finished examining, and re-invoke t1_.scan() from there
  //   (inclusive) under a freshly-paired T1 snapshot/T2 handle -- skipping re-delivery of that one
  //   already-delivered key. Keys arrive from t1_.scan() in strictly ascending order (see its
  //   dedup/sort step), so "resume from the last delivered key" can never re-visit or skip a live
  //   entry, only retry examining the ones a stale pass didn't get to yet.
  template <typename Callback>
  auto scan_impl(std::span<const std::byte> lower_bound,
                 std::span<const std::byte> upper_bound,
                 Callback callback) const -> size_t {
    if (!scan_active_.load(std::memory_order_relaxed)) {
      scan_active_.store(true, std::memory_order_relaxed);
    }

    size_t total_count = 0;
    std::array<std::byte, kStoreKeyBytes> resume_key{};
    bool have_resume_key = false;
    // Same SpinBackoff as get_impl()/try_in_place_update()'s retry loops -- a bare immediate
    // retry here (no backoff) is the identical anti-pattern already found, twice, to cause
    // genuine sustained CI-runner starvation: a concurrent defragment() cycling generations
    // faster than one t1_.scan() pass can complete under real contention could otherwise retry
    // this loop indefinitely without ever yielding real CPU time to whichever thread is actually
    // advancing T2's generation. See SpinBackoff's doc comment.
    SpinBackoff backoff;

    while (true) {
      std::span<const std::byte> current_lower =
          have_resume_key ? std::span<const std::byte>(resume_key.data(), kStoreKeyBytes) : lower_bound;

      bool mismatch = false;
      std::array<std::byte, kStoreKeyBytes> last_key{};
      bool advanced = false;
      size_t pass_count = 0;

      t1_.scan(current_lower,
               upper_bound,
               // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
               [&](std::span<const std::byte> index_key, uint64_t payload, uint64_t hash, uint64_t t2_generation) {
                 if (mismatch) {
                   return;
                 }
                 if (payload == vmemkv::STORE_NOT_FOUND) {
                   return;
                 }
                 // Resuming re-scans from the last delivered key inclusively -- skip re-delivering it.
                 if (have_resume_key &&
                     byte_span_equal(index_key, std::span<const std::byte>(resume_key.data(), kStoreKeyBytes))) {
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
                     // since it's not obvious from the arithmetic alone.
                     static_assert(kStoreKeyBytes == 2 * sizeof(uint64_t));
                     static_assert(std::endian::native == std::endian::little);
                     uint64_t lo_word;
                     uint64_t hi_word;
                     std::memcpy(&lo_word, index_key.data(), sizeof(lo_word));
                     std::memcpy(&hi_word, index_key.data() + sizeof(lo_word), sizeof(hi_word));
                     const size_t len = hi_word != 0 ? sizeof(lo_word) + (std::bit_width(hi_word) + 7) / 8
                                                     : (std::bit_width(lo_word) + 7) / 8;

                     // Inline values never reference T2, so they can never generation-mismatch.
                     // last_key needs the full, untrimmed bytes regardless (a later record's mismatch
                     // can resume from here); key_view reuses that copy instead of a second one.
                     std::memcpy(last_key.data(), index_key.data(), kStoreKeyBytes);
                     std::span<const std::byte> key_view(last_key.data(), len);
                     advanced = true;
                     if (!key_in_range(key_view, lower_bound, upper_bound)) {
                       return;
                     }
                     callback(key_view, std::span<const std::byte>(stack_value.data(), size));
                     ++pass_count;
                     return;
                   }
                 }

                 // Generation mismatch -- see scan_impl()'s doc comment above.
                 T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
                 if (t2_generation != mem->generation) {
                   mismatch = true;
                   return;
                 }

                 std::memcpy(last_key.data(), index_key.data(), kStoreKeyBytes);
                 advanced = true;

                 // Base-region fast path: see try_read_base_record()'s doc comment. No seqlock
                 // needed -- the base mappings' bytes are immutable once written, so callback can
                 // safely receive live spans straight into them.
                 if (const auto base_record = try_read_base_record(mem, payload, BaseReader::kScan, nullptr);
                     base_record.has_value()) {
                   if (key_in_range(base_record->key, lower_bound, upper_bound)) {
                     callback(base_record->key, base_record->value);
                   }
                   ++pass_count;
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
                 ++pass_count;
               });

      total_count += pass_count;
      if (!mismatch) {
        break;
      }
      if (advanced) {
        resume_key = last_key;
        have_resume_key = true;
      }
      // else: the mismatch fired on the very first (non-skipped) candidate this pass -- retry the
      // exact same range under a fresh handle/snapshot pairing.
      backoff.wait();
    }

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

  // Whether enough WAL has accumulated since the last checkpoint to need truncating. Used only by
  // reorg_worker_loop() to decide whether to call checkpoint()-equivalent behavior.
  auto wal_over_threshold() const -> bool { return wal_.size_bytes() >= ConfigT::WalMaxBytesSinceCheckpoint; }

  // Whether T2's total footprint has grown enough since the last defragment_internal() cycle (or
  // since startup, if none has ever run) to warrant another one. Used only by reorg_worker_loop().
  // bytes_used, not base_boundary: bytes_used also counts whatever's currently in the tail, so
  // growth here reflects total space consumed regardless of how recently checkpoint() last ran.
  // Gated by DefragMinBytesBeforeTrigger so a store that hasn't reached a meaningful size yet
  // doesn't pay for a cycle before there's anything worth reclaiming.
  auto defrag_growth_over_threshold() const -> bool {
    const uint64_t current = t2_.get_memory()->bytes_used.load(std::memory_order_acquire);
    if (current < ConfigT::DefragMinBytesBeforeTrigger) {
      return false;
    }
    const uint64_t baseline = bytes_used_at_last_defragment_.load(std::memory_order_acquire);
    return current >= (baseline * ConfigT::DefragGrowthThresholdPercent) / 100;
  }

  // Maps `capacity` bytes of `file_descriptor` MAP_SHARED and wraps the result in a T2Memory,
  // closing the fd in all cases. Used by defragment_internal() for its relocated-generation
  // rebuild -- checkpoint's own adoption at startup goes through T2FlatFile's constructor instead
  // (see its doc comment), which sets up an equivalent mapping directly. `bytes_used` is baked
  // into the T2Memory itself rather than set separately -- see T2Memory::bytes_used's declaration
  // for why a mem pointer and its byte-usage counter must never be independently settable.
  static auto mmap_t2_memory(int file_descriptor,
                             uint64_t capacity,
                             const char *what,
                             uint64_t generation,
                             uint64_t bytes_used) -> std::unique_ptr<vmemkv::T2Memory> {
    void *mapped = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, file_descriptor, 0);
    const int mmap_errno = errno;  // Captured before close(), which on success must not clobber it.
    if (mapped == MAP_FAILED) {
      ::close(file_descriptor);
      throw std::system_error(mmap_errno, std::generic_category(), what);
    }
    // madvise is per-VMA, not per-inode, so this must be re-applied to every fresh mapping this
    // function produces (every defragment remaps T2) -- it does not carry over from
    // T2FlatFile::map_file()'s initial mmap. See that function's identical call for why this
    // stays unconditional.
    if (::madvise(mapped, capacity, MADV_RANDOM) != 0) {
      ::close(file_descriptor);
      throw std::system_error(errno, std::generic_category(), "madvise MADV_RANDOM (checkpoint/reorg)");
    }

    // Best-effort second and third mappings for T2's base-region reads -- see
    // try_read_base_record() above for who reads which: `base_mmap_scan_seq` (MADV_SEQUENTIAL)
    // is Scan's small-record mapping only; `base_mmap_scan` (kernel default) is shared by Scan's
    // large-record path and Get's large-record warm path (Get's small-record path reads the
    // *main* mapping directly, since that's already MADV_RANDOM -- exactly what Get wants
    // regardless of size). Both exist because madvise is a per-VMA property, not a per-read one,
    // so Scan's own two size classes need genuinely different mappings; Get's large-record cold
    // case reads the same bytes via pread instead (below). Both mapped to the *full* `capacity`,
    // not just `bytes_used` -- unlike the main mapping, unwritten/never-promoted pages here are
    // simply never touched (every read is gated by offset < base_boundary), so over-mapping
    // costs nothing and lets a T1-only checkpoint's incremental base_boundary promotion
    // (reorganize_internal()) grow their *effective* coverage later without ever remapping (see
    // T2Memory::base_mmap_scan's comment for why a read-only, never-written-through mapping like
    // these transparently reflects a later pwrite() to the file with no remap needed). Created
    // unconditionally (not gated on bytes_used > 0): a store whose very first checkpoint is a
    // forced full rebuild before any insert ever lands (bytes_used == 0 at that point) must still
    // get real mappings here, or promotion would have nothing to extend later and the whole
    // mechanism would silently never activate for that store. A failure to establish either one
    // is silently non-fatal: the primary mapping above already provides full correctness via
    // scan_impl()'s existing seqlock fallback, these are purely a speed optimization layered on
    // top.
    std::byte *base_mmap_scan_ptr = nullptr;
    std::byte *base_mmap_scan_seq_ptr = nullptr;
    int read_fd_dup = -1;
    {
      void *base_mapped_scan = ::mmap(nullptr, capacity, PROT_READ, MAP_SHARED, file_descriptor, 0);
      if (base_mapped_scan != MAP_FAILED) {
        base_mmap_scan_ptr = static_cast<std::byte *>(base_mapped_scan);
      }

      void *base_mapped_scan_seq = ::mmap(nullptr, capacity, PROT_READ, MAP_SHARED, file_descriptor, 0);
      if (base_mapped_scan_seq != MAP_FAILED) {
        if (::madvise(base_mapped_scan_seq, capacity, MADV_SEQUENTIAL) == 0) {
          base_mmap_scan_seq_ptr = static_cast<std::byte *>(base_mapped_scan_seq);
        } else {
          ::munmap(base_mapped_scan_seq, capacity);
        }
      }

      // dup()'d read handle for get_impl()'s bounded pread() of large records in the same base
      // region -- see T2Memory::read_fd's doc comment for why Get reads large records via pread
      // instead of through one of the two mmaps above. Must dup() before file_descriptor is
      // closed below. Best-effort like the mappings above: a dup() failure just means get_impl()
      // falls back to the always-correct `base` + seqlock path.
      read_fd_dup = ::fcntl(file_descriptor, F_DUPFD_CLOEXEC, 0);
    }

    ::close(file_descriptor);
    auto mem = std::make_unique<vmemkv::T2Memory>(static_cast<std::byte *>(mapped), capacity, generation, bytes_used);
    mem->base_mmap_scan = base_mmap_scan_ptr;
    mem->base_mmap_scan_seq = base_mmap_scan_seq_ptr;
    mem->read_fd = read_fd_dup;
    return mem;
  }

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
    // per-key insertion -- what makes fast boot fast (low_level_design.md 5.4). Every entry in
    // one checkpoint was written against the same T2 generation it rebuilt, and t2_ was
    // constructed against that identical generation (initial_generation_), so all loaded entries
    // are uniformly stamped with it.
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    std::vector<EntrySnapshot> entries;
    entries.reserve(t1_chk.entries().size());
    for (const auto &on_disk : t1_chk.entries()) {
      entries.push_back(EntrySnapshot{on_disk.key_prefix, on_disk.payload_bits, on_disk.hash, initial_generation_});
    }

    t1_.load_sorted_region_from_checkpoint(entries, initial_generation_);
  }

  // Replays the current contents of wal_ into T1 (and, via write_entry_lockfree, T2) -- whether
  // that's the full history or just the post-checkpoint tail is transparent here. Runs before
  // reorg_worker_ is started (constructor order: recovering_=true; ...; recover_from_wal();
  // recovering_=false; *then* reorg_worker_ is move-assigned a real thread), so no other thread
  // can be touching reorg_running_/t1_/t2_ yet -- calls reorganize_internal() directly rather than
  // through the public reorg_running_ CAS/wait wrappers, which would be redundant synchronization
  // against a competitor that cannot exist at this point. Always do_checkpoint=false (checkpointing
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
  auto try_make_inline_payload(std::span<const std::byte> full_key,
                               std::span<const std::byte> value,
                               uint8_t &out_size) const noexcept -> std::optional<uint64_t> {
    if constexpr (ConfigT::UseT1InlineValue) {
      if (full_key.size() <= t1_detail::kPrefixBytes) {
        if (!value.empty() && value.size() <= t1_detail::kInlineValueByteCount) {
          out_size = static_cast<uint8_t>(value.size());
          uint64_t payload = 0;
          std::memcpy(&payload, value.data(), value.size());
          return payload;
        }
      }
    }
    return std::nullopt;
  }

  // Bounds a checkpoint temp file's page cache footprint to roughly one interval's worth instead
  // of growing unbounded until the final commit-time fsync: every kCheckpointSyncIntervalBytes,
  // flushes that span and tells the kernel to drop the now-clean pages. Best-effort -- a failure
  // here only delays memory reclaim, never a correctness problem (durability comes from the
  // unconditional fsync at commit time), so nothing here throws.
  static void maybe_sync_and_drop_checkpoint_cache(int file_descriptor,
                                                   uint64_t bytes_used,
                                                   uint64_t &bytes_synced) noexcept {
    constexpr uint64_t kCheckpointSyncIntervalBytes = 512ULL * 1024 * 1024;  // 512MiB
    if (bytes_used - bytes_synced < kCheckpointSyncIntervalBytes) {
      return;
    }
    if (::fdatasync(file_descriptor) == 0) {
      ::posix_fadvise(file_descriptor,
                      static_cast<off_t>(bytes_synced),
                      static_cast<off_t>(bytes_used - bytes_synced),
                      POSIX_FADV_DONTNEED);
    }
    bytes_synced = bytes_used;
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

  void reorg_worker_loop(std::stop_token stop_token) {
    // Polls on a short, fixed interval instead of an unconditional atomic::wait(): this thread
    // can otherwise be left parked in wait() past the point the destructor's request_stop() +
    // reorg_requested_.store(true) + notify_all() have already run, with no further notify ever
    // coming -- std::atomic<bool>::wait/notify carries no stronger real-world guarantee across
    // standard-library implementations than "eventually observed," and has no timed overload to
    // bound it directly. notify_all() calls elsewhere (maybe_reorganize_if_needed(), the
    // destructor) are now inert -- kept only because they're harmless and cheap, not because
    // anything still waits on them -- so kIdlePollInterval is this loop's only real reaction
    // latency, both for shutdown and for picking up a real reorganize request; kept short (not
    // e.g. 100ms) so neither cost is perceptible against reorganize's own multi-millisecond-plus
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
        // Explicit priority: checkpoint pressure (WAL size) wins over defragment pressure
        // (tail-tracker or T2 footprint growth), which wins over a plain T1-only reorganize. Note
        // reorg_requested_ (this wakeup's trigger) only ever fires on append-region/delete
        // pressure (see maybe_reorganize_if_needed()) -- tail_entries_/WAL-size/T2-footprint
        // changes never themselves cause a wakeup, so this only checks them "while already awake
        // anyway."
        if (wal_over_threshold()) {
          reorganize_internal(ReorgMode::Checkpoint);
        } else if (tail_entries_.near_capacity() || defrag_growth_over_threshold()) {
          reorganize_internal(ReorgMode::Defragment);
        } else {
          reorganize_internal(ReorgMode::T1Only);
        }
      } catch (...) {
        // safe recovery in background
      }
      reorg_running_.store(false, std::memory_order_release);
      reorg_running_.notify_all();
    }
  }

  void maybe_reorganize_if_needed() {
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
      reorg_requested_.notify_all();
    }

    if (append_size >= hard_limit || append_size >= append_capacity) {
      if (reorg_running_.load(std::memory_order_acquire)) {
        hard_stall_count_.fetch_add(1, std::memory_order_relaxed);
      }
      wait_until_reorg_not_running();
    }

    // Same soft/hard split as above, for tail_entries_ instead of the T1 append region. Unlike
    // the append region (drained by *any* reorganize_internal() call), only a defragment cycle
    // drains tail_entries_ -- see reorg_worker_loop()'s priority check, which fires a defragment
    // whenever tail_entries_.near_capacity() so the wakeup this triggers actually picks that kind
    // of cycle.
    const size_t tail_size = tail_entries_.size();
    const size_t tail_capacity = ConfigT::TailEntryCapacityEntries;
    const size_t tail_soft_limit = (tail_capacity * ConfigT::TailEntrySoftThresholdPercent) / 100;
    const size_t tail_hard_limit = (tail_capacity * ConfigT::TailEntryHardThresholdPercent) / 100;

    if (tail_size >= tail_soft_limit) {
      reorg_requested_.store(true, std::memory_order_release);
      reorg_requested_.notify_all();
    }

    if (tail_size >= tail_hard_limit) {
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
        reorg_requested_.notify_all();
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

  auto key_mutex(std::span<const std::byte> key) const noexcept -> std::mutex & {
    const uint64_t hash = t1_detail::hash_full_key(key);
    return write_stripes_[hash & (kKeyStripeCount - 1)].mu;
  }

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

  // Reserved once, before t1_/t2_ construct, and stamped on both t1_'s initial sorted_snapshot_
  // and t2_'s initial T2Memory so the two agree on a matching pair from construction.
  const uint64_t initial_generation_;
  T1IndexT t1_;
  vmemkv::T2FlatFile t2_;
  vmemkv::Wal wal_;
  std::atomic<uint64_t> reorg_t1_count_{0};
  std::atomic<uint64_t> reorg_t2_count_{0};
  std::atomic<uint64_t> hard_stall_count_{0};

  bool recovering_ = false;  // True only during the constructor's initial WAL replay.

  // Write-side barrier claimed by both checkpoint_internal() and defragment_internal() (see each
  // one's own comment and try_in_place_update()'s allow_in_place check): any in-place update whose
  // allow_in_place check observes offset < capture_watermark_ is guaranteed to be redirected
  // out-of-place instead of racing a durabilizing/relocating read of that offset. Set once per
  // cycle and never moved again until the cycle commits (reverted to old_base_boundary on abort,
  // since nothing durable happened) -- checkpoint_internal() claims target (msync() covers the
  // whole range in one shot, so the claim must too); defragment_internal() claims
  // old_base_boundary (its Phase 0 reads only the base region, never the tail). Conservative by
  // construction: only ever needs to be *at least* as far along as what's genuinely being read
  // this cycle, never exactly so.
  mutable std::atomic<uint64_t> capture_watermark_{0};

  // Closes the gap capture_watermark_ alone leaves open: that field stops *new* in-place writes
  // from starting below a claimed boundary, but says nothing about one that had already passed
  // its allow_in_place check and started writing a moment earlier. checkpoint_internal() waits on
  // this (see try_in_place_update()'s enter() call) right after claiming capture_watermark_, so
  // base_boundary never publishes past an offset whose in-place write is still physically in
  // flight -- see InPlaceUpdateBarrier's own contract.
  mutable InPlaceUpdateBarrier in_place_update_barrier_;

  // reorg_worker_loop()'s auto-trigger baseline for defragment(): T2's total footprint
  // (bytes_used) as of the end of the most recent successful defragment_internal() cycle, updated
  // by that function itself regardless of trigger source. 0 until the first cycle ever completes.
  std::atomic<uint64_t> bytes_used_at_last_defragment_{0};

  // Records (StoreKey prefix, hash) for every entry written into T2's tail region (offset >=
  // old_base_boundary) since the last checkpoint_internal() cycle, so copy_live_entries() can
  // enumerate exactly what needs durabilizing instead of scanning the entire live keyspace.
  // Fixed-capacity, atomic-index-allocated, lock-free append: each slot is a compound record --
  // an index's data (entries_[i]) is written as plain (non-atomic) memory, then published via a
  // separate atomic ready flag -- record() writes entries_[i] before ready_[i].store(true,
  // release), drain_and_clear() only reads entries_[i] after observing ready_[i].load(acquire) ==
  // true, so the release/acquire pairing on ready_[i] makes the plain write to entries_[i] safely
  // visible.
  //
  // Dropping an entry here is a correctness bug, not a benign leak (its bytes only exist in the
  // old generation's tail, which the cycle discards) -- capacity is never
  // actually allowed to run out: maybe_reorganize_if_needed()'s hard threshold blocks writers,
  // and the background worker forces a T2-touching cycle (which drains this), well before size()
  // could reach kCapacity. record() unconditionally reserving-then-dropping past kCapacity is
  // defense in depth only, unreachable if those thresholds hold.
  class TailEntryTracker {
   public:
    TailEntryTracker()
        : entries_(static_cast<Entry *>(::mmap(nullptr,
                                               kCapacity * sizeof(Entry),
                                               PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                               -1,
                                               0))),
          ready_(static_cast<std::atomic<bool> *>(::mmap(nullptr,
                                                         kCapacity * sizeof(std::atomic<bool>),
                                                         PROT_READ | PROT_WRITE,
                                                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                                         -1,
                                                         0))) {
      if (entries_ == MAP_FAILED || ready_ == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap TailEntryTracker");
      }
      for (size_t i = 0; i < kCapacity; ++i) {
        new (&ready_[i]) std::atomic<bool>(false);
      }
    }
    ~TailEntryTracker() {
      ::munmap(entries_, kCapacity * sizeof(Entry));
      ::munmap(ready_, kCapacity * sizeof(std::atomic<bool>));
    }
    TailEntryTracker(const TailEntryTracker &) = delete;
    auto operator=(const TailEntryTracker &) -> TailEntryTracker & = delete;

    void record(const StoreKey &prefix, uint64_t hash) noexcept {
      size_t index = tail_.load(std::memory_order_relaxed);
      while (true) {
        if (index >= kCapacity) {
          return;  // Defense in depth only -- see this tracker's own comment.
        }
        if (tail_.compare_exchange_weak(index, index + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
          entries_[index] = Entry{prefix, hash};
          ready_[index].store(true, std::memory_order_release);
          return;
        }
      }
    }

    [[nodiscard]] auto size() const noexcept -> size_t {
      return std::min<size_t>(tail_.load(std::memory_order_relaxed), kCapacity);
    }

    // Forces a checkpoint cycle (which drains this tracker) before it could otherwise fill purely
    // from soft-threshold-triggered T1-only cycles, which don't touch this at all.
    [[nodiscard]] auto near_capacity() const noexcept -> bool {
      return size() >= (kCapacity * ConfigT::TailEntrySoftThresholdPercent) / 100;
    }

    // Read-only pass for copy_live_entries()'s pre-stop call: calls fn(prefix, hash) for every
    // currently-published entry without resetting anything. Safe to call while writers are still
    // live -- unlike drain_and_clear() below, never touches ready_/tail_, so a concurrent
    // record() reusing a low index mid-read can only cause a torn read to skip an entry (silently
    // missing an in-progress publish) or see a *different* live entry than the one that grew
    // tail_ to include this index. Either way, harmless: copy_live_entries()'s post-stop
    // drain_and_clear() call re-derives every candidate fresh from T1 (get_by_prefix_hash())
    // rather than trusting what's cached here, and is guaranteed to see the complete, final set
    // once writers are actually stopped. This call is purely a head start, matching its "safe to
    // run any number of times, including zero" contract -- it is never, on its own, sufficient
    // for correctness.
    template <typename Fn>
    void peek_live(Fn &&fn) const {
      const size_t count = std::min<size_t>(tail_.load(std::memory_order_acquire), kCapacity);
      for (size_t i = 0; i < count; ++i) {
        if (ready_[i].load(std::memory_order_acquire)) {
          fn(entries_[i].prefix, entries_[i].hash);
        }
      }
    }

    // Calls fn(prefix, hash) for every published entry, then resets for the next cycle. Destructive
    // (resets tail_ and every visited ready_[i]) -- only call this when no writer can be
    // concurrently calling record(), i.e. from copy_live_entries()'s post-stop pass, strictly after
    // stop_writers_and_wait() has returned; see peek_live()'s comment above for why an earlier
    // version calling this pre-stop instead corrupted a live entry (entries_[0] held real key/hash
    // data while ready_[0] read false forever, with no thread left in record() to ever flip it).
    //
    // Two races this implementation closes even though its only current caller can't trigger
    // either (kept as defense in depth, not an unchecked assumption, so this stays safe to reuse
    // elsewhere without re-deriving the reasoning):
    //  1. A separately snapshotted "count = tail_.load()" followed by "tail_.store(0)" leaves a
    //     window where a writer's reserve (the tail_ CAS in record()) can land in between: it's
    //     invisible to this call's `count` (taken before the CAS) yet erased by the store(0) (which
    //     lands after), so no drain_and_clear() call, this one or any future one, ever observes it.
    //     Fixed by folding the read and the reset into one atomic exchange() -- a writer's CAS
    //     against the live value of `tail_` can only observe this exchange's result or its own
    //     success, never a torn mix, so it's deterministically on one side of the cut.
    //  2. record()'s reserve (the CAS) and publish (entries_[i]/ready_[i] store) are two separate
    //     steps, so an index included in `count` (because its CAS already landed) may not have
    //     published yet. Skipping such an index instead of waiting for it would lose it the same
    //     way: publish would land after this call has already moved on and reset ready_[i] to
    //     false. Waiting is always bounded -- record() has no blocking call between reserving and
    //     publishing.
    template <typename Fn>
    void drain_and_clear(Fn &&fn) {
      const size_t count = std::min<size_t>(tail_.exchange(0, std::memory_order_acq_rel), kCapacity);
      for (size_t i = 0; i < count; ++i) {
        while (!ready_[i].load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        fn(entries_[i].prefix, entries_[i].hash);
      }
      for (size_t i = 0; i < count; ++i) {
        ready_[i].store(false, std::memory_order_relaxed);
      }
    }

   private:
    struct Entry {
      StoreKey prefix{};
      uint64_t hash = 0;
    };
    static constexpr size_t kCapacity = ConfigT::TailEntryCapacityEntries;
    Entry *entries_;
    std::atomic<bool> *ready_;
    std::atomic<size_t> tail_{0};
  };
  mutable TailEntryTracker tail_entries_;
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

}  // namespace vmemkv
