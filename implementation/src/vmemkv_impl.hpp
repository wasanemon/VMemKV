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
#include <system_error>
#include <thread>
#include <vector>
#include <vmemkv/config.hpp>

#include "t1_index/t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"

inline constexpr std::size_t kCacheLineAlignment = 64;

struct alignas(kCacheLineAlignment) AlignedMutex {
  std::mutex mu;
  std::atomic<uint64_t> live_count{0};
  std::atomic<uint64_t> delete_count{0};
};

namespace vmemkv {

template <typename CopyFunc>
inline auto read_t2_record_seqlock(const T2RecordView &record, CopyFunc copy_func) {
  auto atomic_version = std::atomic_ref<const uint64_t>(record.header->version);
  while (true) {
    uint64_t v1 = atomic_version.load(std::memory_order_acquire);
    if (v1 % 2 != 0) {
      std::this_thread::yield();
      continue;
    }

    auto result = copy_func();

    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t v2 = atomic_version.load(std::memory_order_acquire);
    if (v1 == v2) {
      return result;
    }
  }
}

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

  // Constructor. Creates a VMemKV coordination instance.
  VMemKVImpl(const std::filesystem::path &t2_path, uint64_t t2_bytes_capacity) : t2_(t2_path, t2_bytes_capacity) {
    reorg_worker_ = std::jthread(&VMemKVImpl::reorg_worker_loop, this);
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

  // Reclaims fragmented disk space from deleted/updated T2 records.
  // - Workflow:
  //   1. Merges active records from both sorted and append regions of T1.
  //   2. Writes live records into a temporary T2 file sequentially (garbage collection).
  //   3. Hot-swaps the memory-mapped T2 view and publishes the rebuilt T1 index.
  // - Thread-safety: Thread-safe; synchronized internally via reorganize_mutex_.
  // Pure, non-blocking internal reorganize logic (used under acquired reorg_running_ lock)
  void reorganize_internal(bool force_t2_gc) {
    bool upgrade_to_t2 = force_t2_gc;

    if (!upgrade_to_t2) {
      uint64_t live_bytes = t1_.live_bytes();
      uint64_t t2_used = t2_.bytes_used();
      double space_amp = 1.0;
      if (live_bytes > 0) {
        space_amp = static_cast<double>(t2_used) / static_cast<double>(live_bytes);
      }

      const double amp_threshold = static_cast<double>(ConfigT::T2StorageFragmentationThresholdPercent) / 100.0 + 1.0;
      upgrade_to_t2 = (space_amp >= amp_threshold);
    }

    if (upgrade_to_t2) {
      // T1 + T2 Reorganize (Checkpoint reload & physical storage reclamation)
      std::filesystem::path temp_path_t2 = t2_.path().parent_path() / "t2_flat.tmp";

      std::error_code error_code;
      std::filesystem::remove(temp_path_t2, error_code);

      const int temp_fd = ::open(temp_path_t2.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (temp_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open temp file");
      }

      uint64_t next_bytes_used = 0;
      struct FDGuard {
        int fd;
        ~FDGuard() {
          if (fd >= 0) {
            ::close(fd);
          }
        }
      } fd_guard{temp_fd};

      try {
        t1_.reorganize([&](uint64_t old_payload, uint64_t old_hash) -> uint64_t {
          if (old_payload == vmemkv::STORE_NOT_FOUND) {
            return vmemkv::STORE_NOT_FOUND;
          }

          if constexpr (ConfigT::UseT1InlineValue) {
            if (t1_detail::is_inline(old_hash)) {
              return old_payload;
            }
          }

          T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
          uint64_t pure_offset = old_payload & kOffsetMask;
          T2RecordView record = t2_.at(pure_offset, mem);
          uint64_t new_offset = write_record_to_temp_fd(fd_guard.fd, record.key, record.value, next_bytes_used);

          uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + record.key.size() + record.value.size());
          uint64_t block_count = aligned_len / kBlockAlignment;
          assert(block_count < 65536 && "Record size exceeds 1.04MB limit");
          return new_offset | (block_count << kSizeEmbeddingShift);
        });

        ::close(fd_guard.fd);
        fd_guard.fd = -1;

        uint64_t new_capacity = t2_.bytes_capacity();
        if (next_bytes_used > new_capacity) {
          new_capacity = next_bytes_used;
        }

        if (::truncate(temp_path_t2.c_str(), static_cast<off_t>(new_capacity)) != 0) {
          throw std::system_error(errno, std::generic_category(), "truncate temp file");
        }

        const int map_fd = ::open(temp_path_t2.c_str(), O_RDWR);
        if (map_fd < 0) {
          throw std::system_error(errno, std::generic_category(), "open temp file for mmap");
        }

        std::unique_ptr<vmemkv::T2Memory> new_mem = std::make_unique<vmemkv::T2Memory>(
            static_cast<std::byte *>(
                ::mmap(nullptr, new_capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE, map_fd, 0)),
            new_capacity);
        ::close(map_fd);

        if (new_mem->base == MAP_FAILED) {
          throw std::system_error(errno, std::generic_category(), "mmap temp file");
        }

        t2_.swap_memory(std::move(new_mem), next_bytes_used);

        std::filesystem::rename(temp_path_t2, t2_path(), error_code);
        if (error_code) {
          throw std::system_error(error_code, "rename temp file");
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
      // T1-only Reorganize (Zero I/O, fast path)
      t1_.reorganize([&](uint64_t old_payload, uint64_t /*old_hash*/) -> uint64_t { return old_payload; });
      reorg_t1_count_.fetch_add(1, std::memory_order_relaxed);
      reset_tombstone_counters();
    }
    scan_active_.store(false, std::memory_order_relaxed);
  }

  // Public, blocking-guaranteed synchronize method
  void reorganize(bool force_t2_gc = true) {
    while (true) {
      // 1. Wait for any concurrent background/manual reorganize to complete
      while (reorg_running_.load(std::memory_order_acquire)) {
        reorg_running_.wait(true, std::memory_order_acquire);
      }

      // 2. Check if we actually need to run reorganize (is append region empty?)
      if (t1_.append_size() == 0) {
        break;  // Empty, success!
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
        if (put_result == T1IndexT::PutResult::AppendRegionFull) {
          maybe_reorganize_if_needed();
          continue;
        }
      }

      uint64_t offset;
      if constexpr (ConfigT::UsePrefaulting) {
        offset = t2_.append_prefault(full_key, value);
      } else {
        offset = t2_.append_default(full_key, value);
      }
      uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + full_key.size() + value.size());
      uint64_t block_count = aligned_len / kBlockAlignment;
      assert(block_count < 65536 && "Record size exceeds 1.04MB limit");
      uint64_t encoded_payload = offset | (block_count << kSizeEmbeddingShift);
      const auto put_result = t1_.put(full_key, encoded_payload, false, 0);
      if (put_result == T1IndexT::PutResult::Applied) {
        return true;
      }
      if (put_result == T1IndexT::PutResult::AppendRegionFull) {
        // The appended T2 record becomes unreachable garbage and will be reclaimed by reorganize.
        maybe_reorganize_if_needed();
        continue;
      }
    }
  }

  // Inserts a new key-value pair.
  // - Ordering: Writing to T2 (File) must strictly precede T1 (Index) write
  //   to prevent concurrent readers from encountering dangling offsets in T1.
  // - Thread-safety: Thread-safe (guarded by slot-level spinlocks for writes).
  auto insert_impl(std::span<const std::byte> full_key, std::span<const std::byte> value) -> bool {
    std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

    maybe_reorganize_if_needed();

    if (t1_.get(full_key) != vmemkv::STORE_NOT_FOUND) {
      return false;
    }

    if (write_entry_lockfree(full_key, value)) {
      stripe_state(full_key).live_count.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  // Retrieves a value associated with the key and invokes the callback with its raw bytes.
  // - Thread-safety: Lock-free (concurrently readable during index reorganization).
  // - Guarantees: Resolves inline value directly, otherwise reads T2 record at the T1 offset.
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

    T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
    const T2RecordView record = t2_.at(res.payload_bits & kOffsetMask, mem);

    bool success = false;
    read_t2_record_seqlock(record, [&]() -> bool {
      if (!byte_span_equal(record.key, full_key)) {
        return false;
      }
      callback(record.value);
      success = true;
      return true;
    });

    return success;
  }

  // Updates the value of an existing key.
  // - Ordering: Performs in-place updates directly on T2 if allocation size matches,
  //   otherwise appends to T2 first before updating the pointer in T1.
  // - Thread-safety: Thread-safe (guarded by key hash locks).
  auto update_impl(std::span<const std::byte> full_key, std::span<const std::byte> value) -> bool {
    std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

    const auto res = t1_.get_with_hash(full_key);
    if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
      return false;
    }

    if (!t1_detail::is_inline(res.raw_hash)) {
      T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
      const T2RecordView record = t2_.at(res.payload_bits & kOffsetMask, mem);
      if (byte_span_equal(record.key, full_key) && value.size() <= record.header->alloc_len) {
        return t2_.update_value_at(res.payload_bits & kOffsetMask, value);
      }
    }

    maybe_reorganize_if_needed();

    return write_entry_lockfree(full_key, value);
  }

  // Logically removes a key from the store.
  // - Guarantees: Marks the key offset as STORE_NOT_FOUND in T1 (physical space reclamation is deferred to reorganize).
  // - Thread-safety: Thread-safe (guarded by key hash locks).
  auto remove_impl(std::span<const std::byte> full_key) -> bool {
    auto &stripe = stripe_state(full_key);
    {
      std::lock_guard<std::mutex> key_lock(stripe.mu);

      uint64_t payload = t1_.get(full_key);
      if (payload == vmemkv::STORE_NOT_FOUND) {
        return false;
      }

      if (t1_.put(full_key, vmemkv::STORE_NOT_FOUND) == T1IndexT::PutResult::Applied) {
        stripe.live_count.fetch_sub(1, std::memory_order_relaxed);
        stripe.delete_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        return false;
      }
    }

    maybe_reorganize_if_needed_for_delete(stripe);
    return true;
  }

  // Performs a range scan, returning key-value pairs decoded via the callback.
  // - Thread-safety: Thread-safe and concurrently readable.
  // - Guarantees: Invokes Callback(key, val) for each matching live entry in sorted range.
  template <typename Callback>
  auto scan_impl(std::span<const std::byte> lower_bound,
                 std::span<const std::byte> upper_bound,
                 Callback callback) const -> size_t {
    if (!scan_active_.load(std::memory_order_relaxed)) {
      scan_active_.store(true, std::memory_order_relaxed);
    }
    size_t count = 0;
    {
      T2FlatFile::T2MemoryHandle mem = t2_.get_memory_handle();
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
                     uint64_t val_u64 = 0;
                     std::memcpy(&val_u64, &payload, size);

                     std::array<std::byte, kStoreKeyBytes> stack_key;
                     size_t len = kStoreKeyBytes;
                     while (len > 0 && index_key[len - 1] == std::byte{0}) {
                       --len;
                     }
                     std::memcpy(stack_key.data(), index_key.data(), len);
                     std::span<const std::byte> key_view(stack_key.data(), len);
                     if (!key_in_range(key_view, lower_bound, upper_bound)) {
                       return;
                     }
                     callback(key_view, val_u64);
                     ++count;
                     return;
                   }
                 }

                 // T2 record branch (direct callback under SeqLock loop)
                 const T2RecordView record = t2_.at(payload & kOffsetMask, mem);
                 read_t2_record_seqlock(record, [&]() -> bool {
                   if (!key_in_range(record.key, lower_bound, upper_bound)) {
                     return false;
                   }
                   callback(record.key, decode_u64(record.value));
                   return true;
                 });
                 ++count;
               });
    }
    return count;
  }

 private:
  auto t2_path() const -> std::filesystem::path { return t2_.path(); }

  // Inline optimization is restricted to keys with size <= 16 bytes.
  // Since T1 only stores a 16-byte prefix (StoreKey), we must preserve the full key in T2
  // to resolve conflicts. For keys <= 16 bytes, the prefix is the entire key, so we can
  // safely bypass T2 writes without losing conflict resolution capability.
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
    const uint64_t aligned_len = align_up(raw_len, kDefaultAlignmentBytes);
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

  static auto decode_u64(std::span<const std::byte> bytes) noexcept -> uint64_t {
    uint64_t value = 0;
    std::memcpy(&value, bytes.data(), std::min<size_t>(bytes.size(), kDefaultAlignmentBytes));
    return value;
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

      reorg_running_.store(true, std::memory_order_release);
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

  // Stripe-local delete pressure is only an approximation, not a global tombstone
  // count. We intentionally keep this heuristic lightweight and read it outside the
  // critical section so Delete remains short and contention-friendly.
  // This is still stripe-local and approximate; it does not model a global tombstone
  // ratio across the whole T1 index.
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

  T1IndexT t1_;
  vmemkv::T2FlatFile t2_;
  std::atomic<uint64_t> reorg_t1_count_{0};
  std::atomic<uint64_t> reorg_t2_count_{0};
  std::atomic<uint64_t> hard_stall_count_{0};
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

}  // namespace vmemkv
