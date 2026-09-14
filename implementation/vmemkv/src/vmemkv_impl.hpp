// vmemkv_impl.hpp - VMemKV coordinator: routes reads/writes across T1Index and T2FlatFile.
//
// The coordination logic itself lives in src/vmemkv/: hooks.hpp (checkpoint seams and the
// in-place-update barrier), t2_ownership.hpp (T2 segment accounting, capture watermark,
// barrier), read_path.hpp (base-region reads, scan batching), write_path.hpp (inline
// payloads, lock-free appends, in-place updates, stripe protocol, WAL batching),
// checkpoint_coordinator.hpp (checkpoint/reorganize orchestration), and
// defrag_controller.hpp (T2 defragmentation). This class owns lifetimes (T1/T2/WAL,
// stripes, background workers) and delegates every operation to those units.
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

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <vmemkv/config.hpp>

#include "checkpoint/checkpoint.hpp"
#include "core/cgroup_memory_throttle.hpp"
#include "core/reference_tracker.hpp"
#include "core/swap_check.hpp"
#include "t1_index/sharded_t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"
#include "vmemkv/checkpoint_coordinator.hpp"
#include "vmemkv/defrag_controller.hpp"
#include "vmemkv/hooks.hpp"
#include "vmemkv/read_path.hpp"
#include "vmemkv/t2_ownership.hpp"
#include "vmemkv/write_path.hpp"
#include "wal/wal.hpp"

inline constexpr std::size_t kCacheLineAlignment = 64;

struct alignas(kCacheLineAlignment) AlignedMutex {
  std::mutex mu;
};

