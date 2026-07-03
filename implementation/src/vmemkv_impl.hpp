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
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <vector>
#include <vmemkv/config.hpp>

#include "core/reorganize_coordinator.hpp"
#include "t1_index/t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"

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
// * get(key)    : Look up Payload in T1. If inline, decode. If offset, resolve T2.
// * insert(key) : Write value to T2 -> Get Offset -> Insert (Key, Offset) into T1.
// * reorganize(): Rebuild T2 to a temp file containing only live records,
//                 remap T2, and republish T1 sorted_region (Bypassing concurrent runs).
//
template <typename ConfigT = vmemkv::Config<>>
class VMemKVImpl {
 public:
  static constexpr bool kIsEnabled = true;

  static auto name() -> std::string {
    std::vector<std::string> parts;
    if constexpr (ConfigT::UseAppendMap) {
      parts.emplace_back("AppendMap");
    }
    if constexpr (ConfigT::UseBloomFilter) {
      parts.emplace_back("Bloom");
    }
    if constexpr (ConfigT::UseSimdScan) {
      parts.emplace_back("Simd");
    }
    if constexpr (ConfigT::UseMemoryHints) {
      parts.emplace_back("Hints");
    }
    if constexpr (ConfigT::UseT1InlineValue) {
      parts.emplace_back("T1InlineValue");
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
  VMemKVImpl(const std::filesystem::path &t2_path, uint64_t t2_bytes_capacity) : t2_(t2_path, t2_bytes_capacity) {}

  ~VMemKVImpl() noexcept = default;

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
  void reorganize() {
    std::lock_guard<std::mutex> reorg_lock(reorganize_mutex_);

    std::filesystem::path temp_path_t2 = "t2_flat.tmp";

    std::error_code error_code;
    std::filesystem::remove(temp_path_t2, error_code);

    const int temp_fd = ::open(temp_path_t2.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (temp_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open temp file");
    }

    uint64_t next_bytes_used = 0;
    uint64_t new_capacity = t2_.bytes_capacity();

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

        std::shared_ptr<const vmemkv::T2Memory> mem = t2_.get_memory();
        T2RecordView record = t2_.at(old_payload, mem);
        return write_record_to_temp_fd(fd_guard.fd, record.key, record.value, next_bytes_used);
      });

      ::close(fd_guard.fd);
      fd_guard.fd = -1;

      new_capacity = t2_.bytes_capacity();
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

      std::shared_ptr<vmemkv::T2Memory> new_mem = std::make_shared<vmemkv::T2Memory>(
          static_cast<std::byte *>(
              ::mmap(nullptr, new_capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE, map_fd, 0)),
          new_capacity);
      ::close(map_fd);

      if (new_mem->base == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap temp file");
      }

      t2_.swap_memory(new_mem, next_bytes_used);

      std::filesystem::rename(temp_path_t2, t2_path(), error_code);
      if (error_code) {
        throw std::system_error(error_code, "rename temp file");
      }
    } catch (...) {
      if (fd_guard.fd >= 0) {
        ::close(fd_guard.fd);
        fd_guard.fd = -1;
      }
      std::error_code remove_ec;
      std::filesystem::remove(temp_path_t2, remove_ec);
      throw;
    }
  }

  // Accessors for T1 (Index) and T2 (Flat File) layers (mainly for testing).
  auto t1() noexcept -> T1IndexT & { return t1_; }
  auto t1() const noexcept -> const T1IndexT & { return t1_; }
  auto t2() noexcept -> vmemkv::T2FlatFile & { return t2_; }
  auto t2() const noexcept -> const vmemkv::T2FlatFile & { return t2_; }

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

      const uint64_t offset = t2_.append(full_key, value);
      const auto put_result = t1_.put(full_key, offset, false, 0);
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

