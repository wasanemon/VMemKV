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

// Test-only seam: fires once inside checkpoint_internal(), right before the
// ftruncate()/fsync()/open()/mmap() sequence that publishes the new T2Memory -- strictly before T1
// is ever touched (T1 is only published once, later, from a single I/O-free t1_.reorganize() call).
// Lets a test throw here to verify that any failure up to and including this point leaves T1
// completely untouched, with nothing to roll back. No-op in production.
struct NoOpPreFinishHook {
  void operator()() const {}
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

  // T2 is MAP_PRIVATE (volatile: writes never reach disk), so on-disk T2 bytes from a previous
  // run are meaningless on their own -- the WAL is the ultimate source of truth. T1/T2 are wiped
  // and rebuilt by replaying the WAL, unless a committed checkpoint (manifest) is found, in which
  // case T1/T2 are fast-loaded from it and only the rotated-down WAL tail needs replaying.
  VMemKVImpl(const std::filesystem::path &t2_path, uint64_t t2_bytes_capacity)
      : initial_generation_(vmemkv::T2Memory::allocate_generation()),
        t1_(initial_generation_),
        t2_(prepare_fresh_t2_file(t2_path), t2_bytes_capacity, initial_generation_),
        wal_(vmemkv::derive_wal_path(t2_path)) {
    // Unconditional: measured to help in-memory small-value Get/Hit by ~5% (low_level_design.md
    // 7.7). Removing it (an ablation prototyped and measured, then deleted -- see
    // docs/benchmark/20260810_t2_no_madvise_random.md) wins big on large-value LTM Get/Hit at low
    // concurrency (up to 8x at threads:1) by letting swap-in readahead batch multi-page reads, but
    // that win decays with concurrency and inverts by threads:32 (0.65-0.69x, worse than leaving
    // this on) as the readahead's excess bytes-read start competing with other threads for real
    // disk bandwidth. No known deployment runs at the low, fixed concurrency the win requires, so
    // the ablation isn't worth carrying as a toggle -- re-derive it from the doc above if that
    // ever changes.
    {
      auto mem = t2_.get_memory_handle();
      if (::madvise(mem->base, mem->capacity, MADV_RANDOM) != 0) {
        throw std::system_error(errno, std::generic_category(), "madvise MADV_RANDOM (initial)");
      }
    }
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

  VMemKVImpl(const VMemKVImpl &) = delete;
  auto operator=(const VMemKVImpl &) -> VMemKVImpl & = delete;
  VMemKVImpl(VMemKVImpl &&) = delete;
  auto operator=(VMemKVImpl &&) -> VMemKVImpl & = delete;

  // Merges T1's sorted+append regions and, when do_checkpoint is true, also durabilizes T2's live
  // tail and persists the result as a manifest-committed checkpoint (low_level_design.md 5.2).
  // Called under reorg_running_'s CAS guard (see reorganize()).
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void reorganize_internal(bool do_checkpoint,
                           PreStopHook pre_stop_hook = PreStopHook{},
                           PreFinishHook pre_finish_hook = PreFinishHook{}) {
    // Checkpointing during WAL replay is unsafe: recover_from_wal() runs inside
    // wal_.replay()'s callback, whose read loop keeps using an offset/file_size computed against
    // the pre-checkpoint file. Calling wal_.rotate() reentrantly from inside that same callback
    // (same thread, nested call) would swap Wal's fd_ to a shorter, rotated file and close the
    // old one out from under that in-flight loop. recover_from_wal() is the only caller that can
    // run while recovering_ is true, and it always passes do_checkpoint=false explicitly --
    // assert rather than silently override, so a future caller bug surfaces instead of being
    // papered over.
    assert((!recovering_ || !do_checkpoint) &&
           "must not request a checkpoint while recovering_ -- see recover_from_wal()'s call site");

    if (do_checkpoint) {
      checkpoint_internal(pre_stop_hook, pre_finish_hook);
    } else {
      // T1-only reorganize (zero I/O): T2 isn't touched, so the mapper leaves every entry's
      // payload/generation untouched. Overwriting every generation to "current T2" here would
      // silently erase a stale generation left by a past race, with no way to detect it later.
      t1_.reorganize([](std::span<typename T1IndexT::EntrySnapshot> /*merged*/) {},
                     typename T1IndexT::NoOpChkWriter{},
                     t2_.get_memory()->generation);
      reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
      reset_tombstone_counters();
    }
    scan_active_.store(false, std::memory_order_relaxed);
  }

 private:
  // reorganize_internal()'s do_checkpoint=true branch: durabilizes every live, tail-resident T2
  // record by writing it back to the exact offset it already occupies in T2's one persistent data
  // file (never a separate/cloned file, never a new offset), then republishes T1 and hot-swaps
  // T2's mapping so base_boundary advances to cover the newly-durable tail. Because no record ever
  // moves, T1's payload_bits are never rewritten here -- only the generation tag changes, to match
  // the freshly-mapped T2Memory. Finally persists the result as a manifest-committed checkpoint
  // and rotates the WAL.
  //
  // Structure: everything that can fail (real syscalls: pwrite/fsync/ftruncate/mmap) runs strictly
  // *before* T1 is ever touched. T1 is published exactly once, at the very end, via a single
  // t1_.reorganize() call whose offset_mapper does nothing but restamp a generation tag it already
  // computed -- so it cannot itself fail for any reason this function needs to guard against. This
  // means any exception anywhere above that point leaves T1 completely untouched: there is nothing
  // to roll back, and the existing run_reorganize()/reorg_worker_loop() catch/retry machinery is
  // already correct as-is (see their own comments).
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void checkpoint_internal(PreStopHook pre_stop_hook = PreStopHook{}, PreFinishHook pre_finish_hook = PreFinishHook{}) {
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    const uint64_t checkpoint_lsn = wal_.next_lsn() - 1;

    // The offset below which every byte is already durable from an earlier cycle -- constant for
    // the entire lifetime of the T2Memory being read here (see T2Memory::base_boundary's comment).
    const uint64_t old_base_boundary = t2_.get_memory()->base_boundary;
    capture_watermark_.store(old_base_boundary, std::memory_order_relaxed);

    // The store's one persistent T2 data file: created once (first-ever cycle, via O_CREAT) and
    // appended to in place on every subsequent cycle -- never cloned, replaced, or renamed. Bytes
    // below old_base_boundary are immutable and untouched by anything below; this cycle only ever
    // pwrite()s at offsets >= old_base_boundary, each one exactly where that record already lives
    // in T2's live (MAP_PRIVATE, thus never itself durable -- see T2FlatFile::map_file()'s
    // comment) mapping.
    const std::filesystem::path t2_chk_path = vmemkv::derive_t2_chk_path(t2_path());
    const int t2_fd = ::open(t2_chk_path.c_str(), O_RDWR | O_CREAT, 0600);
    if (t2_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open t2 checkpoint file");
    }
    struct FDGuard {
      int fd;
      ~FDGuard() {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    } fd_guard{t2_fd};

    // Tracks how far the periodic sync below has flushed+dropped, so the copy loop can bound this
    // process's page cache footprint instead of letting it grow unbounded until the final fsync --
    // that growth would otherwise compete with the live T2 mmap's resident pages for the same RAM.
    uint64_t bytes_synced = old_base_boundary;

    // Every T2 offset this cycle has already durabilized -- copy_live_entries()'s dedup between
    // its pre-stop and post-stop passes (both may observe the same tail entry). Keyed by offset,
    // not by (prefix, hash): a key redirected out-of-place between the two passes (its old offset
    // blocked by capture_watermark_, see that member's comment) gets a genuinely new offset that
    // must still be captured, even though the *key* was already durabilized once at its old one.
    std::unordered_set<uint64_t> durabilized;

    // Every (prefix, hash) the post-stop pass's destructive drain_and_clear() call has removed
    // from tail_entries_ so far this cycle -- see the catch block below for why this must be
    // restored if the cycle aborts before committing.
    std::vector<std::pair<StoreKey, uint64_t>> drained_this_cycle;

    // Reused across copy_live_entries() calls to avoid a per-record heap allocation.
    std::vector<std::byte> key_copy;
    std::vector<std::byte> value_copy;

    // Drains tail_entries_ (every key written into T2's tail since the last cycle -- see that
    // tracker's own comment) and pwrite()s any still-live, non-inline, tail-resident entry not
    // already in `durabilized` back to its own existing T2 offset -- no relocation, so no access-
    // order concern the way the old per-entry copy loop had. get_by_prefix_hash() re-resolves each
    // drained (prefix, hash) against T1's current state rather than trusting a value cached at
    // record() time, since the same key can be recorded multiple times (each write re-records it)
    // or deleted before this drains -- whichever state T1 shows right now is authoritative.
    // Purely read-only from T1's perspective -- does not publish anything, does not touch T1 at
    // all.
    // `destructive` must be false for the pre-stop call (writers are still live -- see
    // TailEntryTracker::drain_and_clear()'s contract) and true for the post-stop call (the only
    // point anything actually gets consumed/reset). Every (prefix, hash) a destructive call visits
    // is also recorded into `drained_this_cycle` -- see the catch block far below for why.
    auto copy_live_entries = [&](bool destructive) {
      struct Candidate {
        StoreKey prefix;
        uint64_t hash;
        uint64_t offset;
      };
      std::vector<Candidate> candidates;
      auto visit = [&](const StoreKey &prefix, uint64_t hash) {
        if (destructive) {
          drained_this_cycle.emplace_back(prefix, hash);
        }
        const auto res = t1_.get_by_prefix_hash(prefix, hash);
        if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
          return;  // Deleted since this was recorded.
        }
        if constexpr (ConfigT::UseT1InlineValue) {
          if (t1_detail::is_inline(res.raw_hash)) {
            return;  // A later write shrank this key's value below the inline threshold.
          }
        }
        const uint64_t offset = res.payload_bits & kOffsetMask;
        if (offset < old_base_boundary) {
          return;  // Already durable from an earlier cycle.
        }
        if (durabilized.contains(offset)) {
          return;  // Already durabilized by an earlier call to copy_live_entries() this cycle.
        }
        candidates.push_back({prefix, res.raw_hash, offset});
      };
      if (destructive) {
        tail_entries_.drain_and_clear(visit);
      } else {
        tail_entries_.peek_live(visit);
      }

      if (candidates.empty()) {
        return;
      }

      // Ascending-offset order: kernel/madvise-readahead-friendly for the seqlock reads below
      // (against the live mmap, an effectively-random access pattern otherwise under an LTM-scale
      // corpus) and lets maybe_sync_and_drop_checkpoint_cache()'s running high-water mark actually
      // bound this loop's page cache footprint, the same role it plays in the append-only paths
      // that also call it.
      std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.offset < b.offset;
      });

      T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
      for (const auto &candidate : candidates) {
        // Copy key+value+alloc_len out under the seqlock: this record can be concurrently mutated
        // by update_impl()'s in-place T2 path (key_lock does not serialize against reorganize(),
        // by design). alloc_len (not value_len) is what the on-disk footprint at this offset was
        // originally sized to -- must be preserved as-is so this pwrite() never changes the
        // record's footprint, only its contents, and any later record already sharing this
        // generation's tail stays at the offset it was allocated. t2_.at() is called inside
        // read_t2_record_seqlock() (as AtFunc) rather than once beforehand, since key_len/
        // value_len/alloc_len are unsynchronized and a stale read once wouldn't be caught by the
        // version recheck -- see that function's comment. key_copy/value_copy are reused across
        // calls purely to avoid a per-record heap allocation.
        //
        // Claimed via capture_watermark_ *before* this read (see that member's comment): once
        // this store() is visible, any in-place update racing this exact offset is guaranteed to
        // see allow_in_place==false and redirect out-of-place instead, so the seqlock read below
        // observes the last possible in-place write to this offset, never one that lands after.
        // candidates are processed in ascending-offset order (see the sort above), so this is
        // always a forward-only advance.
        capture_watermark_.store(candidate.offset + 1, std::memory_order_release);

        uint32_t alloc_len = 0;
        uint64_t version = 0;
        read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(candidate.offset, mem); },
                               [&](const T2RecordView &record) -> bool {
                                 key_copy.assign(record.key.begin(), record.key.end());
                                 value_copy.assign(record.value.begin(), record.value.end());
                                 alloc_len = record.header->alloc_len;
                                 version = record.header->version;
                                 return true;
                               });