namespace vmemkv {

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
// * merge_t1()   : Merges T1's append region into its sorted region. Zero I/O, T2 untouched.
//                  reorganize() is the legacy public alias.
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
    if constexpr (ConfigT::UseT1InlineValue) {
      parts.emplace_back("T1InlineValue");
    }
    if constexpr (ConfigT::UseReadPolicyRandomOnly) {
      parts.emplace_back("ReadRandom");
    }
    if constexpr (ConfigT::UseReadPolicySeqOnly) {
      parts.emplace_back("ReadSeq");
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

  using T1IndexT = vmemkv::ShardedT1Index<ConfigT>;
  // Re-exported for API compatibility (tests name ImplT::ReorgMode::Checkpoint).
  using ReorgMode = vmemkv::ReorgMode;

  // T1 payload codec (offsets + embedded 16-byte-granular size hints). Canonical values live
  // in t2_ownership.hpp's detail namespace; these aliases preserve the ImplT::X names.
  static constexpr uint64_t kSizeEmbeddingShift = detail::kPayloadSizeShift;
  static constexpr uint64_t kOffsetMask = detail::kPayloadOffsetMask;
  static constexpr uint64_t kBlockAlignment = detail::kRecordBlockAlignment;

  // T2's live mmap is MAP_SHARED (T2FlatFile's constructor); on-disk bytes below the manifest's
  // committed boundary are trustworthy, everything above it is not (low_level_design.md 5.1/5.3).
  // T1/T2 are wiped and rebuilt by replaying the WAL, unless a committed checkpoint (manifest) is
  // found, in which case T2FlatFile adopts its data file directly and T1 is fast-loaded from the
  // paired T1 checkpoint file, leaving only the rotated-down WAL tail to replay.
  //
  // t1_ is constructed with worker_threads=0 (deferred start): load_checkpoint_if_present()
  // below (single-threaded, safe) may wholesale-replace t1_'s initial shard via
  // load_from_checkpoint(), which is only safe before any other thread -- including t1_'s own
  // background maintenance workers -- could be touching this instance. t1_.start_workers() is
  // called right after that, *before* recover_from_wal(): replay calls put() same as live
  // traffic does, and needs a running worker pool backing its own AppendRegionFull retry/
  // backpressure to avoid livelocking with no worker to drain a full shard.
  VMemKVImpl(const std::filesystem::path &t2_path, uint64_t t2_bytes_capacity)
      : t1_(ConfigT::T1AppendCapacityEntries, ConfigT::T1ShardTargetSizeEntries, /*worker_threads=*/0),
        t2_(t2_path, t2_bytes_capacity, adopted_t2_bytes_used(t2_path)),
        wal_(vmemkv::derive_wal_path(t2_path)),
        t2_own_((t2_bytes_capacity + ConfigT::T2SegmentBytes - 1) / ConfigT::T2SegmentBytes) {
    // Larger-than-memory operation pages through swap; fail fast on an operator-set floor, warn
    // on no swap at all (see swap_check.hpp). First, before any mmap/recovery work.
    vmemkv::validate_swap_for_ltm();
    recovering_ = true;
    load_checkpoint_if_present(t2_path);
    t1_.start_workers();
    recover_from_wal();
    recovering_ = false;
    reorg_worker_ = std::jthread(&VMemKVImpl::reorg_worker_loop, this);  // Started after recovery completes.
    defrag_worker_ = std::jthread(&VMemKVImpl::defrag_worker_loop, this);
  }

  VMemKVImpl(const VMemKVImpl &) = delete;
  auto operator=(const VMemKVImpl &) -> VMemKVImpl & = delete;
  VMemKVImpl(VMemKVImpl &&) = delete;
  auto operator=(VMemKVImpl &&) -> VMemKVImpl & = delete;

  // Merges T1's sorted+append regions and, if `mode` is Checkpoint, also durabilizes T2's live
  // data and persists the result as a manifest-committed checkpoint. Called under the running
  // CAS guard; tests call this directly with injected hooks.
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
    requires CheckpointHook<PreStopHook> && CheckpointHook<PreFinishHook>
  void reorganize_internal(ReorgMode mode,
                           PreStopHook pre_stop_hook = PreStopHook{},
                           PreFinishHook pre_finish_hook = PreFinishHook{}) {
    checkpoint_detail::reorganize_internal(
        reorg_, t1_, t2_, wal_, t2_own_, t2_path(), recovering_, mode, pre_stop_hook, pre_finish_hook);
  }

  // T1-only in-memory merge. Never touches T2, never persists a checkpoint. Safe to call anytime.
  void merge_t1() { run_reorganize(ReorgMode::T1Only); }

  // Legacy public alias of merge_t1() (KVStore concept + StoreAdapter name it reorganize()).
  void reorganize() { merge_t1(); }

  // Forces a checkpoint cycle: durabilizes T2's live tail in place and persists the result as
  // a manifest-committed checkpoint.
  void checkpoint() { run_reorganize(ReorgMode::Checkpoint); }

  // Forces one defragmentation cycle now: relocates live records out of garbage-heavy frozen
  // segments (T2DefragSpaceOverheadPercent gates the background worker, not this call) and
  // hole-punches segments evacuated by the previous cycle. Waits its turn if the background
  // worker holds the cycle. Returns false while recovering, or where the filesystem cannot
  // punch holes (relocation alone cannot reclaim, so no cycle runs at all).
  auto defragment() -> bool {
    if (!defrag_detail::punch_supported(defrag_, t2_path())) {
      return false;
    }
    while (true) {
      if (recovering_) {
        return false;
      }
      if (defrag_detail::defrag_cycle<ConfigT>(defrag_,
                                               t1_,
                                               t2_,
                                               wal_,
                                               t2_own_,
                                               t2_path(),
                                               lock_stripe_fn(),
                                               maybe_reorg_fn(),
                                               reorg_.checkpoint_count.load(std::memory_order_relaxed),
                                               /*force=*/true)) {
        return true;
      }
      // Lost the single-flight race to the background worker: its cycle already covers this
      // request's work (same shared trigger state), so wait for it instead of spinning a
      // second one. Bounded by one cycle's duration.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  // Accessors for T1 (Index) and T2 (Flat File) layers (mainly for testing).
  auto t1() noexcept -> T1IndexT & { return t1_; }
  auto t1() const noexcept -> const T1IndexT & { return t1_; }
  auto t2() noexcept -> vmemkv::T2FlatFile & { return t2_; }
  auto t2() const noexcept -> const vmemkv::T2FlatFile & { return t2_; }

  auto get_statistics() const noexcept -> vmemkv::VMemKVStatistics {
    vmemkv::VMemKVStatistics stats;
    stats.t1_split_count = t1_.total_splits();
    stats.t1_last_split_pause_us = t1_.last_split_pause_us();
    stats.t1_last_split_pause_end_ns = t1_.last_split_pause_end_ns();
    stats.checkpoint_count = reorg_.checkpoint_count.load(std::memory_order_relaxed);
    vmemkv::snapshot_checkpoint_stats(reorg_, stats);
    stats.total_reorganize_wait_duration_us = reorg_.total_wait_us.load(std::memory_order_relaxed);
    stats.append_region_live_count = t1_.append_region_live_count();
    stats.append_region_peak_count = t1_.append_region_peak_count();
    stats.bulk_load_throttle_events = bulk_load_throttle_.throttle_events();
    stats.defrag_cycle_count = defrag_.cycle_count.load(std::memory_order_relaxed);
    stats.last_defrag_duration_us = defrag_.last_duration_us.load(std::memory_order_relaxed);
    stats.last_defrag_moved_bytes = defrag_.last_moved_bytes.load(std::memory_order_relaxed);
    stats.last_defrag_punched_bytes = defrag_.last_punched_bytes.load(std::memory_order_relaxed);
    stats.t2_live_bytes = t2_own_.live_total();
    return stats;
  }

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  // Inserts a new key-value pair.
  // - Ordering: T2 write must strictly precede the T1 write, so concurrent readers never see a
  //   dangling offset in T1. The WAL append comes last, after the mutation succeeds: logging
  //   first would durably persist a record for a write that never happened, and replaying it
  //   would hit the same throw on next restart, permanently bricking the store. Since T1/T2 are
  //   volatile and rebuilt from the WAL, a failed op is safe to simply not log.
  // - Thread-safety: guarded by slot-level spinlocks; the WAL wait and reorg check run after
  //   the stripe is released (see with_key_stripe()'s protocol).
  auto insert_impl(std::span<const std::byte> full_key, std::span<const std::byte> value) -> bool {
    auto check = maybe_reorg_fn();
    return vmemkv::with_key_stripe(
        lock_stripe_fn(),
        [this](Wal::PendingRecord *pending) { wal_.await_durable(pending); },
        check,
        full_key,
        [&]() -> vmemkv::StripeResult {
          if (t1_.get(full_key) != vmemkv::STORE_NOT_FOUND) {
            return {};
          }
          if (vmemkv::write_entry_lockfree<ConfigT>(t1_,
                                                    t2_,
                                                    t2_own_,
                                                    check,
                                                    full_key,
                                                    value,
                                                    typename T1IndexT::LookupResult{vmemkv::STORE_NOT_FOUND, 0})) {
            return {true, wal_.reserve_insert(full_key, value), true};
          }
          return {false, nullptr, true};
        });
  }

  // Retrieves a value and invokes callback with its raw bytes.
  // - Thread-safety: lock-free, concurrently readable during reorganization.
  // - Concurrency note (canonical explanation; other methods below point here): a T1 lookup names
  //   a T2 offset; that offset's bytes are protected by whichever of two independent mechanisms
  //   applies. Below base_boundary, the offset is immutable forever (see
  //   T2Memory::base_boundary's comment) and try_read_base_record() reads it seqlock-free. At or
  //   above it, the offset can be concurrently in-place-updated, so read_t2_record_seqlock()
  //   below re-checks a version counter around the read and retries on a torn observation.
  //   get_memory() is a plain pointer read: T2 never remaps to a different T2Memory
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
        std::array<std::byte, t1_detail::kInlineValueByteCount> stack_buf;
        vmemkv::copy_inline_value(res.payload_bits, size, stack_buf.data());
        callback(std::span<const std::byte>(stack_buf.data(), size));
        return true;
      }
    }

    const T2Memory *mem = t2_.get_memory();
    const uint64_t offset = res.payload_bits & kOffsetMask;

    // Base-region fast path: see try_read_base_record()'s doc comment. Bytes are immutable
    // once written, so callback can safely receive a span straight into the pread'd buffer --
    // no torn-read risk, no extra copy beyond what pread() itself did. thread_local/static for
    // the same reason as tl_get_value_buf below (per-thread reuse, no per-call heap
    // allocation).
    thread_local static std::vector<std::byte> tl_get_base_buf;
    if (const auto base_record = vmemkv::try_read_base_record<ConfigT>(mem, res.payload_bits, BaseReader::kGet,
                                                                       &tl_get_base_buf);
        base_record.has_value()) {
      if (vmemkv::byte_span_equal(base_record->key, full_key)) {
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
    // caller sees mid-read. thread_local (not a plain local) since concurrent callers on
    // different threads must not share one buffer; static so repeated calls on the same thread
    // reuse already-grown capacity instead of reallocating.
    thread_local static std::vector<std::byte> tl_get_value_buf;
    bool key_matches = vmemkv::read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(offset, mem); },
                                                      [&](const T2RecordView &record) -> bool {
                                                        if (!vmemkv::byte_span_equal(record.key, full_key)) {
                                                          return false;
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
  //   then updates T1's pointer. WAL append happens only after the mutation applies.
  // - Thread-safety: guarded by key hash locks; the WAL wait and reorg check run after the
  //   stripe is released (see with_key_stripe()'s protocol).
  auto update_impl(std::span<const std::byte> full_key, std::span<const std::byte> value) -> bool {
    auto check = maybe_reorg_fn();
    return vmemkv::with_key_stripe(
        lock_stripe_fn(),
        [this](Wal::PendingRecord *pending) { wal_.await_durable(pending); },
        check,
        full_key,
        [&]() -> vmemkv::StripeResult {
          // Fast path: an ungated lookup to see whether the *current* value is inline -- if so,
          // this call never touches T2 (write_entry_lockfree() re-decides inline-ness for the
          // new value fresh), so try_in_place_update() below has nothing to protect here.
          const auto quick = t1_.get_with_hash(full_key);
          if (quick.payload_bits == vmemkv::STORE_NOT_FOUND) {
            return {};
          }
          if (t1_detail::is_inline(quick.raw_hash)) {
            if (vmemkv::write_entry_lockfree<ConfigT>(t1_, t2_, t2_own_, check, full_key, value, quick)) {
              return {true, wal_.reserve_update(full_key, value), true};
            }
            return {false, nullptr, true};
          }
          const vmemkv::InPlaceUpdateResult result =
              vmemkv::try_in_place_update<ConfigT>(t1_, t2_, t2_own_, wal_, full_key, value);
          if (result.outcome == vmemkv::InPlaceOutcome::Aborted) {
            return {};
          }
          if (result.outcome == vmemkv::InPlaceOutcome::Applied) {
            return {true, result.pending, false};
          }
          if (vmemkv::write_entry_lockfree<ConfigT>(t1_, t2_, t2_own_, check, full_key, value, quick)) {
            return {true, wal_.reserve_update(full_key, value), true};
          }
          return {false, nullptr, true};
        });
  }

  // Logically removes a key from the store.
  // - Guarantees: Marks the key offset as STORE_NOT_FOUND in T1 (physical space reclamation is deferred to reorganize).
  // - Thread-safety: Thread-safe (guarded by key hash locks); the WAL wait runs after the
  //   stripe is released (see with_key_stripe()'s protocol).
  auto remove_impl(std::span<const std::byte> full_key) -> bool {
    return vmemkv::with_key_stripe(
        lock_stripe_fn(),
        [this](Wal::PendingRecord *pending) { wal_.await_durable(pending); },
        maybe_reorg_fn(),
        full_key,
        [&]() -> vmemkv::StripeResult {
          const auto res = t1_.get_with_hash(full_key);
          if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
            return {};
          }
          if (t1_.put(full_key, vmemkv::STORE_NOT_FOUND) == T1IndexT::PutResult::Applied) {
            t2_own_.note_replace(res.payload_bits, res.raw_hash, /*new_is_offset=*/false, 0);
            return {true, wal_.reserve_delete(full_key), false};
          }
          return {};
        });
  }

  // Bulk-loads `count` entries, bypassing the WAL for higher throughput than individual
  // insert_impl() calls. No durability guarantee: skipping the WAL means a crash after this
  // returns can lose everything loaded, unless the caller separately commits a checkpoint()
  // afterward. Still triggers ordinary T1-only reorganizes via maybe_reorganize_if_needed()
  // once the append region crosses its soft threshold. Under a cgroup v2 memory limit the
  // unbroken dirtying burst below can outrun synchronous direct reclaim and stall, so the loop
  // also paces itself against memory pressure (see CgroupMemoryThrottle -- inactive without
  // such a limit). Not safe to call concurrently with other writers.
  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t count, KeyFn &&make_key, ValueFn &&make_value) {
    auto check = maybe_reorg_fn();
    for (std::size_t index = 0; index < count; ++index) {
      maybe_reorganize_if_needed();
      const std::string key = make_key(index);
      const std::string value = make_value(index);
      vmemkv::write_entry_lockfree<ConfigT>(
          t1_,
          t2_,
          t2_own_,
          check,
          std::span<const std::byte>(reinterpret_cast<const std::byte *>(key.data()), key.size()),
          std::span<const std::byte>(reinterpret_cast<const std::byte *>(value.data()), value.size()),
          std::nullopt);
      bulk_load_throttle_.maybe_throttle(index);
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
    size_t total_count = 0;

    vmemkv::ScanActiveGuard<T1IndexT> scan_active_guard(t1_);

    // Bounded offset-ordered read batches; see flush_scan_batch().
    constexpr size_t kScanBatchSize = 128;
    std::vector<vmemkv::ScanBatchSlot> batch;
    batch.reserve(kScanBatchSize);
    std::vector<size_t> read_order;
    read_order.reserve(kScanBatchSize);
    std::vector<std::byte> arena;

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
                   std::array<std::byte, t1_detail::kInlineValueByteCount> stack_value;
                   vmemkv::copy_inline_value(payload, size, stack_value.data());
                   const std::span<const std::byte> key_view(index_key.data(), vmemkv::inline_key_len(index_key));
                   if (!vmemkv::key_in_range(key_view, lower_bound, upper_bound)) {
                     return;
                   }
                   callback(key_view, std::span<const std::byte>(stack_value.data(), size));
                   ++total_count;
                   return;
                 }
               }

               batch.push_back({payload, hash});
               ++total_count;
               if (batch.size() >= kScanBatchSize) {
                 vmemkv::flush_scan_batch<ConfigT>(batch, read_order, arena, t2_, lower_bound, upper_bound, callback);
               }
             });
    vmemkv::flush_scan_batch<ConfigT>(batch, read_order, arena, t2_, lower_bound, upper_bound, callback);

