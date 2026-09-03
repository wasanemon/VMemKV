// t2_flat_file.hpp - Decoupled Tier 2 Flat Binary File storage management
#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "../api/utils.hpp"
#include "../core/reference_tracker.hpp"
#include "../core/spin_backoff.hpp"

// ─── T2 Flat Storage Structs ──────────────────────────────────────────────

// value_len specifies the current logical size of the value.
// alloc_len specifies the physical size allocated for the value on disk.
// Separating them allows in-place updates (overwrite) when the new value size
// is smaller than or equal to the allocated size, avoiding disk appends.
struct ValueRecordHeader {
  uint32_t key_len;
  uint32_t value_len;
  uint32_t alloc_len;
  uint64_t version;
};

static_assert(std::is_standard_layout_v<ValueRecordHeader>);
static_assert(alignof(ValueRecordHeader) == alignof(uint64_t));

// A transient read-only view mapped directly to the memory-mapped T2 file.
// Using std::span prevents copying the key/value data and avoids dynamic
// memory allocations, ensuring high read performance.
struct T2RecordView {
  const ValueRecordHeader *header = nullptr;
  std::span<const std::byte> key;
  std::span<const std::byte> value;
};

namespace vmemkv {

struct T2Memory {
  std::byte *base = nullptr;
  uint64_t capacity = 0;

  // Bytes of `capacity` allocated so far. Lives inside T2Memory (rather than as a separate
  // T2FlatFile-level counter) so base/capacity/bytes_used form one atomically-consistent triple
  // via a single get_memory_handle() call. `mutable`: fetch_add()/store() must work through the
  // `const T2Memory *` get_memory_handle() hands back.
  mutable std::atomic<uint64_t> bytes_used{0};

  // The offset below which every byte is (a) durably on disk and (b) guaranteed never to be
  // touched by an in-place update again, and is thus safe to read seqlock-free via the
  // base-region mappings below (`base_mmap_scan`/`base_mmap_scan_seq`/`read_fd`). update_impl()
  // checks this directly to decide whether an in-place update must redirect out-of-place instead
  // (see kOffsetMask's use there). checkpoint_internal() advances this in place, on this same
  // T2Memory instance, once `msync()` over the newly-covered range has succeeded -- never
  // regresses, only ever grows. `mutable`/atomic for the same reason `bytes_used` is: readers and
  // the advancing writer share one T2Memory instance rather than transitioning to a new one.
  mutable std::atomic<uint64_t> base_boundary{0};

  // A second, read-only mmap covering [0, capacity) of this file -- distinct from `base`'s
  // MADV_RANDOM mapping used everywhere else, left at the kernel's default readahead
  // policy (no madvise call). Read by scan_impl() (and get_impl()'s large-record path, via
  // try_read_resident_base_record()) for records whose embedded size hint is larger than one page
  // -- see `base_mmap_scan_seq` below for the small-record counterpart and the "T2 base-region
  // reads" comment in vmemkv_impl.hpp for why there are two. Mapped full-capacity (not just
  // bytes_used-at-creation-time): since both this mapping and `base` are MAP_SHARED over the same
  // file, they always transparently agree -- reads are only ever gated by the `offset <
  // base_boundary` check (scan_impl()), never by whether this specific mapping has "seen" a write.
  // Its lifetime is tied to this T2Memory via the same ThreadReferenceTracker-based retirement
  // scheme that already protects `base`/`capacity`. Set unconditionally by T2FlatFile's
  // constructor; nullptr only if that best-effort mapping failed, in which case the reader falls
  // back to the always-correct `base` + seqlock path.
  // `mutable` only so the destructor (a const-safe operation) can unmap it through the same
  // `const T2Memory *` pattern bytes_used already uses; never mutated after construction
  // otherwise.
  mutable std::byte *base_mmap_scan = nullptr;

  // A third mapping of the identical [0, capacity) region as `base_mmap_scan` above, advised
  // MADV_SEQUENTIAL. Read only by scan_impl(), for records whose embedded size hint is one page
  // or smaller -- get_impl() reads records this small through the primary `base` mapping
  // instead (see the "T2 base-region reads" comment in vmemkv_impl.hpp for why). madvise is a property
  // of the whole mapping, not of an individual read, so Scan's own two size classes need
  // separately-advised mappings rather than one shared policy; which one a given record uses is
  // decided per record (try_read_base_record() in vmemkv_impl.hpp), so
  // a corpus with genuinely mixed record sizes is still handled correctly (just a
  // readahead-policy choice, never a correctness one -- both mappings cover identical bytes).
  // Same lifetime/retirement/best-effort/nullptr-by-default story as base_mmap_scan.
  mutable std::byte *base_mmap_scan_seq = nullptr;

