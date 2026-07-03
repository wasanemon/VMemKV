// t2_flat_file.hpp - Decoupled Tier 2 Flat Binary File storage management
#pragma once

#include <sys/mman.h>

#include <atomic>
#include <cassert>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "../api/utils.hpp"

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

  T2Memory(std::byte *base_ptr, uint64_t capacity_bytes) noexcept : base(base_ptr), capacity(capacity_bytes) {}
  ~T2Memory() noexcept {
    if ((base != nullptr) && capacity > 0) {
      ::munmap(base, static_cast<size_t>(capacity));
    }
  }

  T2Memory(const T2Memory &) = delete;
  auto operator=(const T2Memory &) -> T2Memory & = delete;
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

  // Constructor. Creates or maps a T2 flat binary file on disk.
  T2FlatFile(const std::filesystem::path &path, uint64_t bytes_capacity);
  ~T2FlatFile() noexcept = default;

  T2FlatFile(const T2FlatFile &) = delete;
  auto operator=(const T2FlatFile &) -> T2FlatFile & = delete;

  // ─── Memory and Record Access ───
  // Acquires a shared reference to the current mapped memory region.
  // - Thread-safety: Thread-safe; returned pointer guards against concurrent file munmap.
  // - Contract: Callers must hold the returned std::shared_ptr for the duration of record accesses.
  auto get_memory() const noexcept -> std::shared_ptr<const T2Memory> {
    return t2_mem_.load(std::memory_order_acquire);
  }

  // Resolves a record at the given payload offset into a structured view.
  // - Contract: The offset must be within bounds. The returned view references the memory base,
  //   and remains valid as long as the shared 'mem' instance is kept alive.
  auto at(uint64_t payload, const std::shared_ptr<const T2Memory> &mem) const noexcept -> T2RecordView;

  // ─── Storage Operations ───
  // Appends a new key-value record to the end of the flat file.
  // - Thread-safety: Thread-safe; the offset is reserved atomically via fetch_add, and each
  //   thread writes to its own non-overlapping region. No locking required.
  // - Guarantees: Returns the start offset of the new record.
  auto append(std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t;
  // Updates the value of an existing record in-place if the new value fits within alloc_len.
  // - Thread-safety: Thread-safe for distinct keys (callers hold per-key stripe lock).
  // - Guarantees: Returns true on success; false if new value exceeds alloc_len.
  auto update_value_at(uint64_t payload, std::span<const std::byte> value) noexcept -> bool;
  // Swaps the active memory-mapped region with a newly mapped file/capacity.
  // - Guarantees: Thread-safely replaces the underlying atomic pointer, allowing readers
  //   to transition seamlessly without blocking.
  void swap_memory(const std::shared_ptr<const T2Memory> &new_mem, uint64_t bytes_used);

  // ─── Properties ───
  // Returns the number of bytes currently used/allocated in the T2 file.
  auto bytes_used() const noexcept -> uint64_t { return t2_bytes_used_.load(std::memory_order_acquire); }
  // Returns the total virtual memory capacity mapped for the T2 file.
  auto bytes_capacity() const noexcept -> uint64_t { return t2_bytes_capacity_; }

 private:
  // ─── Private Helpers ───
  // Helper to resolve a raw payload offset to a memory pointer.
  static auto resolve_record(uint64_t payload,
                             const std::shared_ptr<const T2Memory> &mem) noexcept -> const std::byte * {
    assert(payload < mem->capacity);
    return mem->base + payload;
  }

  // Helper to create and pre-allocate an empty binary file on disk.
  static auto create_empty_file(const std::filesystem::path &path, uint64_t bytes_capacity) -> uint64_t;

  void map_file(const std::filesystem::path &path, uint64_t bytes_capacity);

  // ─── Member Variables ───
  std::filesystem::path path_;
  uint64_t t2_bytes_capacity_ = 0;

  std::atomic<std::shared_ptr<const T2Memory>> t2_mem_;
  std::atomic<uint64_t> t2_bytes_used_{0};
};

}  // namespace vmemkv
