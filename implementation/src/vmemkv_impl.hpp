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

#include <array>
#include <atomic>
#include <cassert>
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
#include <vector>
#include <vmemkv/config.hpp>

#include "checkpoint/checkpoint.hpp"
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

// Test-only seam: fires once inside reorganize_internal(), right before
// begin_draining_and_wait_for_writers() is called (i.e. while the about-to-be-retired generation
// is still handed out normally by acquire_write_handle()). Lets a test deterministically get a
// writer's T2MemoryHandle registered *before* the drain flag goes up, so the subsequent drain-wait
// has a real, still-in-flight writer to wait for -- exercising the exact handshake that closes the
// residual-window race (see begin_draining_and_wait_for_writers()'s declaration). No-op in
// production.
struct NoOpPreDrainHook {
  void operator()() const noexcept {}
};

// Test-only seam: fires once inside reorganize_internal(), right after the drain above has
// completed (t1_.append_size() has just been proven 0 with no writer able to land another entry)
// and before the close()/truncate()/open()/mmap()/swap_memory() sequence. No-op in production.
struct NoOpPostConvergenceHook {
  void operator()() const noexcept {}
};

// Test-only seam: called from reorganize_internal()'s offset_mapper right before the KNOWN OPEN
// ISSUE assert (see that call site). Returning true skips the now-unsafe T2 dereference instead
// of proceeding, letting a test observe the issue firing without needing a non-NDEBUG build or
// risking an actual out-of-bounds read. Defaults to a no-op returning false; production behavior
// is unchanged either way.
struct NoOpGenerationMismatchHook {
  auto operator()(uint64_t /*old_generation*/, uint64_t /*live_generation*/) const noexcept -> bool { return false; }
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
    if constexpr (!ConfigT::UseMadviseRandom) {
      parts.emplace_back("NoMadvise");
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
    if constexpr (ConfigT::UseMadviseRandom) {
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
  template <typename PostConvergenceHook = NoOpPostConvergenceHook,
            typename GenerationMismatchHook = NoOpGenerationMismatchHook,
            typename PreDrainHook = NoOpPreDrainHook>
  void reorganize_internal(bool force_t2_gc,
                           PostConvergenceHook post_convergence_hook = PostConvergenceHook{},
                           GenerationMismatchHook generation_mismatch_hook = GenerationMismatchHook{},
                           PreDrainHook pre_drain_hook = PreDrainHook{}) {
    using EntrySnapshot = typename T1IndexT::EntrySnapshot;
    const bool upgrade_to_t2 = force_t2_gc || should_upgrade_to_t2();

    if (upgrade_to_t2) {
      // recovering_: livelock-avoidance rebuild only (see recover_from_wal()) -- reclaims T1
      // append-region capacity so replay can continue, but must not persist a checkpoint or
      // touch the WAL (would deadlock; see should_upgrade_to_t2()'s comment).
      const bool persist_checkpoint = !recovering_;
      const uint64_t checkpoint_lsn = persist_checkpoint ? wal_.next_lsn() - 1 : 0;
      const uint64_t generation = checkpoint_lsn;

      const std::filesystem::path t2_chk_final_path = persist_checkpoint
                                                          ? vmemkv::derive_t2_chk_path(t2_path(), generation)
                                                          : t2_.path().parent_path() / "t2_flat.recovery_tmp";
      const std::filesystem::path temp_path_t2 =
          persist_checkpoint ? std::filesystem::path(t2_chk_final_path.string() + ".tmp") : t2_chk_final_path;

      std::error_code error_code;
      std::filesystem::remove(temp_path_t2, error_code);

      const int temp_fd = ::open(temp_path_t2.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (temp_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open temp file");
      }

      uint64_t next_bytes_used = 0;
      // Tracks how far maybe_sync_and_drop_checkpoint_cache() has flushed+dropped, so the
      // offset_mapper callback below can periodically bound the temp T2 file's page cache
      // footprint instead of letting it grow unbounded until the commit-time fsync -- that growth
      // would otherwise compete with the live T2 mmap's resident pages for the same RAM.
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

      // Reused across offset_mapper calls to avoid a per-record heap allocation; safe since
      // offset_mapper runs strictly sequentially.
      std::vector<std::byte> key_copy;
      std::vector<std::byte> value_copy;

      try {
        // Convergence loop: a writer can publish a fresh entry into T1's active append region
        // while this reorg's merge runs, stamped with whatever T2 generation it wrote against
        // (write_entry_lockfree() doesn't check for a racing rebuild). Such an entry isn't part
        // of `merged` and survives into whatever's active when t1_.reorganize() returns -- fine
        // on its own (the next cycle's freeze picks it up), unless this cycle retires that
        // entry's T2 generation via swap_memory() first, leaving it pointing at an unmapped file
        // with no way to recover its data. Draining append_size() to empty before swap_memory()
        // closes that: every entry gets frozen and mapped against the T2 generation still live
        // throughout the loop. Re-processing an entry an earlier pass already remapped is a cheap
        // pass-through (see offset_mapper's `old_generation == t2_pair_generation` check).
        //
        // Deliberately unconditional (no pass cap): a small fixed cap is insufficient under
        // sustained write pressure. What actually bounds this loop is
        // maybe_reorganize_if_needed()'s hard-threshold backpressure: once append_size() crosses
        // the hard limit, every other writer blocks until this call returns, so the pass that
        // freezes the triggering write sees nothing more arrive and converges next check.
        // Named (not inline) so the final gate below can reuse the same callbacks.
        auto offset_mapper_fn =
            [&](uint64_t old_payload, uint64_t old_hash, uint64_t old_generation) -> std::pair<uint64_t, uint64_t> {
          if (old_payload == vmemkv::STORE_NOT_FOUND) {
            return {vmemkv::STORE_NOT_FOUND, old_generation};
          }

          if constexpr (ConfigT::UseT1InlineValue) {
            if (t1_detail::is_inline(old_hash)) {
              return {old_payload, old_generation};
            }
          }

          // Already remapped by an earlier convergence pass this cycle -- its offset is already
          // relative to the temp file being built, not to `mem` below, so pass it through as-is.
          if (old_generation == t2_pair_generation) {
            return {old_payload, old_generation};
          }

          T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
          const uint64_t live_generation = static_cast<const vmemkv::T2Memory *>(mem)->generation;
          // KNOWN OPEN ISSUE, real and currently unresolved: can still fire under sustained
          // concurrent load despite the convergence loop and pre-swap gate above. Deterministic
          // (currently *failing*, on purpose) repro: test_kv_store.cpp,
          // "VMemKV: KNOWN OPEN ISSUE - a straggler entry from reorganize_internal()'s residual
          // window is examined against the wrong T2 generation one cycle later".
          if (old_generation != live_generation) {
            assert(false &&
                   "T1 entry's stamped T2 generation does not match the T2 generation currently "
                   "being rebuilt from -- see reorganize_internal()'s convergence-loop comment "
                   "and this call site's own \"KNOWN OPEN ISSUE\" note");
            if (generation_mismatch_hook(old_generation, live_generation)) {
              // TEST-ONLY escape hatch, not a fix -- entry passes through with its stale
              // generation intact. Production falls through below unchanged either way.
              return {old_payload, old_generation};
            }
          }

          uint64_t pure_offset = old_payload & kOffsetMask;

          // Copy key+value out under the seqlock: this record can be concurrently mutated by
          // update_impl()'s in-place T2 path (key_lock does not serialize against reorganize(),
          // by design). t2_.at() is called inside read_t2_record_seqlock() (as AtFunc) rather
          // than once beforehand, since key_len/value_len are unsynchronized and a stale size
          // read once wouldn't be caught by the version recheck -- see that function's comment.
          // A garbage size here would propagate: write_record_to_temp_fd() writes exactly that
          // many bytes, and capacity computation below keeps the inflated size forever (T2
          // capacity only grows). key_copy/value_copy are reused across calls purely to avoid a
          // per-record heap allocation; fully overwritten by assign() before every use.
          read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(pure_offset, mem); },
                                 [&](const T2RecordView &record) -> bool {
                                   key_copy.assign(record.key.begin(), record.key.end());
                                   value_copy.assign(record.value.begin(), record.value.end());
                                   return true;
                                 });

          uint64_t new_offset = write_record_to_temp_fd(fd_guard.fd, key_copy, value_copy, next_bytes_used);
          maybe_sync_and_drop_checkpoint_cache(fd_guard.fd, next_bytes_used, next_bytes_synced);

          uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + key_copy.size() + value_copy.size());
          uint64_t block_count = aligned_len / kBlockAlignment;
          assert(block_count < 65536 && "Record size exceeds 1.04MB limit");
          return std::pair<uint64_t, uint64_t>{new_offset | (block_count << kSizeEmbeddingShift), t2_pair_generation};
        };
        auto chk_writer_fn = [&](std::span<const EntrySnapshot> merged) {
          // Runs after every offset_mapper call has finished writing T2's records, so this is
          // the last chance to reclaim T2's checkpoint page cache before T1's own checkpoint
          // write below needs headroom.
          if (next_bytes_used > next_bytes_synced && ::fdatasync(fd_guard.fd) == 0) {
            ::posix_fadvise(fd_guard.fd,
                            static_cast<off_t>(next_bytes_synced),
                            static_cast<off_t>(next_bytes_used - next_bytes_synced),
                            POSIX_FADV_DONTNEED);
            next_bytes_synced = next_bytes_used;
          }
          if (persist_checkpoint) {
            vmemkv::write_t1_checkpoint(vmemkv::derive_t1_chk_path(t2_path(), generation), merged);
          }
        };

        while (true) {
          t1_.reorganize(offset_mapper_fn, chk_writer_fn, t2_pair_generation);

          // Deliberately re-checked again right before swap_memory() -- see below.
          if (t1_.append_size() == 0) {
            break;
          }
        }

        // Closes the residual window between here and swap_memory() (formerly the KNOWN OPEN
        // ISSUE this function used to carry): truncate()/open()/mmap() below are genuine
        // syscalls, during which a writer could previously still publish a T1 entry naming the
        // about-to-be-retired generation, surviving to be examined against the wrong T2 memory
        // one cycle later. begin_draining_and_wait_for_writers() marks that generation draining
        // (write_entry_lockfree()'s acquire_write_handle() then defers instead of writing into
        // it) and blocks until every writer that already held a handle -- i.e. started before the
        // flag went up -- has released it, which per that handle's contract only happens after
        // its T1 publish attempt has returned. A bounded, one-time batch of such stragglers can
        // still land during the wait; the single pass below sweeps them up. From here on,
        // t1_.append_size() cannot change again until end_draining() runs (paired via
        // DrainGuard, including on the exception path below), so this is the first point in the
        // function where "no more writes are coming" is actually proven, not just observed.
        const vmemkv::T2Memory *pre_swap_mem = t2_.get_memory();
        // TEST-ONLY: lets a test register a writer's handle to pre_swap_mem before the drain
        // flag goes up. No-op in production -- see NoOpPreDrainHook.
        pre_drain_hook();
        t2_.begin_draining_and_wait_for_writers(pre_swap_mem);
        struct DrainGuard {
          vmemkv::T2FlatFile *t2;
          ~DrainGuard() { t2->end_draining(); }
        } drain_guard{&t2_};

        if (t1_.append_size() != 0) {
          t1_.reorganize(offset_mapper_fn, chk_writer_fn, t2_pair_generation);
        }
        assert(t1_.append_size() == 0 &&
               "begin_draining_and_wait_for_writers() guarantees no writer can still be "
               "publishing against pre_swap_mem at this point");

        uint64_t new_capacity = t2_.bytes_capacity();
        if (next_bytes_used > new_capacity) {
          new_capacity = next_bytes_used;
        }

        // TEST-ONLY: used to simulate a writer landing in the (now-closed) residual window above.
        // No-op in production -- see NoOpPostConvergenceHook.
        post_convergence_hook();

        ::close(fd_guard.fd);
        fd_guard.fd = -1;

        if (::truncate(temp_path_t2.c_str(), static_cast<off_t>(new_capacity)) != 0) {
          throw std::system_error(errno, std::generic_category(), "truncate temp file");
        }

        const int map_fd = ::open(temp_path_t2.c_str(), O_RDWR);
        if (map_fd < 0) {
          throw std::system_error(errno, std::generic_category(), "open temp file for mmap");
        }

        // mmap_t2_memory() is itself a syscall, but unlike before the drain above, append_size()
        // going non-zero here is no longer possible: no writer can hold a handle to pre_swap_mem
        // (drained), and none can be issued a new one until end_draining() runs after swap_memory()
        // below.
        std::unique_ptr<vmemkv::T2Memory> new_mem =
            mmap_t2_memory(map_fd, new_capacity, "mmap temp file", t2_pair_generation, next_bytes_used);
        t2_.swap_memory(std::move(new_mem));
        // Ends the drain as soon as the new generation is live, rather than leaving writers
        // blocked through commit_checkpoint()'s fsync/rename/cleanup below -- DrainGuard's
        // destructor still covers the exception path (e.g. if swap_memory() itself throws), and
        // a redundant end_draining() there is harmless (plain store(false)).
        t2_.end_draining();

        if (persist_checkpoint) {
          commit_checkpoint(generation, checkpoint_lsn, temp_path_t2, t2_chk_final_path, next_bytes_used);
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
    } else {
      // T1-only reorganize (zero I/O): T2 isn't touched, so offset_mapper passes payload and
      // generation through unchanged. Overwriting every generation to "current T2" here would
      // silently erase a stale generation left by a past race, with no way to detect it later.
      t1_.reorganize(
          [&](uint64_t old_payload, uint64_t /*old_hash*/, uint64_t old_generation) -> std::pair<uint64_t, uint64_t> {
            return {old_payload, old_generation};
          },
          typename T1IndexT::NoOpChkWriter{},
          t2_.get_memory()->generation);
      reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
      reset_tombstone_counters();
    }
    scan_active_.store(false, std::memory_order_relaxed);
  }

  // Public, blocking-guaranteed synchronize method
  void reorganize(bool force_t2_gc = true) {
    // Tracks whether this call already drove one reorganize_internal(force_t2_gc) to completion,
    // so a forced checkpoint request can't loop forever re-triggering itself once satisfied.
    bool forced_once = false;
    while (true) {
      // 1. Wait for any concurrent background/manual reorganize to complete
      while (reorg_running_.load(std::memory_order_acquire)) {
        reorg_running_.wait(true, std::memory_order_acquire);
      }

      // 2. force_t2_gc=true's contract is that it always ends with a committed, manifest-
      // referenced generation, even if T1's append region was already empty (e.g. bulk_load()'s
      // periodic internal reorganizes drained it already) -- so only skip once forced_once too.
      if (t1_.append_size() == 0 && (!force_t2_gc || forced_once)) {
        break;
      }

      // 3. Try to acquire the execution lock
      bool expected_running = false;
      if (reorg_running_.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel)) {
        try {
          reorganize_internal(force_t2_gc);
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

      // acquire_write_handle() (not a plain get_memory_handle()) defers while a reorganize() is
      // draining its final pre-swap window, and -- critically -- `mem` is held alive across both
      // the T2 append below and the T1 publish attempt, not released in between. That pairing is
      // what closes reorganize_internal()'s residual-window race: as long as this handle is live,
      // begin_draining_and_wait_for_writers() can't conclude that `mem`'s generation is safe to
      // retire, so a straggler entry naming it can never survive past the reorg that's currently
      // running. write_generation is simply `mem->generation` -- the exact generation this write
      // landed in, since we never let go of `mem` between writing and publishing.
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

  // Retrieves a value and invokes callback with its raw bytes.
  // - Thread-safety: lock-free, concurrently readable during reorganization.
  // - Concurrency note (canonical explanation; other methods below point here): looks up T1,
  //   then checks the resolved entry's stamped generation (SortedSlot::generation) against a
  //   freshly-acquired T2 mem, retrying on mismatch. A T1 offset resolved against the wrong T2
  //   generation is not just wrong data -- the rebuilt T2 file can be a different size, so the
  //   offset can be out of bounds, and read_t2_record_seqlock() can spin forever on bytes that
  //   never settle into a valid record (a real, reproducible hang).
  template <typename Callback>
  auto get_impl(std::span<const std::byte> full_key, Callback callback) const -> bool {
    while (true) {
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

      T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
      if (res.generation != mem->generation) {
        continue;
      }

      bool success = false;
      read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(res.payload_bits & kOffsetMask, mem); },
                             [&](const T2RecordView &record) -> bool {
                               if (!byte_span_equal(record.key, full_key)) {
                                 return false;
                               }
                               callback(record.value);
                               success = true;
                               return true;
                             });

      return success;
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
        // Same generation-pairing check as get_impl() -- see its comment. mem is threaded through
        // to update_value_at() below so the read-decide step and the write resolve `payload`
        // against the same T2 generation.
        bool in_place = false;
        while (true) {
          const auto res = t1_.get_with_hash(full_key);
          if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
            return false;
          }
          if (t1_detail::is_inline(res.raw_hash)) {
            break;  // Not an in-place update (inline entry) -- fall through to write_entry_lockfree().
          }

          T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
          if (res.generation != mem->generation) {
            continue;
          }

          // Read key/alloc_len fresh under the seqlock -- see read_t2_record_seqlock()'s comment
          // for why an unprotected t2_.at() call isn't safe here.
          bool key_matches = false;
          uint32_t alloc_len = 0;
          read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(res.payload_bits & kOffsetMask, mem); },
                                 [&](const T2RecordView &record) -> bool {
                                   key_matches = byte_span_equal(record.key, full_key);
                                   alloc_len = record.header->alloc_len;
                                   return true;
                                 });
          if (key_matches && value.size() <= alloc_len) {
            if (!t2_.update_value_at(res.payload_bits & kOffsetMask, value, mem)) {
              return false;
            }
            pending = wal_.reserve_update(full_key, value);
            in_place = true;
            updated = true;
          }
          break;  // Not an in-place update (doesn't fit alloc_len) -- fall through.
        }

        if (!in_place) {
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
  // commits a checkpoint (reorganize(true)) afterward. Still triggers ordinary T1-only
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

                     std::array<std::byte, kStoreKeyBytes> stack_key;
                     size_t len = kStoreKeyBytes;
                     while (len > 0 && index_key[len - 1] == std::byte{0}) {
                       --len;
                     }
                     std::memcpy(stack_key.data(), index_key.data(), len);
                     std::span<const std::byte> key_view(stack_key.data(), len);
                     // Inline values never reference T2, so they can never generation-mismatch.
                     std::memcpy(last_key.data(), index_key.data(), kStoreKeyBytes);
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

                 // t2_.at() called inside read_t2_record_seqlock() (as AtFunc), matching get_impl() --
                 // see read_t2_record_seqlock()'s comment.
                 read_t2_record_seqlock([&]() -> T2RecordView { return t2_.at(payload & kOffsetMask, mem); },
                                        [&](const T2RecordView &record) -> bool {
                                          if (!key_in_range(record.key, lower_bound, upper_bound)) {
                                            return false;
                                          }
                                          callback(record.key, record.value);
                                          return true;
                                        });
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

  // Whether reorganize_internal() should rebuild T2 (vs. a T1-only, zero-I/O reorganize): T2 has
  // fragmented past threshold, or -- outside recovery -- enough WAL has accumulated since the
  // last checkpoint (low_level_design.md 4.4). Skipped during recovery_: we're inside
  // wal_.replay()'s callback, which holds Wal's (non-reentrant) append_mutex_ for its whole
  // duration, and committing a checkpoint here would call wal_.rotate(), needing that same mutex.
  auto should_upgrade_to_t2() const -> bool {
    const uint64_t live_bytes = t1_.live_bytes();
    const uint64_t t2_used = t2_.bytes_used();
    const double space_amp = live_bytes > 0 ? static_cast<double>(t2_used) / static_cast<double>(live_bytes) : 1.0;
    const double amp_threshold = static_cast<double>(ConfigT::T2StorageFragmentationThresholdPercent) / 100.0 + 1.0;
    if (space_amp >= amp_threshold) {
      return true;
    }
    return !recovering_ && wal_.size_bytes() >= ConfigT::WalMaxBytesSinceCheckpoint;
  }

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
    ::close(file_descriptor);
    if (mapped == MAP_FAILED) {
      throw std::system_error(mmap_errno, std::generic_category(), what);
    }
    if constexpr (ConfigT::UseMadviseRandom) {
      if (::madvise(mapped, capacity, MADV_RANDOM) != 0) {
        throw std::system_error(errno, std::generic_category(), "madvise MADV_RANDOM (checkpoint/reorg)");
      }
    }
    return std::make_unique<vmemkv::T2Memory>(static_cast<std::byte *>(mapped), capacity, generation, bytes_used);
  }

  // Publishes a just-rebuilt T2 as the new checkpoint generation: renames it onto its final path,
  // commits the manifest, rotates the WAL down to checkpoint_lsn's tail, and removes the
  // superseded generation's files. Called only after t2_'s live mapping has already been swapped.
  void commit_checkpoint(uint64_t generation,
                         uint64_t checkpoint_lsn,
                         const std::filesystem::path &temp_path_t2,
                         const std::filesystem::path &t2_chk_final_path,
                         uint64_t next_bytes_used) {
    std::error_code error_code;
    std::filesystem::rename(temp_path_t2, t2_chk_final_path, error_code);
    if (error_code) {
      throw std::system_error(error_code, "rename t2 checkpoint file");
    }

    vmemkv::write_manifest(vmemkv::derive_manifest_path(t2_path()), generation, next_bytes_used);
    wal_.rotate(checkpoint_lsn);

    if (current_generation_.has_value() && *current_generation_ != generation) {
      std::error_code ignored;
      std::filesystem::remove(vmemkv::derive_t1_chk_path(t2_path(), *current_generation_), ignored);
      std::filesystem::remove(vmemkv::derive_t2_chk_path(t2_path(), *current_generation_), ignored);
    }
    current_generation_ = generation;
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

    vmemkv::T1CheckpointFile t1_chk(vmemkv::derive_t1_chk_path(t2_path, manifest->generation));

    const std::filesystem::path t2_chk_path = vmemkv::derive_t2_chk_path(t2_path, manifest->generation);
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
    current_generation_ = manifest->generation;
  }

  // Replays the current contents of wal_ into T1 (and, via write_entry_lockfree, T2) -- whether
  // that's the full history or just the post-checkpoint tail is transparent here. Runs before
  // reorg_worker_ is started, so reorganize() is driven directly rather than via the usual
  // signal-and-wait path (it's public, self-contained, and CAS-guarded, so it runs synchronously
  // with no worker competing for reorg_running_). Without the explicit capacity check below, a
  // WAL with more live distinct keys than one append region holds would livelock inside
  // write_entry_lockfree, which can only escape AppendRegionFull by waiting on a worker that
  // doesn't exist yet.
  void recover_from_wal() {
    wal_.replay([&](vmemkv::WalRecordType type,
                    std::span<const std::byte> key,
                    std::span<const std::byte> value,
                    uint64_t /*lsn*/) {
      if (t1_.append_size() + 1 >= T1IndexT::APPEND_CAP) {
        reorganize(false);
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
    header.flags = 0;
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
    while (!stop_token.stop_requested()) {
      reorg_requested_.wait(false, std::memory_order_acquire);
      if (stop_token.stop_requested()) {
        break;
      }
      reorg_requested_.store(false, std::memory_order_release);

      // CAS, not an unconditional store: reorg_running_ is also claimed by the public
      // reorganize() method, and the two must never both believe they hold it at once (an
      // unconditional store here previously let both end up inside reorganize_internal()
      // concurrently, deadlocking the manual caller in reorg_running_.wait(true, ...) forever).
      // If a manual reorganize() already holds it, this request is redundant -- skip this round.
      bool expected_running = false;
      if (!reorg_running_.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel)) {
        continue;
      }
      try {
        reorganize_internal(false);
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
      while (reorg_running_.load(std::memory_order_acquire)) {
        reorg_running_.wait(true, std::memory_order_acquire);
      }
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

  // Both only ever touched under reorg_running_'s single-flight guarantee (or, for
  // recovering_, only during single-threaded construction before reorg_worker_ exists) --
  // neither needs atomics.
  std::optional<uint64_t> current_generation_;  // Set once a checkpoint has been committed or loaded.
  bool recovering_ = false;                     // True only during the constructor's initial WAL replay.
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

}  // namespace vmemkv
