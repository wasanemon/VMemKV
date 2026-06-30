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

#include <filesystem>
#include <memory>
#include <optional>
#include <mutex>
#include <new>
#include <span>
#include <vector>
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cassert>
#include <cstring>
#include <system_error>

#include <vmemkv/config.hpp>
#include "optimizations/inline_value.hpp"
#include "t1_index/t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"

#ifndef __cpp_lib_hardware_interference_size
namespace std {
    constexpr std::size_t hardware_destructive_interference_size = 64;
}
#endif

struct alignas(std::hardware_destructive_interference_size) AlignedMutex
{
    std::mutex mu;
};

namespace vmemkv {

// ─── VMemKVImpl Layer Coordination Overview ──────────────────────────────────
//
//                     [ Public KVStore Interface (StoreAdapter) ]
//                                         |
//                                         v
//                                 [ VMemKVImpl ]
//                                   /        \
//                                  /          \
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
class VMemKVImpl
{
public:
    static constexpr bool kIsEnabled = true;

    static std::string name()
    {
        std::vector<std::string> parts;
        if constexpr (ConfigT::UseAppendMap)   parts.push_back("AppendMap");
        if constexpr (ConfigT::UseBloomFilter)  parts.push_back("Bloom");
        if constexpr (ConfigT::UseSimdScan)     parts.push_back("Simd");
        if constexpr (ConfigT::UseMemoryHints)  parts.push_back("Hints");
        if constexpr (ConfigT::UseT1InlineValue)  parts.push_back("T1InlineValue");

        if (parts.empty())
            return "VMemKV/Baseline";

        std::string res = "VMemKV";
        for (const auto& p : parts)
            res += "/" + p;
        return res;
    }

    using T1IndexT = vmemkv::T1Index<ConfigT>;

    // Constructor. Creates a VMemKV coordination instance.
    VMemKVImpl(const std::filesystem::path &t2_path, uint64_t t2_bytes_capacity)
        : t2_(t2_path, t2_bytes_capacity)
    {
    }

    ~VMemKVImpl() noexcept = default;

    VMemKVImpl(const VMemKVImpl &) = delete;
    VMemKVImpl &operator=(const VMemKVImpl &) = delete;
    VMemKVImpl(VMemKVImpl &&) = delete;
    VMemKVImpl &operator=(VMemKVImpl &&) = delete;