  // A `dup()`'d file descriptor onto the same base-region file `base_mmap_scan`/
  // `base_mmap_scan_seq` above map, used by get_impl() for a bounded `pread()` of one
  // larger-than-one-page record instead of a page-fault-driven mmap read -- see the "T2
  // base-region reads" comment in vmemkv_impl.hpp for why Get's large-record path and Scan want
  // different read mechanisms on the same immutable bytes. `dup()`'d (not the original fd, which
  // T2FlatFile's constructor closes right after mapping) so this handle's lifetime is
  // self-contained and tied to this T2Memory, matching the two mappings' own retirement story.
  // Set unconditionally by that constructor; -1 only if the best-effort dup() failed, in which
  // case get_impl() falls back to the always-correct `base` + seqlock path. `mutable` for the
  // same reason as base_mmap_scan.
  mutable int read_fd = -1;

  // `initial_bytes_used`: for a rebuilt/adopted mapping with live records already at construction
  // time; 0 for a brand-new empty file.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  T2Memory(std::byte *base_ptr, uint64_t capacity_bytes, uint64_t initial_bytes_used) noexcept
      : base(base_ptr), capacity(capacity_bytes), bytes_used(initial_bytes_used), base_boundary(initial_bytes_used) {}

  ~T2Memory() noexcept {
    if ((base != nullptr) && capacity > 0) {
      ::munmap(base, static_cast<size_t>(capacity));
    }
    if (base_mmap_scan != nullptr) {
      // Mapped to `capacity` (see base_mmap_scan's own comment above), not base_boundary.
      ::munmap(base_mmap_scan, static_cast<size_t>(capacity));
    }
    if (base_mmap_scan_seq != nullptr) {
      ::munmap(base_mmap_scan_seq, static_cast<size_t>(capacity));
    }
    if (read_fd >= 0) {
      ::close(read_fd);
    }
  }

  T2Memory(const T2Memory &) = delete;
  auto operator=(const T2Memory &) -> T2Memory & = delete;
};

// Test-only seam for T2FlatFile::acquire_write_handle(); fires once, right after registering the
// handle and before checking `writer_stop_`. No-op in production.
struct NoOpAcquireWriteHandleHook {
  void operator()() const noexcept {}
};

// ─── T2FlatFile Class ──────────────────────────────────────────────────────
//
//   Virtual Memory Space (mmap, size: capacity_)
//   +------------------+------------------+-------------------+-----------+
//   | Record 0 (k1,v1) | Record 1 (k2,v2) | ...               | (Unused)  |
//   +------------------+------------------+-------------------+-----------+
//   ^                  ^                                      ^
//   0                  offset_1                               bytes_used_ (atomic append)
//
//   Record Layout:
//   +----------------------+--------------------+-----------+-------------+
//   | key_len (4B integral)| val_len (4B)       | Key (str) | Value (bin) |
//   +----------------------+--------------------+-----------+-------------+
//
class T2FlatFile {
 public:
  // ─── Types and Constructors ───
  // RAII handle produced by acquire_write_handle(): registers the calling writer in
  // active_writers_ so stop_writers_and_wait() can quiesce in-flight appenders before
  // checkpoint_internal() captures a frontier. Not used by plain readers -- see
  // get_memory_handle()'s comment.
  using T2MemoryHandle = typename ThreadReferenceTracker<const T2Memory *>::Guard;

  // Constructor. Always creates or adopts vmemkv::derive_t2_chk_path(path) -- the single,
  // persistent T2 data file for this store's whole lifetime; `path` itself is never opened
  // directly, only kept (see path()) as the base other sibling paths (manifest/t1chk/wal) derive
  // from.
  // `initial_bytes_used`: nullopt (default) starts from a fresh, empty data file, discarding
  // whatever untrusted, unpublished bytes might already be at derive_t2_chk_path(path) -- see
  // low_level_design.md 5.1 for why those bytes carry no authority without a manifest vouching for
  // them. A value instead adopts the file already there, trusting the caller has already
  // validated it against a committed manifest naming exactly this many bytes used (see
  // VMemKVImpl::adopted_t2_bytes_used()).
  T2FlatFile(const std::filesystem::path &path,
             uint64_t bytes_capacity,
             std::optional<uint64_t> initial_bytes_used = std::nullopt);
  ~T2FlatFile() noexcept;

