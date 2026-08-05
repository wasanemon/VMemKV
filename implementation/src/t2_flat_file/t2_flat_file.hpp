// t2_flat_file.hpp - Decoupled Tier 2 Flat Binary File storage management
#pragma once

#include <sys/mman.h>

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

// ─── T2 Flat Storage Structs ──────────────────────────────────────────────

// value_len specifies the current logical size of the value.
// alloc_len specifies the physical size allocated for the value on disk.
// Separating them allows in-place updates (overwrite) when the new value size
// is smaller than or equal to the allocated size, avoiding disk appends.
struct ValueRecordHeader {
  uint32_t key_len;
  uint32_t value_len;
  uint32_t alloc_len;
  uint32_t flags;
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
  // Unique across every T2Memory this process ever constructs, so append_prefault()'s
  // thread-local chunk cache (t2_flat_file.cpp) can detect a stale mapping even when a freed
  // T2Memory's heap address is reused by a new one -- comparing raw T2Memory* is an ABA hazard.
  //
  // Also the pairing tag between a T2Memory and the T1Index::SortedSnapshot built against it:
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
      : base(base_ptr), capacity(capacity_bytes), generation(explicit_generation), bytes_used(initial_bytes_used) {}
  ~T2Memory() noexcept {
    if ((base != nullptr) && capacity > 0) {
      ::munmap(base, static_cast<size_t>(capacity));
    }
  }

  T2Memory(const T2Memory &) = delete;
  auto operator=(const T2Memory &) -> T2Memory & = delete;

  // Every T2Memory this process ever constructs must draw from this one process-global counter,
  // never a fixed/arbitrary value: process-global uniqueness is what append_prefault()'s
  // thread-local chunk cache depends on to detect a freed T2Memory's heap address being reused by
  // an unrelated instance (see `generation`'s declaration above). A fixed value here previously
  // caused two unrelated stores to share a tag, letting a stale thread-local cache write through a
  // freed T2Memory into the wrong store's mapping.
  static auto allocate_generation() noexcept -> uint64_t {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }
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
  struct LookupResult {
    uint64_t offset = ~0ULL;
    T2RecordView record;
  };

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

  // Write-side half of the residual-window fix (see begin_draining_and_wait_for_writers() for
  // the reorganize side): acquires a handle for appending a brand-new T2 record, deferring while
  // a reorganize() is draining its final pre-swap window so no writer ever starts writing into a
  // generation that's about to be retired. Callers appending a new record must hold the returned
  // handle until their T1 publish (T1Index::put()) attempt has returned -- releasing it earlier
  // would let this same generation look "drained" to begin_draining_and_wait_for_writers() before
  // the T1 entry naming it actually exists, reopening the exact race this pairing closes.
  auto acquire_write_handle() const noexcept -> T2MemoryHandle;

  // Reorganize-side half: marks `mem` (the still-live generation about to be retired) as
  // draining, so acquire_write_handle() stops handing it to new callers, then blocks until every
  // writer that already holds a handle to it -- i.e. started before the flag went up -- has
  // released it. Per acquire_write_handle()'s contract that only happens after that writer's T1
  // publish attempt returns, so once this call returns, T1's append_region cannot receive another
  // entry naming `mem`'s generation. Must be paired with end_draining(), including on the
  // exception path (the caller's try/catch already covers this; see reorganize_internal()).
  void begin_draining_and_wait_for_writers(const T2Memory *mem) const noexcept;
  // Un-pairs begin_draining_and_wait_for_writers(); safe to call even if no drain is active.
  void end_draining() const noexcept { draining_.store(false, std::memory_order_release); }

  // Resolves a record at the given payload offset into a structured view.
  // - Contract: The offset must be within bounds. The returned view references the memory base,
  //   and remains valid as long as the 'mem' instance is kept alive.
  auto at(uint64_t payload, const T2Memory *mem) const noexcept -> T2RecordView;

  // ─── Storage Operations ───
  // `mem` must come from acquire_write_handle() (not a plain get_memory_handle()), so the
  // draining check above actually gates new writes -- see that method's contract. The generation
  // to stamp on a T1 entry (see SortedSlot::generation) is simply `mem->generation`, since the
  // caller already holds the exact handle the write lands in.
  //
  // Appends a new key-value record to the end of the flat file using atomic offset allocation.
  static auto append_default(const T2Memory *mem,
                             std::span<const std::byte> key,
                             std::span<const std::byte> value) -> uint64_t;
  // Appends a new key-value record using thread-local chunk allocation and pre-faulting optimization.
  static auto append_prefault(const T2Memory *mem,
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

  // Set by begin_draining_and_wait_for_writers(), cleared by end_draining(); gates
  // acquire_write_handle() so no new writer starts against a generation reorganize_internal() is
  // about to retire. See acquire_write_handle()'s declaration for the full contract.
  mutable std::atomic<bool> draining_{false};
};

}  // namespace vmemkv
