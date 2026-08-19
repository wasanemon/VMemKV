// t2_flat_file.hpp - Decoupled Tier 2 Flat Binary File storage management
#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <memory>
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
  // Unique across every T2Memory this process ever constructs -- the pairing tag between a
  // T2Memory and the T1Index::SortedSnapshot built against it:
  // VMemKVImpl::reorganize_internal() stamps the same generation onto both, so a reader can tell
  // whether the pair it holds is self-consistent.
  uint64_t generation;

  // Bytes of `capacity` allocated so far, for *this* generation. Lives per-T2Memory rather than as
  // one T2FlatFile-level counter: a shared counter read separately from `mem` (base/capacity) lets
  // a concurrent swap_memory() reset it to a new generation's value between a writer capturing
  // `mem` and its fetch_add(), producing an offset valid for the new generation but written
  // through the old, stale `base` -- silently clobbering a live record. Keeping bytes_used inside
  // T2Memory makes base/capacity/bytes_used one atomically-consistent triple via a single
  // get_memory_handle() call. `mutable`: fetch_add()/store() must work through the `const
  // T2Memory *` get_memory_handle() hands back.
  mutable std::atomic<uint64_t> bytes_used{0};

  // The offset below which every byte is (a) durably on disk and (b) guaranteed never to be
  // touched by an in-place update again, and is thus safe to read seqlock-free via the
  // base-region mappings below (`base_mmap_scan`/`base_mmap_scan_seq`/`read_fd`). update_impl()
  // checks this directly to decide whether an in-place update must redirect out-of-place instead
  // (see kOffsetMask's use there). Constant for this T2Memory's entire lifetime, set once at
  // construction from `initial_bytes_used` -- checkpoint_internal() only ever grows it by
  // publishing a whole new generation (a fresh T2Memory) via swap_memory(), never by mutating an
  // existing one in place, so no atomic/synchronization is needed here: publication of the new
  // T2Memory itself (via T2FlatFile's atomic pointer swap) already establishes the necessary
  // happens-before relationship for every plain field in it, this one included.
  uint64_t base_boundary = 0;

  // A second, read-only mmap covering [0, capacity) of this exact generation's file -- distinct
  // from `base`'s MAP_PRIVATE|MADV_RANDOM mapping used everywhere else, left at the kernel's
  // default readahead policy (no madvise call). Read by scan_impl() (and get_impl()'s
  // large-record path, via try_read_resident_base_record()) for records whose embedded size
  // hint is larger than one page -- see `base_mmap_scan_seq` below for the small-record
  // counterpart and the "T2 base-region reads" comment in vmemkv_impl.hpp for why there are two.
  // Mapped full-capacity (not just bytes_used-at-creation-time): since this mapping is
  // PROT_READ-only, it never triggers copy-on-write, so it always transparently reflects whatever
  // the backing file's current page-cache content is, including bytes written by
  // checkpoint_internal()'s pwrite() through a different fd -- reads are only ever gated by the
  // `offset < base_boundary` check (scan_impl()), never by whether this specific mapping has
  // "seen" a write. Its lifetime is tied to this T2Memory via the same ThreadReferenceTracker-based
  // retirement scheme that already protects `base`/`capacity`. nullptr unless explicitly set by
  // the caller (mmap_t2_memory() in vmemkv_impl.hpp -- best-effort -- a failure to create it just
  // means the reader falls back to the
  // always-correct `base` + seqlock path) -- the plain T2FlatFile-owned initial mapping never
  // sets this, since a fresh store has base_boundary == 0 and thus nothing to map yet.
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
  // decided per record (try_read_base_record() in vmemkv_impl.hpp), not once per generation, so
  // a corpus with genuinely mixed record sizes is still handled correctly (just a
  // readahead-policy choice, never a correctness one -- both mappings cover identical bytes).
  // Same lifetime/retirement/best-effort/nullptr-by-default story as base_mmap_scan.
  mutable std::byte *base_mmap_scan_seq = nullptr;

  // A `dup()`'d file descriptor onto the same base-region file `base_mmap_scan`/
  // `base_mmap_scan_seq` above map, used by get_impl() for a bounded `pread()` of one
  // larger-than-one-page record instead of a page-fault-driven mmap read -- see the "T2
  // base-region reads" comment in vmemkv_impl.hpp for why Get's large-record path and Scan want
  // different read mechanisms on the same immutable bytes. `dup()`'d (not the original fd, which
  // mmap_t2_memory() always closes right after mapping) so this handle's lifetime is self-
  // contained and tied to this T2Memory, matching the two mappings' own retirement story. -1
  // unless explicitly set by the caller (mmap_t2_memory() -- best-effort -- a dup() failure
  // just means get_impl() falls back to the always-correct
  // `base` + seqlock path) -- the plain T2FlatFile-owned initial mapping never sets this, for the
  // same reason it never sets base_mmap_scan. `mutable` for the same reason as base_mmap_scan.
  mutable int read_fd = -1;

  // Auto-assigns a fresh, process-global-unique generation. Used where no caller needs to know
  // the value in advance (e.g. tests exercising the ABA-detection field directly).
  T2Memory(std::byte *base_ptr, uint64_t capacity_bytes) noexcept
      : base(base_ptr), capacity(capacity_bytes), generation(allocate_generation()) {}

  // Uses a caller-supplied generation -- e.g. to tag a paired T1Index::SortedSnapshot with an
  // identical id before this T2Memory exists. `initial_bytes_used`: for a rebuilt/adopted mapping
  // with live records already at construction time; 0 for a brand-new empty file.
  T2Memory(std::byte *base_ptr,
           // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
           uint64_t capacity_bytes,
           uint64_t explicit_generation,
           uint64_t initial_bytes_used = 0) noexcept
      : base(base_ptr),
        capacity(capacity_bytes),
        generation(explicit_generation),
        bytes_used(initial_bytes_used),
        base_boundary(initial_bytes_used) {}
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

  // Every T2Memory this process ever constructs must draw from this one process-global counter,
  // never a fixed/arbitrary value -- see `generation`'s declaration above for why uniqueness
  // matters (the T1Index::SortedSnapshot pairing check).
  static auto allocate_generation() noexcept -> uint64_t {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }
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
  // RAII handle for lock-free reader thread safety without shared_ptr copy overhead.
  using T2MemoryHandle = typename ThreadReferenceTracker<const T2Memory *>::Guard;

  // Constructor. Creates or maps a T2 flat binary file on disk.
  // `initial_generation`: stamped on the first T2Memory this instance maps. Must come from
  // T2Memory::allocate_generation(), not an arbitrary value -- see T2Memory::generation's
  // declaration for why a fixed value is unsafe.
  T2FlatFile(const std::filesystem::path &path, uint64_t bytes_capacity, uint64_t initial_generation);
  ~T2FlatFile() noexcept;

  T2FlatFile(const T2FlatFile &) = delete;
  auto operator=(const T2FlatFile &) -> T2FlatFile & = delete;

  // ─── Memory and Record Access ───
  // Acquires a shared reference to the current mapped memory region.
  // - Thread-safety: Thread-safe; returned pointer guards against concurrent file munmap.
  // - Contract: Callers must hold the returned pointer (via T2MemoryHandle) for the duration of record accesses.
  auto get_memory() const noexcept -> const T2Memory * { return t2_mem_.load(std::memory_order_acquire); }

  auto get_memory_handle() const noexcept -> T2MemoryHandle { return {active_readers_, get_memory()}; }

  // Write-side half of the residual-window fix (see stop_writers_and_wait() for the reorganize
  // side): acquires a handle for appending a brand-new T2 record, deferring while a reorganize()
  // has new writers stopped for its final pre-swap window so no writer ever starts writing into a
  // generation that's about to be retired. Callers appending a new record must hold the returned
  // handle until their T1 publish (T1Index::put()) attempt has returned -- releasing it earlier
  // would let this same generation look "stopped" to stop_writers_and_wait() before the T1 entry
  // naming it actually exists, reopening the exact race this pairing closes.
  //
  // Registers first, then checks `writer_stop_` -- checking first would leave a gap: a writer
  // whose check saw writer_stop_==false, but who hadn't registered yet, could still be missed by
  // wait_until_retired()'s single index-ordered pass (it never revisits a slot once past it), so
  // the stop could complete while the writer went on to write into a generation about to be
  // retired. Registering first closes this: if writer_stop_ turns out to already be true, this
  // handle is dropped and retried, so no caller ever receives a handle to a generation the stop
  // might have already (or might be about to) declare fully quiesced. `hook` is a test-only seam,
  // firing once right after registering and before the writer_stop_ check -- lets a test pause a
  // writer in exactly that window to reproduce the race deterministically; no-op in production.
  //
  // The outer while(writer_stop_) loop below deliberately does NOT touch active_readers_ at all:
  // measured directly (19 concurrent insert threads) that retrying the register-then-check dance
  // immediately on rejection turns wait_until_retired()'s drain check into a race it can lose
  // indefinitely -- a rejected writer's slot toggles between the retired value and cleared so
  // rapidly that the polling side may never observe a gap, so stop_writers_and_wait() (called by
  // every checkpoint()/defragment() cycle) could hang under sustained write load. Waiting out here
  // first means a writer only touches active_readers_ once per genuine writer_stop_ transition (a
  // single, bounded register-and-immediately-release at the edge), not once per retry -- so once
  // past that transition, the tracked count can only drain, never bounce back up, making the wait
  // deterministic instead of probabilistic. This still doesn't reopen the gap the ordering above
  // protects against: the actual register-then-check pair is unchanged, just no longer attempted
  // in a tight loop while already known to be rejected.
  //
  // Registers/releases through active_readers_ directly (rather than a named T2MemoryHandle
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
      active_readers_.acquire(mem);
      hook();
      if (!writer_stop_.load(std::memory_order_seq_cst)) {
        return {active_readers_, mem};  // re-acquire()s the same value; harmless.
      }
      active_readers_.release();
    }
  }

  // Reorganize-side half: marks `mem` (the still-live generation about to be retired) as
  // stopped-for-writers, so acquire_write_handle() stops handing it to new callers, then blocks
  // until every writer that already holds a handle to it -- i.e. started before the flag went
  // up -- has released it. Per acquire_write_handle()'s contract that only happens after that
  // writer's T1 publish attempt returns, so once this call returns, T1's append_region cannot
  // receive another entry naming `mem`'s generation. Must be paired with resume_writers(),
  // including on the exception path (the caller's try/catch already covers this; see
  // reorganize_internal()).
  void stop_writers_and_wait(const T2Memory *mem) const noexcept;
  // Un-pairs stop_writers_and_wait(); safe to call even if writers aren't currently stopped.
  void resume_writers() const noexcept { writer_stop_.store(false, std::memory_order_release); }

  // Resolves a record at the given payload offset into a structured view.
  // - Contract: The offset must be within bounds. The returned view references the memory base,
  //   and remains valid as long as the 'mem' instance is kept alive.
  auto at(uint64_t payload, const T2Memory *mem) const noexcept -> T2RecordView;

  // ─── Storage Operations ───
  // `mem` must come from acquire_write_handle() (not a plain get_memory_handle()), so the
  // writer_stop_ check above actually gates new writes -- see that method's contract. The generation
  // to stamp on a T1 entry (see SortedSlot::generation) is simply `mem->generation`, since the
  // caller already holds the exact handle the write lands in.
  //
  // Appends a new key-value record to the end of the flat file using atomic offset allocation.
  static auto append_default(const T2Memory *mem,
                             std::span<const std::byte> key,
                             std::span<const std::byte> value) -> uint64_t;
  // Updates the value of an existing record in-place if the new value fits within alloc_len.
  // - Thread-safety: Thread-safe for distinct keys (callers hold per-key stripe lock).
  // - Guarantees: Returns true on success; false if new value exceeds alloc_len.
  auto update_value_at(uint64_t payload, std::span<const std::byte> value) const noexcept -> bool;
  // Same as above, but resolves against a caller-supplied `mem` rather than self-acquiring the
  // current one, so a caller that already validated `payload` against a specific generation writes
  // through that exact mapping instead of a possibly-newer one a concurrent reorganize() swapped in.
  static auto update_value_at(uint64_t payload, std::span<const std::byte> value, const T2Memory *mem) noexcept -> bool;
  // Swaps the active memory-mapped region with a newly mapped file/capacity.
  // - Guarantees: thread-safely replaces the atomic pointer; readers transition without blocking.
  // - Contract: `new_mem->bytes_used` must already hold its correct initial value before this call
  //   (set via the T2Memory constructor's `initial_bytes_used`) -- see T2Memory::bytes_used's
  //   declaration for the race this avoids.
  void swap_memory(std::unique_ptr<T2Memory> new_mem);

  // ─── Properties ───
  // Bytes used in the T2 file (current generation) -- via get_memory_handle(), not a separate
  // counter; see T2Memory::bytes_used's declaration.
  auto bytes_used() const noexcept -> uint64_t {
    return get_memory_handle()->bytes_used.load(std::memory_order_acquire);
  }
  // Total virtual memory capacity mapped for the T2 file (current generation).
  auto bytes_capacity() const noexcept -> uint64_t { return get_memory_handle()->capacity; }
  auto path() const noexcept -> const std::filesystem::path & { return path_; }

 private:
  // ─── Private Helpers ───
  // Helper to resolve a raw payload offset to a memory pointer.
  static auto resolve_record(uint64_t payload, const T2Memory *mem) noexcept -> const std::byte * {
    assert(payload < mem->capacity);
    return mem->base + payload;
  }

  // Helper to create and pre-allocate an empty binary file on disk.
  static auto create_empty_file(const std::filesystem::path &path, uint64_t bytes_capacity) -> uint64_t;

  void map_file(const std::filesystem::path &path, uint64_t bytes_capacity, uint64_t initial_generation);

  void retire_memory(const T2Memory *old_mem);

  // ─── Member Variables ───
  std::filesystem::path path_;

  std::atomic<const T2Memory *> t2_mem_{nullptr};

  mutable ThreadReferenceTracker<const T2Memory *> active_readers_;

  // Set by stop_writers_and_wait(), cleared by resume_writers(); gates acquire_write_handle() so
  // no new writer starts against a generation reorganize_internal() is about to retire. See
  // acquire_write_handle()'s declaration for the full contract.
  mutable std::atomic<bool> writer_stop_{false};
};

}  // namespace vmemkv