  T2FlatFile(const T2FlatFile &) = delete;
  auto operator=(const T2FlatFile &) -> T2FlatFile & = delete;

  // ─── Memory and Record Access ───
  // Returns the current T2Memory instance. There is exactly one T2Memory for a store's whole
  // process lifetime (no relocating rebuild ever replaces it), so a plain pointer is already
  // stable for as long as this T2FlatFile itself is alive -- readers need no reference-counted
  // handle to keep it alive across the call. `get_memory_handle()` is a same-named alias kept for
  // call sites that read via `mem->...`; it does not register with `active_writers_`, which is
  // reserved for acquire_write_handle()'s writer-quiesce contract (see stop_writers_and_wait()).
  auto get_memory() const noexcept -> const T2Memory * { return t2_mem_.load(std::memory_order_acquire); }

  auto get_memory_handle() const noexcept -> const T2Memory * { return get_memory(); }

  // Write-side half of the residual-window fix (see stop_writers_and_wait() for the checkpoint
  // side): acquires a handle for appending a brand-new T2 record, deferring while
  // checkpoint_internal() has new writers stopped for its target-capture window so no append can
  // land past the frontier it's about to durabilize. Callers appending a new record must hold the
  // returned handle until their T1 publish (T1Index::put()) attempt has returned -- releasing it
  // earlier would let this write look "stopped" to stop_writers_and_wait() before the T1 entry
  // naming it actually exists, reopening the exact race this pairing closes.
  //
  // Registers first, then checks `writer_stop_` -- checking first would leave a gap: a writer
  // whose check saw writer_stop_==false, but who hadn't registered yet, could still be missed by
  // wait_until_retired()'s single index-ordered pass (it never revisits a slot once past it), so
  // the stop could complete while the writer went on to write past the frontier being captured.
  // Registering first closes this: if writer_stop_ turns out to already be true, this handle is
  // dropped and retried, so no caller ever receives a handle for a write the stop might have
  // already (or might be about to) declare fully quiesced past. `hook` is a test-only seam,
  // firing once right after registering and before the writer_stop_ check -- lets a test pause a
  // writer in exactly that window to reproduce the race deterministically; no-op in production.
  //
  // The outer while(writer_stop_) loop below deliberately does NOT touch active_writers_ at all:
  // retrying the register-then-check dance immediately on rejection would let a rejected writer's
  // slot toggle between the retired value and cleared rapidly enough that
  // stop_writers_and_wait()'s drain check could lose that race indefinitely under sustained write
  // load. Waiting out here first means a writer only touches active_writers_ once per genuine
  // writer_stop_ transition, so once past that transition the tracked count can only drain, never
  // bounce back up. This doesn't reopen the gap the ordering above protects against: the
  // register-then-check pair itself still runs exactly once per genuine transition, just never
  // retried in a tight loop while already known to be rejected.
  //
  // Registers/releases through active_writers_ directly (rather than a named T2MemoryHandle
  // local) on the retry path: T2MemoryHandle's Guard has a deleted copy constructor and no
  // implicit move constructor, so `return handle;` for a named local isn't guaranteed elided
  // (NRVO is optional, unlike a prvalue return) -- only the success path constructs one, as a
  // prvalue in the return statement itself, which mandatory copy elision does cover.
  template <typename Hook = NoOpAcquireWriteHandleHook>
  auto acquire_write_handle(Hook &&hook = {}) const noexcept -> T2MemoryHandle {
    while (true) {
      vmemkv::SpinBackoff backoff;
      while (writer_stop_.load(std::memory_order_seq_cst)) {
        backoff.wait();
      }
      const T2Memory *mem = get_memory();
      active_writers_.acquire(mem);
      hook();
      if (!writer_stop_.load(std::memory_order_seq_cst)) {
        return {active_writers_, mem};  // re-acquire()s the same value; harmless.
      }
      active_writers_.release();
    }
  }