    return write_entry_lockfree(full_key, value);
  }

  // Retrieves a value associated with the key.
  // - Thread-safety: Lock-free (concurrently readable during index reorganization).
  // - Guarantees: Resolves inline value directly, otherwise reads T2 record at the T1 offset.
  auto get_impl(std::span<const std::byte> full_key) const -> uint64_t {
    if constexpr (ConfigT::UseT1InlineValue) {
      const auto res = t1_.get_with_hash(full_key);
      if (res.payload_bits != vmemkv::STORE_NOT_FOUND && t1_detail::is_inline(res.raw_hash)) {
        size_t size = t1_detail::decode_size(res.raw_hash);
        uint64_t val_u64 = 0;
        std::memcpy(&val_u64, &res.payload_bits, size);
        return val_u64;
      }
    }

    std::shared_ptr<const vmemkv::T2Memory> mem = t2_.get_memory();
    const std::optional<typename vmemkv::T2FlatFile::LookupResult> found = lookup_record(full_key, mem);
    return found.has_value() ? decode_u64(found->record.value) : vmemkv::STORE_NOT_FOUND;
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
      std::shared_ptr<const vmemkv::T2Memory> mem = t2_.get_memory();
      const std::optional<typename vmemkv::T2FlatFile::LookupResult> found = lookup_record(full_key, mem);
      if (found.has_value() && value.size() <= found->record.header->alloc_len) {
        return t2_.update_value_at(found->offset, value);
      }
    }

    maybe_reorganize_if_needed();

    return write_entry_lockfree(full_key, value);
  }

  // Logically removes a key from the store.
  // - Guarantees: Marks the key offset as STORE_NOT_FOUND in T1 (physical space reclamation is deferred to reorganize).
  // - Thread-safety: Thread-safe (guarded by key hash locks).
  auto remove_impl(std::span<const std::byte> full_key) -> bool {
    std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

    uint64_t payload = t1_.get(full_key);
    if (payload == vmemkv::STORE_NOT_FOUND) {
      return false;
    }

    return t1_.put(full_key, vmemkv::STORE_NOT_FOUND) == T1IndexT::PutResult::Applied;
  }

  // Retrieves the raw variable-length byte value associated with a key.
  // - Thread-safety: Lock-free (concurrently readable).
  // - Contract: Resolves inline value directly, otherwise fetches raw T2 record.
  auto get_bytes_impl(std::span<const std::byte> full_key) const -> std::optional<std::vector<std::byte>> {
    if constexpr (ConfigT::UseT1InlineValue) {
      const auto res = t1_.get_with_hash(full_key);
      if (res.payload_bits != vmemkv::STORE_NOT_FOUND && t1_detail::is_inline(res.raw_hash)) {
        size_t size = t1_detail::decode_size(res.raw_hash);
        std::vector<std::byte> val_bytes(size);
        std::memcpy(val_bytes.data(), &res.payload_bits, size);
        return val_bytes;
      }
    }

    return lookup_record(full_key, t2_.get_memory()).transform([](const auto &found) {
      return std::vector<std::byte>(found.record.value.begin(), found.record.value.end());
    });
  }

  // Performs a range scan, returning key-value pairs decoded via the callback.
  // - Thread-safety: Thread-safe and concurrently readable.
  // - Guarantees: Invokes Callback(key, val) for each matching live entry in sorted range.
  template <typename Callback>
  auto scan_impl(std::span<const std::byte> lower_bound,
                 std::span<const std::byte> upper_bound,
                 Callback callback) const -> size_t {
    struct ScanItem {
      std::vector<std::byte> key;
      uint64_t value;
    };
    std::vector<ScanItem> items;
    {
      std::shared_ptr<const vmemkv::T2Memory> mem = t2_.get_memory();
      t1_.scan(
          lower_bound,
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

                size_t len = kStoreKeyBytes;
                while (len > 0 && index_key[len - 1] == std::byte{0}) {
                  --len;
                }
                const auto len_diff = static_cast<std::ptrdiff_t>(len);
                std::vector<std::byte> key(index_key.begin(), std::next(index_key.begin(), len_diff));
                if (!key_in_range(key, lower_bound, upper_bound)) {
                  return;
                }
                items.push_back({key, val_u64});
                return;
              }
            }

            // T2 record branch
            const T2RecordView record = t2_.at(payload, mem);
            if (!key_in_range(record.key, lower_bound, upper_bound)) {
              return;
            }
            items.push_back({std::vector<std::byte>(record.key.begin(), record.key.end()), decode_u64(record.value)});
          });
    }

    for (const auto &item : items) {
      callback(item.key, item.value);
    }
    return items.size();
  }

 private:
  auto t2_path() const -> std::filesystem::path { return "t2_flat.db"; }

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
    return std::ranges::lexicographical_compare(lhs, rhs);
  }

  static auto key_in_range(std::span<const std::byte> key,
                           std::span<const std::byte> lower_bound,
                           std::span<const std::byte> upper_bound) noexcept -> bool {
    return !byte_span_less(key, lower_bound) && !byte_span_less(upper_bound, key);
  }

  auto lookup_record(std::span<const std::byte> full_key, const std::shared_ptr<const vmemkv::T2Memory> &mem) const
      -> std::optional<typename vmemkv::T2FlatFile::LookupResult> {
    const auto res = t1_.get_with_hash(full_key);
    const uint64_t payload = res.payload_bits;
    if (payload == vmemkv::STORE_NOT_FOUND) {
      return std::nullopt;
    }

    if constexpr (ConfigT::UseT1InlineValue) {
      if (t1_detail::is_inline(res.raw_hash)) {
        return std::nullopt;
      }
    }

    const T2RecordView record = t2_.at(payload, mem);
    if (!byte_span_equal(record.key, full_key)) {
      return std::nullopt;
    }

    return typename vmemkv::T2FlatFile::LookupResult{payload, record};
  }

  auto make_reorganize_hints() const noexcept -> ReorganizeCoordinator::ReorganizeHints {
    return ReorganizeCoordinator::ReorganizeHints{
        .append_size = t1_.append_size(),
        .append_capacity = T1IndexT::append_capacity(),
        .soft_threshold_percent = ConfigT::T1ReorganizeSoftThresholdPercent,
        .hard_threshold_percent = ConfigT::T1ReorganizeHardThresholdPercent,
    };
  }

  void maybe_reorganize_if_needed() {
    reorganize_coordinator_.maybe_reorganize_if_needed(make_reorganize_hints(), [&] { reorganize(); });
  }

  mutable std::mutex reorganize_mutex_;
  ReorganizeCoordinator reorganize_coordinator_;
  // Mutex striping based on key hash. Splitting into 256 stripes prevents lock contention
  // on concurrent writes without the overhead of dynamic allocation for individual key locks.
  // Aligned to cache line size to prevent false sharing.
  static constexpr size_t kKeyStripeCount = 256;
  mutable std::array<AlignedMutex, kKeyStripeCount> write_stripes_;

  auto key_mutex(std::span<const std::byte> key) const noexcept -> std::mutex & {
    const uint64_t hash = t1_detail::hash_full_key(key);
    return write_stripes_[hash & (kKeyStripeCount - 1)].mu;
  }

  T1IndexT t1_;
  vmemkv::T2FlatFile t2_;
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

}  // namespace vmemkv