    return total_count;
  }

 private:
  // Shared wait/CAS/run/retry loop for merge_t1()/checkpoint() below; see
  // checkpoint_detail::run_reorganize().
  void run_reorganize(ReorgMode mode) {
    checkpoint_detail::run_reorganize(
        reorg_, t1_, t2_, wal_, t2_own_, t2_path(), recovering_, mode, NoOpPreStopHook{}, NoOpPreFinishHook{});
  }

  void maybe_reorganize_if_needed() { checkpoint_detail::maybe_reorganize_if_needed(reorg_, wal_); }

  auto maybe_reorg_fn() { return [this] { maybe_reorganize_if_needed(); }; }

  auto lock_stripe_fn() {
    return [this](std::span<const std::byte> key) -> std::mutex & { return key_mutex(key); };
  }

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

    vmemkv::ShardedT1CheckpointFile t1_chk(vmemkv::derive_t1_chk_path(t2_path));

    // O(N) memcpy-shaped conversion (on-disk order -> EntrySnapshot order), no hashing or
    // per-key insertion -- what makes fast boot fast (low_level_design.md 5.4), done once per
    // shard. Runs before t1_.start_workers() (see the constructor's own comment), so this is the
    // single-threaded window load_from_checkpoint()'s own contract requires.
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    std::vector<std::vector<EntrySnapshot>> per_shard_entries;
    per_shard_entries.reserve(t1_chk.shard_count());
    for (size_t shard_index = 0; shard_index < t1_chk.shard_count(); ++shard_index) {
      std::vector<EntrySnapshot> entries;
      const auto on_disk_entries = t1_chk.shard_entries(shard_index);
      entries.reserve(on_disk_entries.size());
      for (const auto &on_disk : on_disk_entries) {
        entries.push_back(EntrySnapshot{on_disk.key_prefix, on_disk.payload_bits, on_disk.hash});
        // Rebuild segment accounting from the checkpoint: tombstones and inline values address
        // no T2 segment. The WAL tail replayed next adjusts these counters incrementally
        // through the same write paths as live traffic.
        if (on_disk.payload_bits != vmemkv::STORE_NOT_FOUND && !t1_detail::is_inline(on_disk.hash)) {
          t2_own_.note_replace(vmemkv::STORE_NOT_FOUND, 0, /*new_is_offset=*/true, on_disk.payload_bits);
        }
      }
      per_shard_entries.push_back(std::move(entries));
    }

    t1_.load_from_checkpoint(t1_chk.boundaries(), per_shard_entries);
  }

  // Replays the current contents of wal_ into T1 (and, via the lock-free append path, T2) --
  // whether that's the full history or just the post-checkpoint tail is transparent here. Runs
  // before reorg_worker_ (VMemKVImpl's own, Checkpoint-triggering worker) is started
  // (constructor order: recovering_=true; ...; recover_from_wal(); recovering_=false; *then*
  // reorg_worker_ is move-assigned a real thread), so no other VMemKVImpl-level thread can be
  // touching the reorg state yet -- recovery relies on that rather than on the public
  // single-flight wrappers, which would be redundant synchronization against a competitor
  // that cannot exist at this point.
  //
  // t1_'s *own* background worker pool, in contrast, is already running by this point (started
  // right after load_checkpoint_if_present(), before this call -- see the constructor's own
  // comment): the append path below relies on it to drain a shard's append region if replay
  // pushes it toward AppendRegionFull, the same way live traffic does.
  void recover_from_wal() {
    auto check = maybe_reorg_fn();
    wal_.replay([&](vmemkv::WalRecordType type,
                    std::span<const std::byte> key,
                    std::span<const std::byte> value,
                    uint64_t /*lsn*/) {
      switch (type) {
        case vmemkv::WalRecordType::Insert:
        case vmemkv::WalRecordType::Update:
          // T1Index::put() overwrites in place if the key exists, so Insert and Update replay
          // identically -- last-writer-wins falls out of existing T1 semantics for free.
          vmemkv::write_entry_lockfree<ConfigT>(t1_, t2_, t2_own_, check, key, value, std::nullopt);
          break;
        case vmemkv::WalRecordType::Delete: {
          const auto old = t1_.get_with_hash(key);
          t1_.put(key, vmemkv::STORE_NOT_FOUND);  // Mirrors remove_impl's tombstone write.
          t2_own_.note_replace(old.payload_bits, old.raw_hash, /*new_is_offset=*/false, 0);
          break;
        }
      }
    });
  }

  // The inline-value stack buffers above (get_impl()/scan_impl()) are sized by T1Index's own
  // inline-value byte cap -- the single source of truth, so T1Index can never accept a payload
  // wider than these buffers.
  static_assert(t1_detail::kInlineValueByteCount == 8);

  void defrag_worker_loop(std::stop_token stop_token) {
    // Same bounded-poll rationale as reorg_worker_loop(): 1s granularity is invisible against a
    // cycle's own cost (full T1 scans plus relocation I/O), and the predicate itself is a few
    // atomic loads. A dedicated thread (rather than sharing reorg_worker_loop or T1's
    // shard-typed pool) keeps long relocation cycles from delaying checkpoint triggering.
    constexpr auto kIdlePollInterval = std::chrono::seconds(1);
    while (!stop_token.stop_requested()) {
      std::this_thread::sleep_for(kIdlePollInterval);
      if (stop_token.stop_requested()) {
        break;
      }
      if (recovering_) {
        continue;
      }
      if (!defrag_detail::defrag_wanted<ConfigT>(defrag_,
                                                 t2_own_,
                                                 t2_,
                                                 recovering_,
                                                 reorg_.checkpoint_count.load(std::memory_order_relaxed))) {
        continue;
      }
      try {
        defrag_detail::defrag_cycle<ConfigT>(defrag_,
                                             t1_,
                                             t2_,
                                             wal_,
                                             t2_own_,
                                             t2_path(),
                                             lock_stripe_fn(),
                                             maybe_reorg_fn(),
                                             reorg_.checkpoint_count.load(std::memory_order_relaxed),
                                             /*force=*/false);
      } catch (...) {
        // safe recovery in background
      }
    }
  }

  void reorg_worker_loop(std::stop_token stop_token) {
    // Polls on a short, fixed interval rather than blocking on a condition variable: bounds both
    // shutdown latency and reaction time to a real reorganize request without needing a wakeup
    // signal. Short enough to be imperceptible against reorganize's own multi-millisecond-plus
    // duration.
    constexpr auto kIdlePollInterval = std::chrono::milliseconds(10);
    while (!stop_token.stop_requested()) {
      if (!reorg_.requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kIdlePollInterval);
        continue;
      }
      if (stop_token.stop_requested()) {
        break;
      }
      reorg_.requested.store(false, std::memory_order_release);

      // CAS, not an unconditional store: the running flag is also claimed by explicit
      // reorganize()/checkpoint() callers, and the two must never both believe they hold it at
      // once. If an explicit call already holds it, this request is redundant -- skip this round.
      bool expected_running = false;
      if (!reorg_.running.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel)) {
        continue;
      }
      try {
        // reorg_.requested only ever fires on WAL-size pressure now (see
        // maybe_reorganize_if_needed()) -- T1 append-region maintenance is ShardedT1Index's own
        // responsibility (its background worker pool), not something this loop needs to drive
        // anymore. Re-check wal_over_threshold() at consumption time rather than trusting the
        // flag alone: it could have already resolved (e.g. a concurrent manual checkpoint() call
        // already rotated the WAL). auto_reorg_suppressed() lets a benchmark driver suppress this
        // for a bounded window -- with nothing else for this trigger to do instead, suppressed
        // just means skip this round.
        if (checkpoint_detail::wal_over_threshold(wal_) && !checkpoint_detail::auto_reorg_suppressed()) {
          checkpoint_detail::reorganize_internal(reorg_,
                                                 t1_,
                                                 t2_,
                                                 wal_,
                                                 t2_own_,
                                                 t2_path(),
                                                 recovering_,
                                                 ReorgMode::Checkpoint,
                                                 NoOpPreStopHook{},
                                                 NoOpPreFinishHook{});
        }
      } catch (...) {
        // safe recovery in background
      }
      reorg_.running.store(false, std::memory_order_release);
    }
  }

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

  T1IndexT t1_;
  vmemkv::T2FlatFile t2_;
  vmemkv::Wal wal_;
  // Paces bulk_load_impl() against cgroup v2 memory pressure (see its own comment). Member (not
  // a bulk_load-local) so the engagement count survives in get_statistics().
  vmemkv::CgroupMemoryThrottle bulk_load_throttle_;
  // T2 segment accounting, checkpoint watermark, and in-place-update barrier.
  T2Ownership<ConfigT> t2_own_;
  // Checkpoint/reorganize single-flight state and phase stats.
  ReorgState reorg_;
  // Defrag single-flight state, punch queue, and cycle stats.
  DefragState defrag_;

  bool recovering_ = false;  // True only during the constructor's initial WAL replay.

  // Declared last so destruction joins the workers before any state they touch. std::jthread's
  // destructor requests stop and joins; the loops above poll stop_token, so shutdown latency is
  // bounded by one poll interval. Listed defrag-first so reorg_ joins first, matching the
  // historical shutdown order.
  std::jthread defrag_worker_;
  std::jthread reorg_worker_;
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

}  // namespace vmemkv