    // Reclaims fragmented disk space from deleted/updated T2 records.
    // - Workflow:
    //   1. Merges active records from both sorted and append regions of T1.
    //   2. Writes live records into a temporary T2 file sequentially (garbage collection).
    //   3. Hot-swaps the memory-mapped T2 view and publishes the rebuilt T1 index.
    // - Thread-safety: Thread-safe; synchronized internally via reorganize_mutex_.
    void reorganize()
    {
        std::lock_guard<std::mutex> reorg_lock(reorganize_mutex_);

        std::filesystem::path temp_path_t2 = "t2_flat.tmp"; 

        std::error_code ec;
        std::filesystem::remove(temp_path_t2, ec);

        const int temp_fd = ::open(temp_path_t2.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (temp_fd < 0)
            throw std::system_error(errno, std::generic_category(), "open temp file");

        uint64_t next_bytes_used = 0;
        uint64_t new_capacity = t2_.bytes_capacity();

        struct FDGuard {
            int fd;
            ~FDGuard() { if (fd >= 0) ::close(fd); }
        } fd_guard{temp_fd};

        try {
            t1_.reorganize([&](uint64_t old_payload, uint64_t old_hash) -> uint64_t {
                if (old_payload == vmemkv::STORE_NOT_FOUND)
                    return vmemkv::STORE_NOT_FOUND;

                if constexpr (ConfigT::UseT1InlineValue)
                {
                    const bool is_inline = (old_hash & (1ULL << 63)) != 0;
                    if (is_inline)
                    {
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
            if (next_bytes_used > new_capacity)
            {
                new_capacity = next_bytes_used;
            }

            if (::truncate(temp_path_t2.c_str(), static_cast<off_t>(new_capacity)) != 0)
            {
                throw std::system_error(errno, std::generic_category(), "truncate temp file");
            }

            const int map_fd = ::open(temp_path_t2.c_str(), O_RDWR);
            if (map_fd < 0)
                throw std::system_error(errno, std::generic_category(), "open temp file for mmap");

            std::shared_ptr<vmemkv::T2Memory> new_mem = std::make_shared<vmemkv::T2Memory>(
                static_cast<std::byte *>(::mmap(nullptr, new_capacity, PROT_READ | PROT_WRITE, MAP_SHARED,
                                                map_fd, 0)),
                new_capacity);
            ::close(map_fd);

            if (new_mem->base == MAP_FAILED)
            {
                throw std::system_error(errno, std::generic_category(), "mmap temp file");
            }

            t2_.swap_memory(new_mem, next_bytes_used);

            std::filesystem::rename(temp_path_t2, t2_path(), ec);
            if (ec)
            {
                throw std::system_error(ec, "rename temp file");
            }
        }
        catch (...) {
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
    T1IndexT &t1() noexcept { return t1_; }
    const T1IndexT &t1() const noexcept { return t1_; }
    vmemkv::T2FlatFile &t2() noexcept { return t2_; }
    const vmemkv::T2FlatFile &t2() const noexcept { return t2_; }

    // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

    // Inserts a new key-value pair.
    // - Ordering: Writing to T2 (File) must strictly precede T1 (Index) write
    //   to prevent concurrent readers from encountering dangling offsets in T1.
    // - Thread-safety: Thread-safe (guarded by slot-level spinlocks for writes).
    bool insert_impl(std::span<const std::byte> full_key, std::span<const std::byte> value)
    {
        std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

        if (t1_.get(full_key) != vmemkv::STORE_NOT_FOUND)
            return false;

        uint8_t inline_size = 0;
        if (const auto inline_payload = try_make_inline_payload(full_key, value, inline_size))
        {
            return t1_.put(full_key, *inline_payload, true, inline_size);
        }

        const uint64_t offset = t2_.append(full_key, value);
        return t1_.put(full_key, offset, false, 0);
    }

    // Retrieves a value associated with the key.
    // - Thread-safety: Lock-free (concurrently readable during index reorganization).
    // - Guarantees: Resolves inline value directly, otherwise reads T2 record at the T1 offset.
    uint64_t get_impl(std::span<const std::byte> full_key) const
    {
        if constexpr (ConfigT::UseT1InlineValue)
        {
            const auto res = t1_.get_with_hash(full_key);
            if (res.payload_bits != vmemkv::STORE_NOT_FOUND)
            {
                const bool is_inline = (res.raw_hash & (1ULL << 63)) != 0;
                if (is_inline)
                {
                    uint64_t size_code = (res.raw_hash >> 60) & 7ULL;
                    size_t size = (size_code == 0) ? 8 : static_cast<size_t>(size_code);

                    const auto val_bytes = inline_value::unpack(res.payload_bits, size);
                    return decode_u64(val_bytes);
                }
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
    bool update_impl(std::span<const std::byte> full_key, std::span<const std::byte> value)
    {
        std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

        const auto res = t1_.get_with_hash(full_key);
        uint64_t old_payload = res.payload_bits;
        if (old_payload == vmemkv::STORE_NOT_FOUND)
            return false;

        const bool was_inline = (res.raw_hash & (1ULL << 63)) != 0;

        uint8_t inline_size = 0;
        if (const auto inline_payload = try_make_inline_payload(full_key, value, inline_size))
        {
            return t1_.put(full_key, *inline_payload, true, inline_size);
        }

        if (!was_inline)
        {
            std::shared_ptr<const vmemkv::T2Memory> mem = t2_.get_memory();
            const std::optional<typename vmemkv::T2FlatFile::LookupResult> found = lookup_record(full_key, mem);
            if (found.has_value() && value.size() <= found->record.header->alloc_len)
            {
                return t2_.update_value_at(found->offset, value);
            }
        }

        const uint64_t offset = t2_.append(full_key, value);
        return t1_.put(full_key, offset, false, 0);
    }

    // Logically removes a key from the store.
    // - Guarantees: Marks the key offset as STORE_NOT_FOUND in T1 (physical space reclamation is deferred to reorganize).
    // - Thread-safety: Thread-safe (guarded by key hash locks).
    bool remove_impl(std::span<const std::byte> full_key)
    {
        std::lock_guard<std::mutex> key_lock(key_mutex(full_key));

        uint64_t payload = t1_.get(full_key);
        if (payload == vmemkv::STORE_NOT_FOUND)
            return false;

        return t1_.put(full_key, vmemkv::STORE_NOT_FOUND);
    }

    // Retrieves the raw variable-length byte value associated with a key.
    // - Thread-safety: Lock-free (concurrently readable).
    // - Contract: Resolves inline value directly, otherwise fetches raw T2 record.
    std::optional<std::vector<std::byte>> get_bytes_impl(std::span<const std::byte> full_key) const
    {
        if constexpr (ConfigT::UseT1InlineValue)
        {
            const auto res = t1_.get_with_hash(full_key);
            if (res.payload_bits != vmemkv::STORE_NOT_FOUND)
            {
                const bool is_inline = (res.raw_hash & (1ULL << 63)) != 0;
                if (is_inline)
                {
                    uint64_t size_code = (res.raw_hash >> 60) & 7ULL;
                    size_t size = (size_code == 0) ? 8 : static_cast<size_t>(size_code);
                    return inline_value::unpack(res.payload_bits, size);
                }
            }
        }

        return lookup_record(full_key, t2_.get_memory())
            .transform([](const auto &found) {
                return std::vector<std::byte>(found.record.value.begin(),
                                              found.record.value.end());
            });
    }

    // Performs a range scan, returning key-value pairs decoded via the callback.
    // - Thread-safety: Thread-safe and concurrently readable.
    // - Guarantees: Invokes Callback(key, val) for each matching live entry in sorted range.
    template <typename Callback>
    size_t scan_impl(std::span<const std::byte> lo,
                      std::span<const std::byte> hi,
                      Callback cb) const
    {
        struct ScanItem {
            std::vector<std::byte> key;
            uint64_t value;
        };
        std::vector<ScanItem> items;
        {
            std::shared_ptr<const vmemkv::T2Memory> mem = t2_.get_memory();
            t1_.scan(lo, hi,
                     [&](std::span<const std::byte> index_key, uint64_t payload, uint64_t hash)
                     {
                          if (payload == vmemkv::STORE_NOT_FOUND)
                              return;

                          if constexpr (ConfigT::UseT1InlineValue)
                          {
                              const bool is_inline = (hash & (1ULL << 63)) != 0;
                              if (is_inline)
                              {
                                  uint64_t size_code = (hash >> 60) & 7ULL;
                                  size_t size = (size_code == 0) ? 8 : static_cast<size_t>(size_code);

                                  const auto val_bytes = inline_value::unpack(payload, size);
                                  uint64_t val = decode_u64(val_bytes);
                                  size_t len = 16;
                                  while (len > 0 && index_key[len - 1] == std::byte{0}) {
                                      --len;
                                  }
                                  std::vector<std::byte> key(index_key.begin(), index_key.begin() + len);
                                  if (!key_in_range(key, lo, hi))
                                      return;
                                  items.push_back({key, val});
                                  return;
                              }
                          }

                          // T2 record branch
                          const T2RecordView record = t2_.at(payload, mem);
                          if (!key_in_range(record.key, lo, hi))
                               return;
                          items.push_back({std::vector<std::byte>(record.key.begin(), record.key.end()), decode_u64(record.value)});
                     });
        }

        for (const auto &item : items)
        {
            cb(item.key, item.value);
        }
        return items.size();
    }

private:
    std::filesystem::path t2_path() const { return "t2_flat.db"; }

    std::optional<uint64_t> try_make_inline_payload(std::span<const std::byte> full_key, std::span<const std::byte> value, uint8_t &out_size) const noexcept
    {
        if constexpr (ConfigT::UseT1InlineValue)
        {
            if (full_key.size() <= t1_detail::kPrefixBytes)
            {
                if (value.size() >= 1 && value.size() <= 8)
                {
                    out_size = static_cast<uint8_t>(value.size());
                    return inline_value::pack(value);
                }
            }
        }
        return std::nullopt;
    }

    uint64_t write_record_to_temp_fd(int fd, std::span<const std::byte> key, std::span<const std::byte> value, uint64_t &bytes_used)
    {
        ValueRecordHeader header;
        header.key_len = static_cast<uint32_t>(key.size());
        header.value_len = static_cast<uint32_t>(value.size());
        header.alloc_len = static_cast<uint32_t>(value.size());
        header.flags = 0;
        header.version = 0;

        uint64_t offset = bytes_used;
        const uint64_t raw_len = sizeof(header) + key.size() + value.size();
        const uint64_t aligned_len = align_up(raw_len, 8);
        const uint64_t padding = aligned_len - raw_len;

        if (::write(fd, &header, sizeof(header)) != sizeof(header))
            throw std::system_error(errno, std::generic_category(), "write header");
        if (::write(fd, key.data(), key.size()) != static_cast<ssize_t>(key.size()))
            throw std::system_error(errno, std::generic_category(), "write key");
        if (!value.empty()) {
            if (::write(fd, value.data(), value.size()) != static_cast<ssize_t>(value.size()))
                throw std::system_error(errno, std::generic_category(), "write value");
        }
        if (padding > 0) {
            std::byte pad[8]{};
            if (::write(fd, pad, padding) != static_cast<ssize_t>(padding))
                throw std::system_error(errno, std::generic_category(), "write padding");
        }

        bytes_used += aligned_len;
        return offset;
    }

    static uint64_t decode_u64(std::span<const std::byte> bytes) noexcept
    {
        uint64_t value = 0;
        std::memcpy(&value, bytes.data(), std::min<size_t>(bytes.size(), 8));
        return value;
    }

    static bool byte_span_equal(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept
    {
        return std::ranges::equal(lhs, rhs);
    }

    static bool byte_span_less(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept
    {
        return std::ranges::lexicographical_compare(lhs, rhs);
    }

    static bool key_in_range(std::span<const std::byte> key,
                             std::span<const std::byte> lo,
                             std::span<const std::byte> hi) noexcept
    {
        return !byte_span_less(key, lo) && !byte_span_less(hi, key);
    }

    std::optional<typename vmemkv::T2FlatFile::LookupResult> lookup_record(
        std::span<const std::byte> full_key,
        const std::shared_ptr<const vmemkv::T2Memory> &mem) const
    {
        const auto res = t1_.get_with_hash(full_key);
        const uint64_t payload = res.payload_bits;
        if (payload == vmemkv::STORE_NOT_FOUND)
            return std::nullopt;

        if constexpr (ConfigT::UseT1InlineValue)
        {
            const bool is_inline = (res.raw_hash & (1ULL << 63)) != 0;
            if (is_inline)
                return std::nullopt;
        }

        const T2RecordView record = t2_.at(payload, mem);
        if (!byte_span_equal(record.key, full_key))
            return std::nullopt;

        return typename vmemkv::T2FlatFile::LookupResult{payload, record};
    }

    mutable std::mutex reorganize_mutex_;
    static constexpr size_t kKeyStripeCount = 256;
    mutable std::array<AlignedMutex, kKeyStripeCount> write_stripes_;

    std::mutex &key_mutex(std::span<const std::byte> key) const noexcept
    {
        const uint64_t hash = t1_detail::hash_full_key(key);
        return write_stripes_[hash & (kKeyStripeCount - 1)].mu;
    }

    T1IndexT t1_;
    vmemkv::T2FlatFile t2_;
};

using VMemKV = VMemKVImpl<vmemkv::Config<>>;

} // namespace vmemkv
