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

// Test-only seam: fires once inside rebuild_t2_and_maybe_checkpoint(), right before
// T2FlatFile::stop_writers_and_wait() is called (i.e. while the about-to-be-retired generation is
// still handed out normally by acquire_write_handle()). Lets a test deterministically get a
// writer's T2MemoryHandle registered *before* the stop flag goes up, so the subsequent
// stop-and-wait has a real, still-in-flight writer to wait for -- exercising the exact handshake
// that closes the residual-window race (see T2FlatFile::stop_writers_and_wait()'s declaration).
// No-op in production.
struct NoOpPreStopHook {
  void operator()() const noexcept {}
};

// Test-only seam: fires once inside rebuild_t2_and_maybe_checkpoint(), right before the
// close()/truncate()/open()/mmap() sequence that finishes the new T2 file -- strictly before T1 is
// ever touched (T1 is only published once, later, from a single I/O-free t1_.reorganize() call).
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
    if constexpr (ConfigT::UsePrefaulting) {
      parts.emplace_back("Prefaulting");
    }
    if constexpr (ConfigT::UseScanBaseSequential) {
      parts.emplace_back("ScanBaseSequential");
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

  // Reclaims fragmented disk space from deleted/updated T2 records. Outside of initial WAL
  // recovery, this doubles as a checkpoint (low_level_design.md 5.2): merges T1's sorted+append
  // regions, writes live records to a temp T2 file, hot-swaps the T2 mapping and republishes T1,
  // then (unless recovering) persists the result as a manifest-committed checkpoint and rotates
  // the WAL. Called under reorg_running_'s CAS guard (see reorganize()).
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void reorganize_internal(bool do_t2_rebuild,
                           bool do_checkpoint,
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

    if (do_t2_rebuild) {
      rebuild_t2_and_maybe_checkpoint(do_checkpoint, pre_stop_hook, pre_finish_hook);
    } else if (do_checkpoint) {
      checkpoint_t1_only_with_delta_flush();
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
  // reorganize_internal()'s do_t2_rebuild=true branch: performs a full T2 GC/rebuild (dead
  // entries dropped, live entries relocated to a fresh temp file in old-physical-offset order),
  // then republishes T1 with the new offsets and hot-swaps T2's mapping. When do_checkpoint is
  // true this also persists the result as a manifest-committed checkpoint and rotates the WAL;
  // recover_from_wal()'s livelock-avoidance rebuild (reclaiming T1 append-region capacity so
  // replay can continue) always passes do_checkpoint=false, which reorganize_internal() asserts.
  //
  // Structure: everything that can fail (real syscalls: open/write/truncate/mmap) runs strictly
  // *before* T1 is ever touched. T1 is published exactly once, at the very end, via a single
  // t1_.reorganize() call whose offset_mapper does nothing but look up already-computed results
  // -- so it cannot itself fail for any reason this function needs to guard against. This means
  // any exception anywhere above that point leaves T1 completely untouched: there is nothing to
  // roll back, and the existing run_reorganize()/reorg_worker_loop() catch/retry machinery is
  // already correct as-is (see their own comments).
  template <typename PreStopHook = NoOpPreStopHook, typename PreFinishHook = NoOpPreFinishHook>
  void rebuild_t2_and_maybe_checkpoint(bool do_checkpoint,
                                       PreStopHook pre_stop_hook = PreStopHook{},
                                       PreFinishHook pre_finish_hook = PreFinishHook{}) {
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    const uint64_t checkpoint_lsn = do_checkpoint ? wal_.next_lsn() - 1 : 0;
    const uint64_t t1_generation = checkpoint_lsn;

    const std::filesystem::path t2_chk_final_path = do_checkpoint ? vmemkv::derive_t2_chk_path(t2_path(), t1_generation)
                                                                  : t2_.path().parent_path() / "t2_flat.recovery_tmp";
    const std::filesystem::path temp_path_t2 =
        do_checkpoint ? std::filesystem::path(t2_chk_final_path.string() + ".tmp") : t2_chk_final_path;

    std::error_code error_code;
    std::filesystem::remove(temp_path_t2, error_code);

    const int temp_fd = ::open(temp_path_t2.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (temp_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open temp file");
    }

    uint64_t next_bytes_used = 0;
    // Tracks how far the periodic sync below has flushed+dropped, so the copy loop can bound the
    // temp T2 file's page cache footprint instead of letting it grow unbounded until the final
    // fsync -- that growth would otherwise compete with the live T2 mmap's resident pages for the
    // same RAM.
    uint64_t next_bytes_synced = 0;
    struct FDGuard {
      int fd;
      ~FDGuard() {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    } fd_guard{temp_fd};

    // Distinct from `generation`/checkpoint_lsn (a checkpoint's file generation, not
    // guaranteed unique per rebuild): this is what get_impl()/scan_impl()/update_impl()
    // validate against T2Memory::generation to detect a reorganize landing mid-read.
    const uint64_t t2_pair_generation = vmemkv::T2Memory::allocate_generation();

    // Maps a live entry's (T1 key prefix, hash) -- the same pair EntrySnapshot itself carries --
    // to where its bytes ended up in the temp file below. Built entirely by copy_live_entries()
    // (read-only from T1's perspective) before T1 is ever touched; consulted by the single,
    // I/O-free offset_mapper passed to t1_.reorganize() at the very end. `hash` alone drives
    // hashing here (it's already a well-distributed hash of the real key); the prefix is only
    // needed for equality, to disambiguate the astronomically rare case of two different keys
    // sharing both a 16-byte prefix and a hash collision.
    struct RelocationKeyHash {
      auto operator()(const std::pair<StoreKey, uint64_t> &k) const noexcept -> size_t {
        return std::hash<uint64_t>{}(k.second);
      }
    };
    std::unordered_map<std::pair<StoreKey, uint64_t>, uint64_t, RelocationKeyHash> relocated;

    // Reused across copy_live_entries() calls to avoid a per-record heap allocation.
    std::vector<std::byte> key_copy;
    std::vector<std::byte> value_copy;

    // Reads every currently-live, non-inline entry via the public, lock-free t1_.scan() API and
    // copies any not already in `relocated` into the temp file -- in old-physical-offset order,
    // not `merged`'s key order, for the same reason the old per-entry code sorted: T2 physical
    // offset is unrelated to key order, so reading in key order is an effectively-random access
    // pattern under an LTM-scale mmap; old-offset order is monotonically increasing and
    // kernel/madvise-readahead-friendly. Purely read-only from T1's perspective -- does not
    // publish anything, does not touch T1 at all.
    static constexpr std::array<std::byte, kStoreKeyBytes> kMaxKeyBytes = [] {
      std::array<std::byte, kStoreKeyBytes> bytes{};
      bytes.fill(std::byte{0xFF});
      return bytes;
    }();
    auto copy_live_entries = [&] {
      struct Candidate {
        StoreKey prefix;
        uint64_t hash;
        uint64_t old_offset;
      };
      std::vector<Candidate> candidates;
      t1_.scan(std::span<const std::byte>{},
               std::span<const std::byte>(kMaxKeyBytes),
               [&](std::span<const std::byte> index_key, uint64_t payload, uint64_t hash, uint64_t /*t2_generation*/) {
                 if (payload == vmemkv::STORE_NOT_FOUND) {
                   return;
                 }
                 if constexpr (ConfigT::UseT1InlineValue) {
                   if (t1_detail::is_inline(hash)) {
                     return;
                   }
                 }
                 StoreKey prefix;
                 std::copy(index_key.begin(), index_key.end(), prefix.begin());
                 if (relocated.contains({prefix, hash})) {
                   return;  // Already copied by an earlier call to copy_live_entries().
                 }
                 candidates.push_back({prefix, hash, payload & kOffsetMask});
               });

      if (candidates.empty()) {
        return;
      }

      std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.old_offset < b.old_offset;
      });

      T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
      for (const auto &candidate : candidates) {
        // Copy key+value out under the seqlock: this record can be concurrently mutated by
        // update_impl()'s in-place T2 path (key_lock does not serialize against reorganize(), by
        // design). t2_.at() is called inside read_t2_record_seqlock() (as AtFunc) rather than
        // once beforehand, since key_len/value_len are unsynchronized and a stale size read once
        // wouldn't be caught by the version recheck -- see that function's comment. key_copy/
        // value_copy are reused across calls purely to avoid a per-record heap allocation.
        read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(candidate.old_offset, mem); },
                               [&](const T2RecordView &record) -> bool {
                                 key_copy.assign(record.key.begin(), record.key.end());
                                 value_copy.assign(record.value.begin(), record.value.end());
                                 return true;
                               });

        const uint64_t new_offset = write_record_to_temp_fd(fd_guard.fd, key_copy, value_copy, next_bytes_used);
        maybe_sync_and_drop_checkpoint_cache(fd_guard.fd, next_bytes_used, next_bytes_synced);

        const uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + key_copy.size() + value_copy.size());
        const uint64_t block_count = aligned_len / kBlockAlignment;
        assert(block_count < 65536 && "Record size exceeds 1.04MB limit");
        relocated[{candidate.prefix, candidate.hash}] = new_offset | (block_count << kSizeEmbeddingShift);
      }
    };

    try {
      // Pre-stop pass: a pure performance optimization, not a correctness requirement -- see
      // stop_writers_and_wait() below for why the amount of work left for the post-stop pass
      // matters. Safe to run any number of times (including zero); T1 is not touched.
      copy_live_entries();

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
      // the store, not just ones being relocated (see acquire_write_handle()'s contract), so
      // everything between here and resume_writers() below should be as fast as possible -- which
      // is exactly why the pre-stop pass above exists, to shrink how much the post-stop pass still
      // has to do.
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
      // runs, below.
      copy_live_entries();

      uint64_t new_capacity = t2_.bytes_capacity();
      if (next_bytes_used > new_capacity) {
        new_capacity = next_bytes_used;
      }

      // Last chance to reclaim the temp file's page cache before T1's own checkpoint write
      // (inside the offset_mapper call at the very end) needs headroom.
      if (next_bytes_used > next_bytes_synced && ::fdatasync(fd_guard.fd) == 0) {
        ::posix_fadvise(fd_guard.fd,
                        static_cast<off_t>(next_bytes_synced),
                        static_cast<off_t>(next_bytes_used - next_bytes_synced),
                        POSIX_FADV_DONTNEED);
        next_bytes_synced = next_bytes_used;
      }

      // TEST-ONLY: lets a test throw here, strictly before T1 is ever touched. No-op in
      // production -- see NoOpPreFinishHook.
      pre_finish_hook();

      ::close(fd_guard.fd);
      fd_guard.fd = -1;

      if (::truncate(temp_path_t2.c_str(), static_cast<off_t>(new_capacity)) != 0) {
        throw std::system_error(errno, std::generic_category(), "truncate temp file");
      }

      const int map_fd = ::open(temp_path_t2.c_str(), O_RDWR);
      if (map_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open temp file for mmap");
      }

      // Fully constructed and ready, just not yet installed as live -- everything from here on
      // (the offset_mapper below, and swap_memory() after it) is I/O-free and cannot throw.
      std::unique_ptr<vmemkv::T2Memory> new_mem =
          mmap_t2_memory(map_fd, new_capacity, "mmap temp file", t2_pair_generation, next_bytes_used);

      // The only place T1 gets published this cycle. offset_mapper is a pure lookup into
      // `relocated` -- everything it needs was already computed and durably written above. Every
      // entry `merged` (T1's own fresh Sorted+Append merge, taken fresh right now) can possibly
      // contain is guaranteed to already be in `relocated`: nothing can have been added since the
      // post-stop pass (writer_stop_ still set), and the two ways an entry could otherwise change
      // without going through acquire_write_handle() -- an in-place update's value bytes, a
      // delete's tombstone -- don't affect payload_bits/generation identity, or are skipped below.
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
          auto it = relocated.find({entry.key, entry.hash});
          assert(it != relocated.end() &&
                 "live T1 entry missing from this cycle's relocation map -- stop_writers_and_wait() "
                 "should make this impossible, see this function's own comment");
          if (it == relocated.end()) {
            continue;  // Defense in depth only -- unreachable if the invariant above holds.
          }
          entry.payload_bits = it->second;
          entry.generation = t2_pair_generation;
        }
      };
      auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) {
        if (do_checkpoint) {
          vmemkv::write_t1_checkpoint(vmemkv::derive_t1_chk_path(t2_path(), t1_generation), merged);
        }
      };
      t1_.reorganize(offset_mapper_fn, chk_writer_fn, t2_pair_generation);

      t2_.swap_memory(std::move(new_mem));
      // Resumes writers as soon as the new generation is live, rather than leaving them blocked
      // through commit_checkpoint()'s fsync/rename/cleanup below -- WriterResumeGuard's destructor
      // still covers the exception path (e.g. if swap_memory() itself throws), and a redundant
      // resume_writers() there is harmless (plain store(false)).
      t2_.resume_writers();

      if (do_checkpoint) {
        commit_checkpoint(
            t1_generation, checkpoint_lsn, next_bytes_used, T2RebuildPublish{temp_path_t2, t2_chk_final_path});
      } else {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path_t2, remove_ec);  // Mapping stays valid after unlink.
      }

      reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
      reorg_t2_count_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      if (fd_guard.fd >= 0) {
        ::close(fd_guard.fd);
        fd_guard.fd = -1;
      }
      std::error_code remove_ec;
      std::filesystem::remove(temp_path_t2, remove_ec);
      throw;
    }

    reset_tombstone_counters();
  }

  // reorganize_internal()'s do_t2_rebuild=false/do_checkpoint=true branch: commits a checkpoint
  // that keeps referencing the existing T2 checkpoint file (current_t2_generation_) instead of
  // paying for a full T2 rewrite. Callers only ever reach this combination when
  // current_t2_generation_ is already set (see checkpoint()'s decide-fn, which falls back to
  // do_t2_rebuild=true otherwise) -- there is always something to reference here. This still
  // costs O(new T2 bytes since the last durable flush), not zero: T2's live mapping is
  // MAP_PRIVATE (see T2FlatFile::map_file()'s comment), so ordinary inserts since the last full
  // rebuild (or T1-only delta flush) exist only in this process's memory, never on disk on their
  // own -- see the delta-flush block below. Still strictly cheaper than a full rebuild, which
  // also re-reads and rewrites every already-durable live byte, not just the new ones.
  // No pre/post-stop scan passes or writer-stop barrier needed here, unlike
  // rebuild_t2_and_maybe_checkpoint(): write_entry_lockfree() always publishes to T1 before it
  // reserves the write's WAL LSN, so every record at or before checkpoint_lsn is already visible
  // to a single t1_.reorganize() freeze taken after checkpoint_lsn is captured -- and since this
  // branch never calls swap_memory(), the T2-generation-retirement hazard the writer-stop barrier
  // exists for doesn't apply here at all.
  void checkpoint_t1_only_with_delta_flush() {
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    const uint64_t checkpoint_lsn = wal_.next_lsn() - 1;
    const uint64_t t1_generation = checkpoint_lsn;
    const uint64_t expected_t2_generation = t2_.get_memory()->generation;

    bool checkpoint_written = false;
    uint64_t committed_t2_bytes_used = 0;
    auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) {
      // Defense-in-depth: this branch's offset_mapper never validates generations (it's a pure
      // passthrough, same as the plain T1-only branch in reorganize_internal()), so a straggler
      // entry stamped with an already-retired T2 generation (see reorganize_internal()'s
      // offset_mapper_fn comment) would otherwise be persisted here completely unvalidated --
      // load_checkpoint_if_present() re-stamps every loaded entry uniformly with zero per-entry
      // checking, so a mismatched offset would silently resolve to the wrong bytes after a
      // restart. Refuse to persist if any live, non-inline entry doesn't match the one T2
      // generation this checkpoint would reference: strictly no worse than the plain T1-only
      // branch (which never persists anything at all), since skipping just leaves the WAL to keep
      // growing until either a future genuine T2 rebuild's offset_mapper_fn relocation or a
      // quieter cycle lets this check pass.
      for (const auto &entry : merged) {
        if (entry.payload_bits == vmemkv::STORE_NOT_FOUND) {
          continue;
        }
        if constexpr (ConfigT::UseT1InlineValue) {
          if (t1_detail::is_inline(entry.hash)) {
            continue;
          }
        }
        if (entry.generation != expected_t2_generation) {
          return;
        }
      }

      // Durably flush whatever ordinary inserts have appended to T2's MAP_PRIVATE mapping since
      // the last durable point (a full rebuild's write_record_to_temp_fd() calls, or an earlier
      // T1-only checkpoint's own flush -- t2_durable_bytes_ tracks whichever was most recent).
      // Existing bytes below that point never move for T1-only checkpoints (no relocation, no
      // compaction), so a plain pwrite() of the new range at its own offset is exactly correct --
      // no need to rewrite anything already on disk.
      const uint64_t current_t2_bytes = t2_.bytes_used();
      if (current_t2_bytes > t2_durable_bytes_) {
        const vmemkv::T2Memory *mem = t2_.get_memory();
        if constexpr (ConfigT::UseScanBaseSequential) {
          // base_boundary promotion, step 1/2 (gate + drain): close off new in-place updates to
          // [t2_durable_bytes_, current_t2_bytes) *before* this function reads those bytes for
          // the flush below, then wait for any updater that already read the *old*
          // write_gate_boundary and is still mid-write to finish. Must happen in this order and
          // before the pwrite below -- see T2Memory::write_gate_boundary's comment for why a
          // single boundary field can't safely serve both this and base_boundary's role, and
          // update_epoch_tracker_'s declaration for why this drain (not
          // T2FlatFile::stop_writers_and_wait(), which only gates new appends) is
          // needed here.
          mem->write_gate_boundary.store(current_t2_bytes, std::memory_order_release);
          const uint64_t new_epoch = update_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
          update_epoch_tracker_.wait_until_epoch(new_epoch);
        }

        const std::filesystem::path t2_chk_path = vmemkv::derive_t2_chk_path(t2_path(), *current_t2_generation_);
        const int fd = ::open(t2_chk_path.c_str(), O_WRONLY);
        if (fd < 0) {
          throw std::system_error(errno, std::generic_category(), "open t2 checkpoint for delta flush");
        }
        const std::byte *delta_base = mem->base + t2_durable_bytes_;
        const uint64_t delta_len = current_t2_bytes - t2_durable_bytes_;
        uint64_t written = 0;
        while (written < delta_len) {
          const ssize_t chunk =
              ::pwrite(fd, delta_base + written, delta_len - written, static_cast<off_t>(t2_durable_bytes_ + written));
          if (chunk <= 0) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "write t2 checkpoint delta");
          }
          written += static_cast<uint64_t>(chunk);
        }
        if (::fsync(fd) != 0) {
          const int err = errno;
          ::close(fd);
          throw std::system_error(err, std::generic_category(), "fsync t2 checkpoint delta");
        }
        ::close(fd);
        t2_durable_bytes_ = current_t2_bytes;

        if constexpr (ConfigT::UseScanBaseSequential) {
          // base_boundary promotion, step 2/2 (publish): only now, with the bytes above durably
          // on disk (and no in-place writer left that could still touch them, per step 1), is it
          // safe to let base_mmap readers treat this range as immutable -- publishing any
          // earlier would let a reader observe bytes that haven't reached disk yet.
          mem->base_boundary.store(current_t2_bytes, std::memory_order_release);
        }
      }
      committed_t2_bytes_used = current_t2_bytes;

      vmemkv::write_t1_checkpoint(vmemkv::derive_t1_chk_path(t2_path(), t1_generation), merged);
      checkpoint_written = true;
    };

    // Third arg is T2Memory's own pairing tag (unrelated to file-naming generations above, see
    // T2Memory::allocate_generation()'s declaration) -- same value the plain T1-only branch in
    // reorganize_internal() passes, since T2 isn't changing here either.
    t1_.reorganize([](std::span<EntrySnapshot> /*merged*/) {}, chk_writer_fn, expected_t2_generation);
    reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
    reset_tombstone_counters();

    if (checkpoint_written) {
      commit_checkpoint(t1_generation, checkpoint_lsn, committed_t2_bytes_used, /*t2_rebuild=*/std::nullopt);
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

      // Registers this thread's epoch *before* the write_gate_boundary read below, so that a
      // concurrent base_boundary promotion (reorganize_internal()'s T1-only-checkpoint branch)
      // can prove, via update_epoch_tracker_.wait_until_epoch(), that this update either observed
      // the promotion's new write_gate_boundary or has fully finished (guard released) before the
      // promotion proceeds to flush/publish. memory_order_acquire here is required, not the
      // relaxed style T1Index::active_epochs_ uses elsewhere -- there the epoch value itself
      // carries no safety information (a separate acquire-load does); here it must, since
      // promotion's happens-before argument depends on this specific load observing promotion's
      // fetch_add.
      using UpdateEpochGuard = typename decltype(update_epoch_tracker_)::Guard;
      std::optional<UpdateEpochGuard> epoch_guard;
      if constexpr (ConfigT::UseScanBaseSequential) {
        epoch_guard.emplace(update_epoch_tracker_, update_epoch_.load(std::memory_order_acquire));
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
      // ScanBaseSequential reads T2's base region through its own seqlock-free mmap (see
      // T2Memory::base_boundary/base_mmap), which is only safe if base offsets never change after
      // being written -- so under this ablation, an in-place update targeting the base is
      // redirected out-of-place (falls through to write_entry_lockfree()) instead. Checks
      // write_gate_boundary, not base_boundary: the two diverge during a T1-only checkpoint's
      // promotion window (see T2Memory::write_gate_boundary's comment), and this decision must
      // use the earlier-published one to stay correct during that window.
      bool allow_in_place = true;
      if constexpr (ConfigT::UseScanBaseSequential) {
        allow_in_place = (res.payload_bits & kOffsetMask) >= mem->write_gate_boundary.load(std::memory_order_acquire);
      }
      if (key_matches && value.size() <= alloc_len && allow_in_place) {
        if (!t2_.update_value_at(res.payload_bits & kOffsetMask, value, mem)) {
          return {InPlaceOutcome::Aborted};
        }
        return {InPlaceOutcome::Applied, wal_.reserve_update(full_key, value)};
      }
      return {InPlaceOutcome::FellThrough};  // Doesn't fit alloc_len, or targets the base region.
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
  // guarantee, never before): current_t2_generation_ (which checkpoint()'s decide-fn reads) is
  // only safe to touch under that guarantee -- see its own declaration -- so evaluating `decide`
  // any earlier would be a data race against a concurrent commit_checkpoint(). `force_run`
  // mirrors the old force_t2_gc=true contract: guarantees at least one reorganize_internal() call
  // actually completes even if T1's append region is already empty (e.g. bulk_load()'s periodic
  // internal reorganizes already drained it), rather than skipping because there was "nothing to
  // do" by the T1-emptiness measure alone.
  template <typename DecideFn>
  void run_reorganize(DecideFn &&decide, bool force_run) {
    bool forced_once = false;
    while (true) {
      // 1. Wait for any concurrent background/manual reorganize to complete
      wait_until_reorg_not_running();

      // 2. force_run's contract is that it always ends with a committed cycle, even if T1's
      // append region was already empty -- so only skip once forced_once too.
      if (t1_.append_size() == 0 && (!force_run || forced_once)) {
        break;
      }

      // 3. Try to acquire the execution lock
      bool expected_running = false;
      if (reorg_running_.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel)) {
        const auto [do_t2_rebuild, do_checkpoint] = decide();
        try {
          reorganize_internal(do_t2_rebuild, do_checkpoint);
        } catch (...) {
          reorg_running_.store(false, std::memory_order_release);
          reorg_running_.notify_all();
          throw;
        }
        reorg_running_.store(false, std::memory_order_release);
        reorg_running_.notify_all();
        forced_once = true;
      }
    }
  }

 public:
  // T1-only in-memory merge. Never touches T2, never persists a checkpoint. Safe to call anytime.
  void reorganize() {
    run_reorganize([] { return std::pair<bool, bool>{false, false}; }, /*force_run=*/false);
  }

  // Always fully rebuilds T2 (reclaims fragmentation) and persists the result as a checkpoint --
  // the rebuild already durably wrote everything via real write() calls, so persisting is what
  // lets the retired generation's files be safely deleted.
  void defragment() {
    run_reorganize([] { return std::pair<bool, bool>{true, true}; }, /*force_run=*/true);
  }

  // Always persists a checkpoint via the cheapest available path: falls back to a full rebuild
  // only if no T2 checkpoint has ever been committed yet (nothing cheap to reference); otherwise
  // takes the T1-merge + T2-delta-flush path (see reorganize_internal()'s T1-only-checkpoint
  // branch). Never rebuilds T2 for fragmentation reasons on its own -- that's defragment()'s job.
  void checkpoint() {
    run_reorganize([this] { return std::pair<bool, bool>{!current_t2_generation_.has_value(), true}; },
                   /*force_run=*/true);
  }

  // Accessors for T1 (Index) and T2 (Flat File) layers (mainly for testing).
  auto t1() noexcept -> T1IndexT & { return t1_; }
  auto t1() const noexcept -> const T1IndexT & { return t1_; }
  auto t2() noexcept -> vmemkv::T2FlatFile & { return t2_; }
  auto t2() const noexcept -> const vmemkv::T2FlatFile & { return t2_; }

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
        uint64_t offset;
        if constexpr (ConfigT::UsePrefaulting) {
          offset = vmemkv::T2FlatFile::append_prefault(mem, full_key, value);
        } else {
          offset = vmemkv::T2FlatFile::append_default(mem, full_key, value);
        }
        uint64_t write_generation = mem->generation;

        uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + full_key.size() + value.size());
        uint64_t block_count = aligned_len / kBlockAlignment;
        assert(block_count < 65536 && "Record size exceeds 1.04MB limit");
        uint64_t encoded_payload = offset | (block_count << kSizeEmbeddingShift);
        put_result = t1_.put(full_key, encoded_payload, false, 0, write_generation);
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
  // T2 base-region reads (the ScanBaseSequential ablation). Once a record's offset is below
  // `base_boundary`, its bytes are immutable forever (update_impl() redirects in-place updates
  // targeting that range out-of-place instead -- see write_gate_boundary's comment), so a reader
  // doesn't need the seqlock protecting the mutable tail and can read straight out of a mapping.
  // Get and Scan want *different* readahead policies for that read though (madvise is a property
  // of the whole mapping, not of one read), so several distinct mappings of the identical bytes
  // exist -- which one a given call should use is decided per record (never once per generation:
  // a real corpus isn't guaranteed uniform record sizes even though this project's benchmarks
  // happen to use one size per run) by try_read_base_record()'s switch below. That switch is the
  // single place this decision is made, and callers never choose a mapping themselves.
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
  // mmap for free (no syscall, no copy), exactly matching what a plain mmap-based Get would have
  // cost before this ablation existed. Returns std::nullopt on any doubt (mincore()
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
  // This is deliberately ONE function with the reader/size decision laid out explicitly in one
  // place, not two similar functions that happen to share a helper: an earlier version had Get's
  // small-record path silently reuse Scan's own function (and thus Scan's MADV_SEQUENTIAL
  // mapping), which under real LTM memory pressure cost ~10x more kernel time per major page
  // fault than Get's ideal policy -- a genuinely random access pattern paying for readahead +
  // drop-behind it never benefits from (see the kGet case below for the fix and measurements).
  // Consolidating the decision here means changing a reader's policy, or adding a new reader,
  // only ever touches one switch, not two functions that need to be kept in sync by hand.
  //
  // Callers must guard their call with `if constexpr (ConfigT::UseScanBaseSequential)` (see the
  // assert below) -- get_impl()/scan_impl() do this rather than this function checking the
  // ablation flag itself, so that calling it when the ablation is off is a caught implementation
  // mistake, not a silently-absorbed no-op.
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
    assert(ConfigT::UseScanBaseSequential &&
           "try_read_base_record() requires callers to guard with "
           "`if constexpr (ConfigT::UseScanBaseSequential)`");

    const uint64_t offset = payload_bits & kOffsetMask;
    // Single read: base_boundary can grow concurrently (a T1-only checkpoint's incremental
    // promotion, see T2Memory's comment), but never shrinks, so a torn/inconsistent read here
    // could only be *more* conservative than reality, never less -- reading once is just
    // cheaper, not required for safety.
    const uint64_t base_boundary = mem->base_boundary.load(std::memory_order_acquire);
    if (offset >= base_boundary) {
      return std::nullopt;
    }

    // The embedded block-count size hint is 16-byte-granular while records are only 8-byte
    // aligned (align_up()), so it can undershoot the true aligned length by up to 8 bytes -- the
    // same margin the retired IoUringScanRealRead ablation needed for this identical "read
    // exactly this many bytes" problem. A read sized to (hint + margin) covers every record this
    // store can produce, whether resident (mmap path) or not (pread path).
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
          // to a 64KB record's copy but dominating a 1KB one's ~2us *total* pre-existing cost --
          // 1KB In-Memory Get/Hit regressed 90-98% before this guard was added. Measured
          // directly: routing this through base_mmap_scan (no madvise, some readahead -- an
          // earlier version of this did) still cost ~4.4x more kernel time per major fault than
          // `base` does, and through base_mmap_scan_seq (Scan's mapping, MADV_SEQUENTIAL -- the
          // original bug this consolidation exists to prevent a repeat of) ~10x more,
          // LTM/1KB Get/Hit/Zipf/threads:32.
          return read_base_record_via(mem->base, offset, base_boundary);
        }
        {
          // Large records: check residency first (mincore(), which never blocks on a fault
          // itself) before committing to a read -- if resident, base_mmap_scan gives a free
          // mmap read costing nothing beyond what a plain mmap-based Get would have paid before
          // this ablation existed; if not, one bounded pread() beats the N separate page faults
          // an mmap read of a multi-page cold record would trigger.
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
  //   rebuild_t2_and_maybe_checkpoint() always calls t1_.reorganize() (re-stamping every entry to
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
      if constexpr (ConfigT::UseScanBaseSequential) {
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

      uint64_t payload = t1_.get(full_key);
      if (payload == vmemkv::STORE_NOT_FOUND) {
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
                 if constexpr (ConfigT::UseScanBaseSequential) {
                   if (const auto base_record = try_read_base_record(mem, payload, BaseReader::kScan, nullptr);
                       base_record.has_value()) {
                     if (key_in_range(base_record->key, lower_bound, upper_bound)) {
                       callback(base_record->key, base_record->value);
                     }
                     ++pass_count;
                     return;
                   }
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

  // Whether T2 is fragmented enough to warrant a full rebuild (GC). Used by reorg_worker_loop()
  // and recover_from_wal() to decide whether to call defragment()-equivalent behavior --
  // reorganize_internal() does not make this decision itself; callers do (low_level_design.md
  // 4.4).
  auto space_amp_over_threshold() const -> bool {
    const uint64_t live_bytes = t1_.live_bytes();
    const uint64_t t2_used = t2_.bytes_used();
    const double space_amp = live_bytes > 0 ? static_cast<double>(t2_used) / static_cast<double>(live_bytes) : 1.0;
    const double amp_threshold = static_cast<double>(ConfigT::T2StorageFragmentationThresholdPercent) / 100.0 + 1.0;
    return space_amp >= amp_threshold;
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

    // Best-effort second and third mappings for ScanBaseSequential -- see try_read_base_record()
    // above for who reads which: `base_mmap_scan_seq` (MADV_SEQUENTIAL) is Scan's small-record
    // mapping only; `base_mmap_scan` (kernel default) is shared by Scan's large-record path and
    // Get's large-record warm path (Get's small-record path reads the *main* mapping directly,
    // since that's already MADV_RANDOM -- exactly what Get wants regardless of size). Both exist
    // because madvise is a per-VMA property, not a per-read one, so Scan's own two size classes
    // need genuinely different mappings; Get's large-record cold case reads the same bytes via
    // pread instead (below). Both mapped to the *full* `capacity`, not just `bytes_used` -- unlike the main
    // mapping, unwritten/never-promoted pages here are simply never touched (every read is
    // gated by offset < base_boundary), so over-mapping costs nothing and lets a T1-only
    // checkpoint's incremental base_boundary promotion (reorganize_internal()) grow their
    // *effective* coverage later without ever remapping (see T2Memory::base_mmap_scan's comment
    // for why a read-only, never-written-through mapping like these transparently reflects a
    // later pwrite() to the file with no remap needed). Created unconditionally whenever this
    // ablation is on (not gated on bytes_used > 0 as an earlier version did): a store whose very
    // first checkpoint is a forced full rebuild before any insert ever lands (bytes_used == 0 at
    // that point) must still get real mappings here, or promotion would have nothing to extend
    // later and the whole mechanism would silently never activate for that store. A failure to
    // establish either one is silently non-fatal: the primary mapping above already provides
    // full correctness via scan_impl()'s existing seqlock fallback, these are purely a speed
    // optimization layered on top.
    std::byte *base_mmap_scan_ptr = nullptr;
    std::byte *base_mmap_scan_seq_ptr = nullptr;
    int read_fd_dup = -1;
    if constexpr (ConfigT::UseScanBaseSequential) {
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

      // Under Prefaulting, eagerly install page table entries for just the currently-valid
      // prefix [0, bytes_used) in one bulk call per mapping, not via MAP_POPULATE on the mmap
      // calls themselves (which would eagerly fault in the entire capacity). This matters even
      // for already page-cache-resident data: mmap() doesn't share page *table* entries across
      // separate VMAs of the same file, so each fresh mapping still needs its own per-page minor
      // fault to install a PTE the first time it's touched, cache-resident or not. get_impl()
      // reads single-page records straight through base_mmap_scan_seq, and a Uniform-distributed
      // workload touching most of a large corpus pays for *every one* of those first-touch minor
      // faults during the timed benchmark itself if a mapping it reads isn't pre-warmed --
      // measured to regress 1KB In-Memory Get/Hit/Uniform by ~250x without this. Both mappings
      // are warmed (not just whichever one this generation's corpus happens to use), since which
      // one is actually hit is now decided per record, not once here.
      // Best-effort: a failure here just means the first touch of each page pays an ordinary (if
      // still page-cache-resident-cheap) minor fault instead of finding it pre-installed.
      if (ConfigT::UsePrefaulting && bytes_used > 0) {
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
      // closed below (that close() is unconditional and happens regardless of this ablation).
      // Best-effort like the mappings above: a dup() failure just means get_impl() falls back to
      // the always-correct `base` + seqlock path.
      read_fd_dup = ::fcntl(file_descriptor, F_DUPFD_CLOEXEC, 0);
    }

    ::close(file_descriptor);
    auto mem = std::make_unique<vmemkv::T2Memory>(static_cast<std::byte *>(mapped), capacity, generation, bytes_used);
    mem->base_mmap_scan = base_mmap_scan_ptr;
    mem->base_mmap_scan_seq = base_mmap_scan_seq_ptr;
    mem->read_fd = read_fd_dup;
    return mem;
  }

  // A just-rebuilt T2 temp file, ready to be renamed onto its final checkpoint path. Present only
  // when this cycle actually rebuilt T2; absent for a T1-only checkpoint, whose commit references
  // the existing, unchanged T2 checkpoint file instead (see T2RebuildPublish's absence handling in
  // commit_checkpoint() below).
  struct T2RebuildPublish {
    std::filesystem::path temp_path_t2;
    std::filesystem::path t2_chk_final_path;
  };

  // Publishes this cycle's checkpoint: if `t2_rebuild` is present, renames its temp file onto its
  // final path (t2_generation == t1_generation, a fresh T2 checkpoint); otherwise this is a
  // T1-only checkpoint and t2_generation stays whatever it already was (current_t2_generation_ --
  // callers only take this path when it's already set, see checkpoint()'s decide-fn, which falls
  // back to a full rebuild otherwise). Either way, commits the manifest, rotates the WAL down to
  // checkpoint_lsn's tail, and removes only the files an on-disk generation number actually
  // stopped referencing: the T1 chk file always (a checkpoint always writes a fresh one), but the
  // T2 chk file only when this cycle actually rebuilt T2 -- otherwise it's still the file a T1-only
  // checkpoint just committed the manifest against, and deleting it would corrupt that checkpoint.
  // Called only after t2_'s live mapping has already been swapped, when t2_rebuild is present.
  void commit_checkpoint(uint64_t t1_generation,
                         uint64_t checkpoint_lsn,
                         uint64_t t2_bytes_used,
                         std::optional<T2RebuildPublish> t2_rebuild) {
    if (t2_rebuild.has_value()) {
      std::error_code error_code;
      std::filesystem::rename(t2_rebuild->temp_path_t2, t2_rebuild->t2_chk_final_path, error_code);
      if (error_code) {
        throw std::system_error(error_code, "rename t2 checkpoint file");
      }
    } else {
      assert(current_t2_generation_.has_value() &&
             "T1-only checkpoint requires an existing T2 checkpoint generation to reference -- "
             "checkpoint()'s decide-fn should have forced a full rebuild otherwise");
    }
    const uint64_t t2_generation = t2_rebuild.has_value() ? t1_generation : *current_t2_generation_;

    vmemkv::write_manifest(vmemkv::derive_manifest_path(t2_path()), t1_generation, t2_generation, t2_bytes_used);
    wal_.rotate(checkpoint_lsn);

    if (current_t1_generation_.has_value() && *current_t1_generation_ != t1_generation) {
      std::error_code ignored;
      std::filesystem::remove(vmemkv::derive_t1_chk_path(t2_path(), *current_t1_generation_), ignored);
    }
    if (t2_rebuild.has_value() && current_t2_generation_.has_value() && *current_t2_generation_ != t2_generation) {
      std::error_code ignored;
      std::filesystem::remove(vmemkv::derive_t2_chk_path(t2_path(), *current_t2_generation_), ignored);
    }
    current_t1_generation_ = t1_generation;
    current_t2_generation_ = t2_generation;
    // A full rebuild's bytes were all written via real write() calls (write_record_to_temp_fd()),
    // so they're already durable -- t2_durable_bytes_ tracks that baseline for future T1-only
    // checkpoints' delta flushes. A T1-only checkpoint's caller already advanced it itself before
    // calling here (see reorganize_internal()'s T1-only-with-checkpoint branch), so leave it alone
    // in that case.
    if (t2_rebuild.has_value()) {
      t2_durable_bytes_ = t2_bytes_used;
    }
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

    vmemkv::T1CheckpointFile t1_chk(vmemkv::derive_t1_chk_path(t2_path, manifest->t1_generation));

    const std::filesystem::path t2_chk_path = vmemkv::derive_t2_chk_path(t2_path, manifest->t2_generation);
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
    current_t1_generation_ = manifest->t1_generation;
    current_t2_generation_ = manifest->t2_generation;
    // Whatever was captured in the manifest is exactly what's physically on disk at this
    // generation's checkpoint file (see the T1-only-with-checkpoint branch's delta-flush comment).
    t2_durable_bytes_ = manifest->t2_bytes_used;
  }

  // Replays the current contents of wal_ into T1 (and, via write_entry_lockfree, T2) -- whether
  // that's the full history or just the post-checkpoint tail is transparent here. Runs before
  // reorg_worker_ is started (constructor order: recovering_=true; ...; recover_from_wal();
  // recovering_=false; *then* reorg_worker_ is move-assigned a real thread), so no other thread
  // can be touching reorg_running_/t1_/t2_ yet -- calls reorganize_internal() directly rather than
  // through the public reorg_running_ CAS/wait wrappers, which would be redundant synchronization
  // against a competitor that cannot exist at this point. do_checkpoint is always false here
  // (checkpointing mid-replay would deadlock, see reorganize_internal()'s own assert); do_t2_rebuild
  // is space_amp_over_threshold() alone (this replay's only possible driver of T2 fragmentation is
  // replaying many delete/update records) -- this is the "livelock-avoidance rebuild" mentioned in
  // reorganize_internal()'s do_t2_rebuild branch: reclaims T1 append-region capacity without
  // persisting anything. Without the explicit capacity check below, a WAL with more live distinct
  // keys than one append region holds would livelock inside write_entry_lockfree, which can only
  // escape AppendRegionFull by waiting on a worker that doesn't exist yet.
  void recover_from_wal() {
    wal_.replay([&](vmemkv::WalRecordType type,
                    std::span<const std::byte> key,
                    std::span<const std::byte> value,
                    uint64_t /*lsn*/) {
      if (t1_.append_size() + 1 >= T1IndexT::APPEND_CAP) {
        reorganize_internal(space_amp_over_threshold(), /*do_checkpoint=*/false);
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

  auto write_record_to_temp_fd(int file_descriptor,
                               std::span<const std::byte> key,
                               std::span<const std::byte> value,
                               uint64_t &bytes_used) -> uint64_t {
    ValueRecordHeader header;
    header.key_len = static_cast<uint32_t>(key.size());
    header.value_len = static_cast<uint32_t>(value.size());
    header.alloc_len = static_cast<uint32_t>(value.size());
    header.version = 0;

    uint64_t offset = bytes_used;
    const uint64_t raw_len = sizeof(header) + key.size() + value.size();
    const uint64_t aligned_len = align_up(raw_len);
    const uint64_t padding = aligned_len - raw_len;

    if (::write(file_descriptor, &header, sizeof(header)) != sizeof(header)) {
      throw std::system_error(errno, std::generic_category(), "write header");
    }
    if (::write(file_descriptor, key.data(), key.size()) != static_cast<ssize_t>(key.size())) {
      throw std::system_error(errno, std::generic_category(), "write key");
    }
    if (!value.empty()) {
      if (::write(file_descriptor, value.data(), value.size()) != static_cast<ssize_t>(value.size())) {
        throw std::system_error(errno, std::generic_category(), "write value");
      }
    }
    if (padding > 0) {
      std::array<std::byte, kDefaultAlignmentBytes> pad{};
      if (::write(file_descriptor, pad.data(), padding) != static_cast<ssize_t>(padding)) {
        throw std::system_error(errno, std::generic_category(), "write padding");
      }
    }

    bytes_used += aligned_len;
    return offset;
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
        // Explicit priority (space_amp wins over WAL size): only one of these actually needs T2
        // I/O (defragment()-equivalent); the other two are cheap. Note reorg_requested_ (this
        // wakeup's trigger) only ever fires on append-region/delete pressure (see
        // maybe_reorganize_if_needed()) -- space_amp/WAL-size changes never themselves cause a
        // wakeup, so this only checks them "while already awake anyway."
        if (space_amp_over_threshold()) {
          reorganize_internal(/*do_t2_rebuild=*/true, /*do_checkpoint=*/true);
        } else if (wal_over_threshold()) {
          reorganize_internal(/*do_t2_rebuild=*/!current_t2_generation_.has_value(), /*do_checkpoint=*/true);
        } else {
          reorganize_internal(/*do_t2_rebuild=*/false, /*do_checkpoint=*/false);
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

  // Drains in-place T2 updates racing a T1-only checkpoint's base_boundary promotion (see
  // reorganize_internal()'s T1-only-checkpoint branch and update_impl()'s in-place path).
  // Scoped to in-place updates specifically, not reused from T2FlatFile::active_readers_ (which
  // every Get/Scan/Update registers with): waiting on that broader tracker would block promotion
  // on unrelated, possibly long-running Scans for no reason. update_epoch_ is bumped once per
  // promotion; update_impl() tags its epoch guard with whatever value it observes *before*
  // reading write_gate_boundary (memory_order_acquire is required there, not the relaxed style
  // T1Index::active_epochs_ uses -- see update_impl()'s comment for why).
  mutable vmemkv::ThreadReferenceTracker<uint64_t> update_epoch_tracker_;
  std::atomic<uint64_t> update_epoch_{1};

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

  // Both only ever touched under reorg_running_'s single-flight guarantee (or, for
  // recovering_, only during single-threaded construction before reorg_worker_ exists) --
  // neither needs atomics.
  // Names the currently-referenced T1/T2 checkpoint files (derive_t1_chk_path/derive_t2_chk_path).
  // Set once a checkpoint has been committed or loaded. current_t1_generation_ advances on every
  // committed checkpoint; current_t2_generation_ advances only when a checkpoint actually rebuilt
  // T2 (a T1-only checkpoint leaves it unchanged, reusing the existing T2 checkpoint file).
  std::optional<uint64_t> current_t1_generation_;
  std::optional<uint64_t> current_t2_generation_;
  // How many bytes of current_t2_generation_'s checkpoint file are known to physically exist on
  // disk. T2's live mmap is MAP_PRIVATE (see T2FlatFile::map_file()'s comment): ordinary inserts
  // write only into this process's private pages, never back to the file, so bytes appended since
  // the last full T2 rebuild exist nowhere durable until a T1-only checkpoint explicitly flushes
  // them (see reorganize_internal()'s T1-only-with-checkpoint branch). Meaningless/unused before
  // current_t2_generation_ is first set.
  uint64_t t2_durable_bytes_ = 0;
  bool recovering_ = false;  // True only during the constructor's initial WAL replay.
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

}  // namespace vmemkv