  // Checkpoint-side half: marks `mem` (the live T2Memory checkpoint_internal() is about to capture
  // a frontier from) as stopped-for-writers, so acquire_write_handle() stops handing it to new
  // callers, then blocks until every writer that already holds a handle to it -- i.e. started
  // before the flag went up -- has released it. Per acquire_write_handle()'s contract that only
  // happens after that writer's T1 publish attempt returns, so once this call returns, T1's
  // append_region cannot receive another entry naming a T2 offset past the frontier about to be
  // captured. Must be paired with resume_writers(), including on the exception path (the caller's
  // try/catch already covers this; see checkpoint_internal()).
  void stop_writers_and_wait(const T2Memory *mem) const noexcept;
  // Un-pairs stop_writers_and_wait(); safe to call even if writers aren't currently stopped.
  void resume_writers() const noexcept { writer_stop_.store(false, std::memory_order_release); }

  // Resolves a record at the given payload offset into a structured view.
  // - Contract: The offset must be within bounds. The returned view references the memory base,
  //   and remains valid as long as the 'mem' instance is kept alive.
  static auto at(uint64_t payload, const T2Memory *mem) noexcept -> T2RecordView;

  // ─── Storage Operations ───
  // `mem` must come from acquire_write_handle() (not a plain get_memory_handle()), so the
  // writer_stop_ check above actually gates new writes -- see that method's contract.
  //
  // Appends a new key-value record to the end of the flat file using atomic offset allocation.
  static auto append_default(const T2Memory *mem,
                             std::span<const std::byte> key,
                             std::span<const std::byte> value) -> uint64_t;
  // Updates the value of an existing record in-place if the new value fits within alloc_len.
  // - Thread-safety: Thread-safe for distinct keys (callers hold per-key stripe lock).
  // - Guarantees: Returns true on success; false if new value exceeds alloc_len.
  static auto update_value_at(uint64_t payload, std::span<const std::byte> value, const T2Memory *mem) noexcept -> bool;

  // ─── Properties ───
  // Bytes used in the T2 file -- via get_memory_handle(), not a separate counter; see
  // T2Memory::bytes_used's declaration.
  auto bytes_used() const noexcept -> uint64_t {
    return get_memory_handle()->bytes_used.load(std::memory_order_acquire);
  }
  auto path() const noexcept -> const std::filesystem::path & { return path_; }

 private:
  // ─── Private Helpers ───
  // Helper to resolve a raw payload offset to a memory pointer.
  static auto resolve_record(uint64_t payload, const T2Memory *mem) noexcept -> const std::byte * {
    assert(payload < mem->capacity);
    return mem->base + payload;
  }

  // Helper to create and pre-allocate an empty binary file on disk.
  static void create_empty_file(const std::filesystem::path &path, uint64_t bytes_capacity);

  void map_file(const std::filesystem::path &path, uint64_t bytes_capacity, uint64_t initial_bytes_used);

  // ─── Member Variables ───
  // The identity path passed to the constructor -- NOT the file actually opened/mapped (that's
  // always vmemkv::derive_t2_chk_path(path_), see the constructor's doc comment). Sibling paths
  // (manifest/t1chk/wal) are derived from this one, unsuffixed value.
  std::filesystem::path path_;

  std::atomic<const T2Memory *> t2_mem_{nullptr};

  // Tracks only in-flight writers (acquire_write_handle() registrants) -- plain readers
  // (get_impl()/scan_impl()/try_in_place_update(), via get_memory_handle()) never register here.
  // The sole purpose is stop_writers_and_wait()'s quiesce: closing the residual-window race where
  // an append could land past the frontier checkpoint_internal() is about to durabilize. That race
  // is about appenders publishing new T2 offsets, not readers, so excluding readers only shortens
  // the wait -- it does not weaken the guarantee.
  mutable ThreadReferenceTracker<const T2Memory *> active_writers_;

  // Set by stop_writers_and_wait(), cleared by resume_writers(); gates acquire_write_handle() so
  // no new writer appends past the frontier checkpoint_internal() is about to capture. See
  // acquire_write_handle()'s declaration for the full contract.
  mutable std::atomic<bool> writer_stop_{false};
};

}  // namespace vmemkv