        const uint64_t written_through =
            pwrite_record_at_offset(fd_guard.fd, candidate.offset, key_copy, value_copy, alloc_len, version);
        maybe_sync_and_drop_checkpoint_cache(fd_guard.fd, written_through, bytes_synced);
        durabilized.insert(candidate.offset);
      }
    };

    try {
      // Pre-stop pass: a pure performance optimization, not a correctness requirement -- see
      // stop_writers_and_wait() below for why the amount of work left for the post-stop pass
      // matters. Safe to run any number of times (including zero); T1 is not touched. Must use the
      // non-destructive peek_live() (destructive=false), not drain_and_clear() -- writers are still
      // live here, and drain_and_clear()'s reset races them (see TailEntryTracker::drain_and_clear()'s
      // contract comment).
      copy_live_entries(/*destructive=*/false);

      // Closes the residual window between here and T1's publish at the very end: while
      // writer_stop_ is set, acquire_write_handle() defers new writers instead of handing out a
      // handle to this about-to-retire generation, so no new T1 entry naming it can appear from
      // here on -- guaranteeing the post-stop pass below sees the complete, final live set.
      // stop_writers_and_wait() blocks until every writer that already held a handle -- i.e.
      // started before the flag went up -- has released it, which per that handle's contract only
      // happens after its T1 publish attempt has returned; that straggler's entry is then simply
      // part of what the post-stop scan below observes, needing no special handling.
      //
      // Deliberately kept as short as possible: writer_stop_ blocks new writes to *any* key in
      // the store, not just ones in the tail -- so everything between here and resume_writers()
      // below should be as fast as possible, which is exactly why the pre-stop pass above exists,
      // to shrink how much the post-stop pass still has to do.
      const vmemkv::T2Memory *pre_swap_mem = t2_.get_memory();
      // TEST-ONLY: lets a test register a writer's handle to pre_swap_mem before the stop flag
      // goes up. No-op in production -- see NoOpPreStopHook.
      pre_stop_hook();
      t2_.stop_writers_and_wait(pre_swap_mem);
      struct WriterResumeGuard {
        vmemkv::T2FlatFile *t2;
        ~WriterResumeGuard() { t2->resume_writers(); }
      } resume_guard{&t2_};

      // Post-stop pass: guaranteed complete -- nothing more can appear until resume_writers()
      // runs, below. The only call site where destructive=true (drain_and_clear()) is safe -- no
      // writer can reach record() until resume_writers() runs, above.
      copy_live_entries(/*destructive=*/true);

      // The new base_boundary: with nothing relocated, it's simply how far the live bump
      // allocator had advanced by the time writers stopped above -- everything below this offset
      // is now durably on disk (every live record in [old_base_boundary, new_base_boundary) was
      // just pwritten by one of the two passes above; every dead one was skipped and is simply
      // never referenced again).
      const uint64_t new_base_boundary = t2_.get_memory()->bytes_used.load(std::memory_order_acquire);

      uint64_t new_capacity = t2_.bytes_capacity();
      if (new_base_boundary > new_capacity) {
        new_capacity = new_base_boundary;
      }

      // Last chance to reclaim this cycle's page cache footprint before T1's own checkpoint write
      // (inside the offset_mapper call at the very end) needs headroom.
      if (new_base_boundary > bytes_synced && ::fdatasync(fd_guard.fd) == 0) {
        ::posix_fadvise(fd_guard.fd,
                        static_cast<off_t>(bytes_synced),
                        static_cast<off_t>(new_base_boundary - bytes_synced),
                        POSIX_FADV_DONTNEED);
        bytes_synced = new_base_boundary;
      }

      // TEST-ONLY: lets a test throw here, strictly before T1 is ever touched. No-op in
      // production -- see NoOpPreFinishHook.
      pre_finish_hook();

      if (::ftruncate(fd_guard.fd, static_cast<off_t>(new_capacity)) != 0) {
        throw std::system_error(errno, std::generic_category(), "ftruncate t2 checkpoint file");
      }
      if (::fsync(fd_guard.fd) != 0) {
        throw std::system_error(errno, std::generic_category(), "fsync t2 checkpoint file");
      }

      // A second, independent fd for the fresh mmap below: mmap_t2_memory() always closes the fd
      // it's handed, and fd_guard's own fd must stay open (and pointing at the same, still-in-use
      // file) through this function's remaining fsync-adjacent bookkeeping.
      const int map_fd = ::open(t2_chk_path.c_str(), O_RDWR);
      if (map_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open t2 checkpoint file for mmap");
      }

      // Reserved once and stamped on both sides of the adopted pair, same as
      // load_checkpoint_if_present(). Fully constructed and ready, just not yet installed as live
      // -- everything from here on (the offset_mapper below, and swap_memory() after it) is I/O-
      // free and cannot throw.
      const uint64_t t2_pair_generation = vmemkv::T2Memory::allocate_generation();
      std::unique_ptr<vmemkv::T2Memory> new_mem =
          mmap_t2_memory(map_fd, new_capacity, "mmap t2 checkpoint (in-place)", t2_pair_generation, new_base_boundary);

      // The only place T1 gets published this cycle. No record's payload_bits ever changes here
      // (nothing was relocated) -- the only thing that needs updating is each live entry's
      // generation tag, to match the freshly-mapped T2Memory it must now be read against.
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
          entry.generation = t2_pair_generation;
        }
      };
      auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) {
        vmemkv::write_t1_checkpoint(vmemkv::derive_t1_chk_path(t2_path()), merged);
      };
      t1_.reorganize(offset_mapper_fn, chk_writer_fn, t2_pair_generation);

      t2_.swap_memory(std::move(new_mem));
      // Resumes writers as soon as the new generation is live, rather than leaving them blocked
      // through the manifest write/WAL rotate below -- WriterResumeGuard's destructor still
      // covers the exception path (e.g. if swap_memory() itself throws), and a redundant
      // resume_writers() there is harmless (plain store(false)).
      t2_.resume_writers();

      vmemkv::write_manifest(vmemkv::derive_manifest_path(t2_path()), checkpoint_lsn, new_base_boundary);
      wal_.rotate(checkpoint_lsn);

      reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
      reorg_t2_count_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      // Nothing to clean up on the file itself: every pwrite() above landed at an offset >=
      // old_base_boundary, and the manifest (rewritten only on the success path above) still
      // names old_base_boundary as the durable boundary -- so those bytes are simply orphaned,
      // unreferenced-by-anything, and get overwritten by the next successful cycle's own pwrite()
      // at the same offsets.
      //
      // Restores every entry the post-stop pass's drain_and_clear() permanently removed from
      // tail_entries_ this cycle. T1 was never touched (this function's own top comment: every
      // real failure runs strictly before T1 is ever published), so these keys are still exactly
      // as tail-resident/undurabilized as they were before this cycle started -- the next cycle
      // must see them again, or an entry's generation stamp silently goes stale the next time
      // it's actually durabilized, sending try_in_place_update()'s generation-pairing check into
      // an unwinnable retry loop (reproduced directly via a checkpoint() following an aborted
      // cycle).
      for (const auto &[prefix, hash] : drained_this_cycle) {
        tail_entries_.record(prefix, hash);
      }
      // Un-claim whatever this aborted attempt had blocked: nothing durable actually happened
      // (see above), so there is no reason for in-place updates to keep redirecting out-of-place
      // for offsets this cycle never actually captured.
      capture_watermark_.store(old_base_boundary, std::memory_order_relaxed);
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
      // T2's base region is read through its own seqlock-free mmaps (see
      // T2Memory::base_boundary's comment), which is only safe if base offsets never change after
      // being written -- so an in-place update targeting the base is redirected out-of-place
      // (falls through to write_entry_lockfree()) instead. base_boundary is constant for this
      // T2Memory's entire lifetime (see its own declaration), so this check needs no additional
      // synchronization beyond the plain read `mem` already required.
      //
      // capture_watermark_ extends the same redirect to a record checkpoint_internal() has
      // already claimed *this cycle*, before base_boundary itself has advanced to cover it (see
      // that member's own comment) -- without this, an in-place update landing between
      // checkpoint_internal()'s claim and its actual durabilizing read/write of this offset could
      // mutate bytes it's about to (or already did) capture, silently reverting a live read back
      // to the pre-update value the moment the new generation publishes (proven via a direct
      // repro, not just reasoned about -- see the crash-recovery regression test for this).
      const bool allow_in_place =
          offset >= mem->base_boundary && offset >= capture_watermark_.load(std::memory_order_acquire);
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
        const bool do_checkpoint = decide();
        try {
          reorganize_internal(do_checkpoint);
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
    run_reorganize([] { return false; }, /*force_run=*/false);
  }

  // Placeholder: T2 has no compaction mechanism (low_level_design.md 5.2 notes why -- durably
  // reclaiming fragmented T2 space needs either a relocating rewrite or a reflink-capable
  // filesystem, neither of which checkpoint_internal()'s in-place design provides). Currently
  // identical to reorganize() -- kept as its own entry point so a future compaction
  // implementation has a stable call site to land in, and so callers that explicitly ask for GC
  // are not silently no-ops when there happens to be nothing in T1's append region.
  void defragment() {
    run_reorganize([] { return false; }, /*force_run=*/true);
  }

  // Forces a checkpoint_internal() cycle: durabilizes T2's live tail in place and persists the
  // result as a manifest-committed checkpoint.
  void checkpoint() {
    run_reorganize([] { return true; }, /*force_run=*/true);
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
    // base_boundary is constant for this T2Memory's entire lifetime (see its own declaration), so
    // this plain read needs no extra synchronization beyond `mem` itself.
    const uint64_t base_boundary = mem->base_boundary;
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
      // T2Memory::base (MAP_PRIVATE, concurrently update_value_at()-writable); the seqlock's
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

  // T2 is MAP_PRIVATE (volatile), so any bytes surviving on disk from a previous run are
  // meaningless -- they must never be trusted. Called from the constructor's initializer list
  // before t2_ is constructed, so this must be static.
  static auto prepare_fresh_t2_file(const std::filesystem::path &t2_path) -> const std::filesystem::path & {
    std::error_code error_code;
    std::filesystem::remove(t2_path, error_code);
    return t2_path;
  }

  // Whether enough WAL has accumulated since the last checkpoint to need truncating. Used only by
  // reorg_worker_loop() to decide whether to call checkpoint()-equivalent behavior.
  auto wal_over_threshold() const -> bool { return wal_.size_bytes() >= ConfigT::WalMaxBytesSinceCheckpoint; }

  // Maps `capacity` bytes of `file_descriptor` MAP_PRIVATE and wraps the result in a T2Memory,
  // closing the fd in all cases. Shared by reorganize_internal()'s T2 rebuild and
  // load_checkpoint_if_present()'s fast-boot adoption. `bytes_used` is baked into the T2Memory
  // itself rather than set separately -- see T2Memory::bytes_used's declaration for why a mem
  // pointer and its byte-usage counter must never be independently settable.
  static auto mmap_t2_memory(int file_descriptor,
                             uint64_t capacity,
                             const char *what,
                             uint64_t generation,
                             uint64_t bytes_used) -> std::unique_ptr<vmemkv::T2Memory> {
    void *mapped = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE, file_descriptor, 0);
    const int mmap_errno = errno;  // Captured before close(), which on success must not clobber it.
    if (mapped == MAP_FAILED) {
      ::close(file_descriptor);
      throw std::system_error(mmap_errno, std::generic_category(), what);
    }
    // madvise is per-VMA, not per-inode, so this must be re-applied to every fresh mapping this
    // function produces (every reorganize/checkpoint remaps T2) -- it does not carry over from the
    // constructor's initial mmap. See the constructor's identical call for why this stays
    // unconditional.
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
      void *base_mapped_scan = ::mmap(nullptr, capacity, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
      if (base_mapped_scan != MAP_FAILED) {
        base_mmap_scan_ptr = static_cast<std::byte *>(base_mapped_scan);
      }

      void *base_mapped_scan_seq = ::mmap(nullptr, capacity, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
      if (base_mapped_scan_seq != MAP_FAILED) {
        if (::madvise(base_mapped_scan_seq, capacity, MADV_SEQUENTIAL) == 0) {
          base_mmap_scan_seq_ptr = static_cast<std::byte *>(base_mapped_scan_seq);
        } else {
          ::munmap(base_mapped_scan_seq, capacity);
        }
      }

      // Unconditional (not gated by any config tag): eagerly install page table entries for just
      // the currently-valid prefix [0, bytes_used) in one bulk call per mapping, not via
      // MAP_POPULATE on the mmap calls themselves (which would eagerly fault in the entire
      // capacity). This
      // matters even for already page-cache-resident data: mmap() doesn't share page *table*
      // entries across separate VMAs of the same file, so each fresh mapping still needs its own
      // per-page minor fault to install a PTE the first time it's touched, cache-resident or not.
      // get_impl() reads single-page records straight through base_mmap_scan_seq, and a
      // Uniform-distributed workload touching most of a large corpus pays for *every one* of
      // those first-touch minor faults during the timed benchmark itself if a mapping it reads
      // isn't pre-warmed -- measured to regress 1KB In-Memory Get/Hit/Uniform by ~250x without
      // this. Both mappings are warmed (not just whichever one this generation's corpus happens
      // to use), since which one is actually hit is now decided per record, not once here.
      // Best-effort: a failure here just means the first touch of each page pays an ordinary (if
      // still page-cache-resident-cheap) minor fault instead of finding it pre-installed.
      if (bytes_used > 0) {
        if (base_mmap_scan_ptr != nullptr) {
          ::madvise(base_mmap_scan_ptr, bytes_used, MADV_POPULATE_READ);
        }
        if (base_mmap_scan_seq_ptr != nullptr) {
          ::madvise(base_mmap_scan_seq_ptr, bytes_used, MADV_POPULATE_READ);
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
  // sorted_region and T2 mapping directly. Leaves t1_/t2_ untouched if no manifest exists yet --
  // the ordinary fresh-start case, where recover_from_wal()'s full replay from LSN 1 is already
  // correct.
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

    const std::filesystem::path t2_chk_path = vmemkv::derive_t2_chk_path(t2_path);
    const int map_fd = ::open(t2_chk_path.c_str(), O_RDWR);
    if (map_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open t2 checkpoint");
    }
    struct stat file_stat {};
    if (::fstat(map_fd, &file_stat) != 0) {
      const int err = errno;
      ::close(map_fd);
      throw std::system_error(err, std::generic_category(), "fstat t2 checkpoint");
    }
    const auto capacity = static_cast<uint64_t>(file_stat.st_size);
    if (manifest->t2_bytes_used > capacity) {
      ::close(map_fd);
      throw std::runtime_error("manifest t2_bytes_used exceeds t2 checkpoint file size");
    }

    // Reserved once and stamped on both sides of the adopted pair, same as reorganize_internal().
    const uint64_t t2_pair_generation = vmemkv::T2Memory::allocate_generation();
    std::unique_ptr<vmemkv::T2Memory> new_mem =
        mmap_t2_memory(map_fd, capacity, "mmap t2 checkpoint", t2_pair_generation, manifest->t2_bytes_used);

    // O(N) memcpy-shaped conversion (on-disk order -> EntrySnapshot order), no hashing or
    // per-key insertion -- what makes fast boot fast (low_level_design.md 5.4). Every entry in
    // one checkpoint was written against the same T2 generation it rebuilt, so all loaded entries
    // are uniformly stamped with the freshly-reserved t2_pair_generation.
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    std::vector<EntrySnapshot> entries;
    entries.reserve(t1_chk.entries().size());
    for (const auto &on_disk : t1_chk.entries()) {
      entries.push_back(EntrySnapshot{on_disk.key_prefix, on_disk.payload_bits, on_disk.hash, t2_pair_generation});
    }

    t2_.swap_memory(std::move(new_mem));
    t1_.load_sorted_region_from_checkpoint(entries, t2_pair_generation);
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
        reorganize_internal(/*do_checkpoint=*/false);
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

  // Writes a record to its own existing T2 offset (checkpoint_internal()'s durabilization
  // pwrite -- never a relocation: `offset` and `alloc_len` are exactly what the live record
  // already has, captured under a seqlock by the caller). Preserving alloc_len as-is (not
  // shrinking it to value.size(), the way a compacting rewrite would) is what keeps this write's
  // footprint identical to the slot already reserved for this record, so it never encroaches on a
  // neighboring record's bytes. Returns offset + the aligned footprint just written, i.e. how far
  // this pwrite() advanced the durable region -- used by the caller to bound its periodic sync.
  static auto pwrite_record_at_offset(int file_descriptor,
                                      uint64_t offset,
                                      std::span<const std::byte> key,
                                      std::span<const std::byte> value,
                                      uint32_t alloc_len,
                                      uint64_t version) -> uint64_t {
    ValueRecordHeader header;
    header.key_len = static_cast<uint32_t>(key.size());
    header.value_len = static_cast<uint32_t>(value.size());
    header.alloc_len = alloc_len;
    header.version = version;

    const uint64_t raw_len = sizeof(header) + key.size() + alloc_len;
    const uint64_t aligned_len = align_up(raw_len);

    std::vector<std::byte> buffer(aligned_len, std::byte{0});
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), key.data(), key.size());
    if (!value.empty()) {
      std::memcpy(buffer.data() + sizeof(header) + key.size(), value.data(), value.size());
    }
    // Bytes from value.size() to alloc_len, plus alignment padding, stay zero -- never meaningful
    // (only the first value_len bytes of the value region are ever read).

    if (::pwrite(file_descriptor, buffer.data(), buffer.size(), static_cast<off_t>(offset)) !=
        static_cast<ssize_t>(buffer.size())) {
      throw std::system_error(errno, std::generic_category(), "pwrite record");
    }
    return offset + aligned_len;
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
        // Explicit priority (tail-tracker pressure wins over WAL size). Note reorg_requested_
        // (this wakeup's trigger) only ever fires on append-region/delete pressure (see
        // maybe_reorganize_if_needed()) -- tail_entries_/WAL-size changes never themselves cause a
        // wakeup, so this only checks them "while already awake anyway."
        if (tail_entries_.near_capacity() || wal_over_threshold()) {
          reorganize_internal(/*do_checkpoint=*/true);
        } else {
          reorganize_internal(/*do_checkpoint=*/false);
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
    // the append region (drained by *any* reorganize_internal() call), only a checkpoint cycle
    // drains tail_entries_ -- see reorg_worker_loop()'s priority check, which fires a checkpoint
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

  // Write-side barrier for checkpoint_internal()'s tail durabilization (see its own comment and
  // try_in_place_update()'s allow_in_place check). Reset to old_base_boundary at the start of
  // each cycle; checkpoint_internal() advances it strictly forward, past a record's offset,
  // *before* reading that record -- so any in-place update whose allow_in_place check observes
  // offset < capture_watermark_ is guaranteed to be redirected out-of-place instead of mutating a
  // record checkpoint_internal() is about to (or already did) durabilize. Conservative by
  // construction: it only ever needs to be *at least* as far along as what's genuinely durable
  // this cycle, never exactly so, so races with the claim step never need to be resolved -- an
  // update that's blocked slightly earlier than strictly necessary just takes the always-safe
  // out-of-place path instead.
  mutable std::atomic<uint64_t> capture_watermark_{0};

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
