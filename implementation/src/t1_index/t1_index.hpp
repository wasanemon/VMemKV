// t1_index.hpp — Fixed-size two-region in-memory index with lock-free readers.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <vmemkv/config.hpp>
#include "../api/utils.hpp"
#include "../core/lock_free_hash_table.hpp"
#include "../optimizations/bloom_filter.hpp"
#include "../optimizations/memory_hints.hpp"
#include "../optimizations/simd_scan.hpp"

// Tier 1's internal 16-byte ordered key prefix.
using StoreKey = std::array<std::byte, 16>;

namespace t1_detail
{
inline constexpr std::size_t kPrefixBytes = 16;

inline StoreKey prefix_from_bytes(std::span<const std::byte> key) noexcept
{
    StoreKey out{};
    const std::size_t n = std::min(key.size(), kPrefixBytes);
    if (n != 0)
        std::copy_n(key.data(), n, out.data());
    return out;
}

inline uint64_t hash_full_key(std::span<const std::byte> key) noexcept
{
    constexpr uint64_t basis = 14695981039346656037ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    uint64_t hash = basis;
    for (const std::byte b : key)
    {
        hash ^= static_cast<uint8_t>(b);
        hash *= prime;
    }
    return hash;
}

} // namespace t1_detail

namespace vmemkv {

static constexpr uint64_t STORE_NOT_FOUND = ~0ULL;

// ─── T1Index Structure Overview ─────────────────────────────────────────────
//
//               +-----------------------------+
//               | sorted_region_ (shared_ptr) |
//               +--------------+--------------+
//                              |
//                              v
//             +----------------------------------+
//             | SortedSlot0 | SortedSlot1 | ...  |
//             +----------------------------------+
//                    O(log N) Binary Search
//                    (Cold / Read-Only Region)
//
//               +-----------------------------+
//               |       append_region_        |
//               +--------------+--------------+
//                              |
//                              v
//             +----------------------------------+
//             | AppendSlot0 | AppendSlot1 | ...  |
//             +----------------------------------+
//                    O(1) Lock-free Hash Table
//                    (Hot / Active Write Region)
//
// ─── Read Concurrency Protocol (OCC/SeqLock) ──────────────────────────────────
// 1. Load start_seq = reorg_seq_ (acquire).
// 2. Search append_region_. If found, validate seq and return payload.
// 3. Search sorted_region_. If found, validate seq and return payload.
// 4. If seq changed during search, retry loop (concurrent reorganize in progress).
//
template <typename Config = vmemkv::Config<>>
class T1Index
{
public:
    using Key = StoreKey;
    using Payload = uint64_t;
    static constexpr size_t APPEND_CAP = 1u << 21; // 2,097,152 entries

    T1Index()
        : sorted_region_(std::make_shared<SortedRegion>())
    {
        if constexpr (Config::UseAppendMap)
        {
            append_index_.clear();
        }
        apply_memory_hints_to_append();
    }

    T1Index(const T1Index &) = delete;
    T1Index &operator=(const T1Index &) = delete;

    // Retrieves the 64-bit payload associated with a key prefix.
    // - Thread-safety: Lock-free and concurrently readable while reorganization is in progress.
    // - Guarantees: Returns the latest visible value or STORE_NOT_FOUND if not found.
    Payload get(std::span<const std::byte> key) const
    {
        const auto [prefix, hash] = prepare_key_and_hash(key);

        return read_optimistic([&]() -> Payload {
            const auto sorted = sorted_region_.load(std::memory_order_acquire);

            // 1. check append region
            if (const AppendSlot *slot = find_append(prefix, hash))
            {
                return slot->payload_bits.load(std::memory_order_acquire);
            }

            // 2. check sorted region
            if (const SortedSlot *slot = find_sorted(*sorted, prefix, hash))
            {
                return slot->payload_bits.load(std::memory_order_acquire);
            }

            return STORE_NOT_FOUND;
        });
    }

    // Inserts or updates the 64-bit payload for a given key prefix.
    // - Thread-safety: Safe for concurrent writers (guarded internally by slot-level atomic operations or table locks).
    // - Guarantees: Writes to the append region if the key does not exist; updates the slot in-place if it does.
    // - Note on Concurrency: This write method does not require a SeqLock retry loop because
    //   concurrent writes with reorganize are reconciled by the re-check of write_version_ during Phase 2 of reorganize.
    bool put(std::span<const std::byte> key, Payload value)
    {
        const auto [prefix, hash] = prepare_key_and_hash(key);

        ResolvedSlot slot = resolve(prefix, hash);
        if (slot.found())
        {
            slot.store(value);
            write_version_.fetch_add(1, std::memory_order_release);
            return true;
        }

        const size_t index = append_region_.reserve();
        if (index >= APPEND_CAP)
            throw std::runtime_error("T1 Append Region Full");

        append_region_.publish(index, prefix, hash, value);
        publish_append_index(index, prefix, hash);
        write_version_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Performs a range scan over keys between lo_bytes and hi_bytes (inclusive).
    // - Thread-safety: Lock-free and concurrently readable.
    // - Guarantees: Collects a consistent snapshot of both sorted and append regions,
    //   sorts/deduplicates them, and invokes the callback for each match.
    template <typename Callback>
    size_t scan(std::span<const std::byte> lo_bytes,
                 std::span<const std::byte> hi_bytes,
                 Callback cb) const
    {
        const StoreKey lo = t1_detail::prefix_from_bytes(lo_bytes);
        const StoreKey hi = t1_detail::prefix_from_bytes(hi_bytes);

        return read_optimistic([&]() -> size_t {
            const auto sorted = sorted_region_.load(std::memory_order_acquire);

            // Fetch the atomic size bounds first to validate optimistic concurrency check
            const size_t append_n = append_region_.size();

            std::vector<EntrySnapshot> merged = collect_live_entries(sorted, append_n);
            sort_and_dedup_entries(merged);

            size_t match_count = 0;
            for (const auto &entry : merged)
            {
                if (!(entry.key < lo) && !(hi < entry.key))
                {
                    cb(prefix_to_span(entry.key), entry.payload_bits);
                    ++match_count;
                }
            }
            return match_count;
        });
    }

    // Reorganizes the T1 index by merging append_region_ into sorted_region_.
    //
    // - Template Contract:
    //   - OffsetMapper: Must be a Callable object satisfying the signature:
    //     `uint64_t (uint64_t payload)`.
    //   - Semantics: Must map an old T2 record offset (payload) to its new compiled offset,
    //     and return the value unchanged if it represents an inlined payload.
    // - Thread-safety: Thread-safe. Lock-free readers (get/scan) can run concurrently.
    template <typename OffsetMapper>
    void reorganize(OffsetMapper offset_mapper) noexcept
    {
        bool expected = false;
        if (!reorg_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return;
        }

        // 1. Capture snapshots of sorted_region and append_region tail
        const auto sorted_snapshot = sorted_region_.load(std::memory_order_acquire);
        const size_t append_n_snapshot = append_region_.size();
        const uint64_t version_snapshot = write_version_.load(std::memory_order_acquire);

        // Phase 1: rebuild a sorted candidate
        std::vector<EntrySnapshot> merged = collect_live_entries(sorted_snapshot, append_n_snapshot);
        maybe_set_sequential_hints(append_n_snapshot);

        // Phase 2: briefly stop writes, validate candidate, and publish.
        reorg_seq_.fetch_add(1, std::memory_order_release); // enter odd

        const auto current_sorted = sorted_region_.load(std::memory_order_acquire);
        const size_t current_append_n = append_region_.size();

        if (write_version_.load(std::memory_order_acquire) != version_snapshot ||
            current_sorted != sorted_snapshot)
        {
            merged = collect_live_entries(current_sorted, current_append_n);
            maybe_set_sequential_hints(current_append_n);
        }

        sort_and_dedup_entries(merged);

        for (auto &entry : merged)
        {
            entry.payload_bits = offset_mapper(entry.payload_bits);
        }

        std::shared_ptr<const SortedRegion> next_sorted =
            std::make_shared<SortedRegion>(merged);
        apply_memory_hints_to_sorted(*next_sorted);

        sorted_region_.store(std::move(next_sorted), std::memory_order_release);
        reset_append_after_reorganize(current_append_n);

        reorg_seq_.fetch_add(1, std::memory_order_release); // leave even
        reorg_in_progress_.store(false, std::memory_order_release);
    }

private:
    // ─── Private Type Definitions ──────────────────────────────────────────
    struct EntrySnapshot
    {
        Key key{};
        Payload payload_bits{STORE_NOT_FOUND};
        uint64_t hash{0};
    };

    struct SortedSlot
    {
        Key key{};
        uint64_t hash{0};
        mutable std::atomic<Payload> payload_bits{STORE_NOT_FOUND};
    };

    struct AppendSlot
    {
        Key key{};
        uint64_t hash{0};
        std::atomic<Payload> payload_bits{STORE_NOT_FOUND};
        std::atomic<bool> published{false};
    };

    struct SortedRegion
    {
        size_t size = 0;
        std::unique_ptr<SortedSlot[]> slots;
        [[no_unique_address]] std::conditional_t<Config::UseBloomFilter,
                                                 t1_detail::BloomFilter,
                                                 EmptyOption>
            bloom;

        SortedRegion() = default;
        explicit SortedRegion(const std::vector<EntrySnapshot> &entries)
            : size(entries.size()),
              slots(size == 0 ? nullptr : std::make_unique<SortedSlot[]>(size))
        {
            if constexpr (Config::UseBloomFilter)
                bloom.reset(entries.size());
            for (size_t i = 0; i < size; ++i)
            {
                slots[i].key = entries[i].key;
                slots[i].hash = entries[i].hash;
                slots[i].payload_bits.store(entries[i].payload_bits,
                                            std::memory_order_relaxed);
                if constexpr (Config::UseBloomFilter)
                    bloom.add(entries[i].hash);
            }
        }
    };

    using AppendIndex = LockFreeHashTable<Key, AppendSlot, APPEND_CAP>;

    struct ResolvedSlot
    {
        AppendSlot *append = nullptr;
        const SortedSlot *sorted = nullptr;

        bool found() const noexcept
        {
            return append != nullptr || sorted != nullptr;
        }

        Payload load() const noexcept
        {
            if (append != nullptr)
                return append->payload_bits.load(std::memory_order_acquire);
            return sorted->payload_bits.load(std::memory_order_acquire);
        }

        void store(Payload value) const noexcept
        {
            if (append != nullptr)
                append->payload_bits.store(value, std::memory_order_release);
            else
                sorted->payload_bits.store(value, std::memory_order_release);
        }
    };

    class AppendRegion
    {
    public:
        AppendRegion() : slots_(std::make_unique<AppendSlot[]>(APPEND_CAP)) {}

        AppendSlot *data() noexcept { return slots_.get(); }
        const AppendSlot *data() const noexcept { return slots_.get(); }

        size_t size() const noexcept
        {
            return tail_.load(std::memory_order_acquire);
        }

        size_t capacity_bytes() const noexcept
        {
            return APPEND_CAP * sizeof(AppendSlot);
        }

        size_t bytes_for(size_t count) const noexcept
        {
            return count * sizeof(AppendSlot);
        }

        size_t reserve() noexcept
        {
            size_t index = tail_.load(std::memory_order_relaxed);
            while (true)
            {
                if (index >= APPEND_CAP)
                    return APPEND_CAP;
                if (tail_.compare_exchange_weak(
                        index, index + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    return index;
                }
            }
        }

        void publish(size_t index, Key key, uint64_t hash, Payload value) noexcept
        {
            slots_[index].key = key;
            slots_[index].hash = hash;
            slots_[index].payload_bits.store(value, std::memory_order_relaxed);
            slots_[index].published.store(true, std::memory_order_release);
        }

        void clear(size_t count) noexcept
        {
            for (size_t i = 0; i < count; ++i)
                slots_[i].published.store(false, std::memory_order_release);
            tail_.store(0, std::memory_order_release);
        }

        template <typename Index>
        AppendSlot *find_with_index(const Index &index, Key key, uint64_t hash) noexcept
        {
            return const_cast<AppendSlot *>(
                static_cast<const AppendRegion *>(this)->find_with_index(index, key, hash));
        }

        template <typename Index>
        const AppendSlot *find_with_index(const Index &index, Key key, uint64_t hash) const noexcept
        {
            const auto slot_plus_one =
                index.find_slot_index(key, hash,
                                      [&](size_t i) -> const AppendSlot &
                                      { return slots_[i]; });
            if (slot_plus_one == Index::kNotFound)
                return nullptr;
            const AppendSlot &slot = slots_[static_cast<size_t>(slot_plus_one - 1)];
            if (!slot.published.load(std::memory_order_acquire))
                return nullptr;
            return (slot.key == key && slot.hash == hash) ? &slot : nullptr;
        }

        AppendSlot *find_linear(Key key, uint64_t hash) noexcept
        {
            return const_cast<AppendSlot *>(
                static_cast<const AppendRegion *>(this)->find_linear(key, hash));
        }

        const AppendSlot *find_linear(Key key, uint64_t hash) const noexcept
        {
            const size_t n = size();
            for (size_t i = n; i > 0; --i)
            {
                const AppendSlot &slot = slots_[i - 1];
                if (!slot.published.load(std::memory_order_acquire))
                    continue;
                if (slot.hash == hash && slot.key == key)
                    return &slot;
            }
            return nullptr;
        }

        template <bool UseSimdScan, typename Callback>
        size_t scan(Key lo, Key hi, Callback &cb) const
        {
            return t1_detail::scan_append<UseSimdScan>(
                slots_.get(), size(), lo, hi, cb, is_live);
        }

        void collect_live_entries(std::vector<EntrySnapshot> &out, size_t count) const
        {
            for (size_t i = 0; i < count; ++i)
            {
                const AppendSlot &slot = slots_[i];
                if (!slot.published.load(std::memory_order_acquire))
                    continue;
                const Payload value =
                    slot.payload_bits.load(std::memory_order_acquire);
                if (!is_live(value))
                    continue;
                out.push_back(EntrySnapshot{slot.key, value, slot.hash});
            }
        }

    private:
        std::unique_ptr<AppendSlot[]> slots_;
        std::atomic<size_t> tail_{0};
    };

    // ─── Helper Methods: Key and Lookup ────────────────────────────────────
    // Helper to generate prefix and hash representation of a key span.
    std::pair<StoreKey, uint64_t> prepare_key_and_hash(std::span<const std::byte> key) const noexcept
    {
        const StoreKey prefix = t1_detail::prefix_from_bytes(key);
        return {prefix, key_hash(key, prefix)};
    }

    static bool is_live(Payload value) noexcept
    {
        return value != STORE_NOT_FOUND;
    }

    static uint64_t key_hash(std::span<const std::byte> key, const StoreKey &prefix) noexcept
    {
        (void)prefix;
        return t1_detail::hash_full_key(key);
    }

    static std::span<const std::byte> prefix_to_span(const Key &key) noexcept
    {
        return std::span<const std::byte>(key.data(), t1_detail::kPrefixBytes);
    }

    const AppendSlot *find_append(Key key, uint64_t hash) const noexcept
    {
        if constexpr (Config::UseAppendMap)
            return append_region_.find_with_index(append_index_, key, hash);
        else
            return append_region_.find_linear(key, hash);
    }

    const SortedSlot *find_sorted(const SortedRegion &sorted, Key key, uint64_t hash) const noexcept
    {
        if constexpr (Config::UseBloomFilter)
        {
            if (!sorted.bloom.maybe_contains(hash))
                return nullptr;
        }

        const size_t n = sorted.size;
        if (n == 0)
            return nullptr;

        const std::span<const SortedSlot> slots_span(sorted.slots.get(), sorted.size);
        auto found_it = std::ranges::lower_bound(slots_span, key, {}, &SortedSlot::key);
        const SortedSlot *end = sorted.slots.get() + sorted.size;
        const SortedSlot *found = (found_it != slots_span.end()) ? &(*found_it) : end;

        while (found != end && found->key == key)
        {
            if (found->hash == hash)
            {
                return found;
            }
            ++found;
        }
        return nullptr;
    }

    ResolvedSlot resolve(Key key, uint64_t hash) noexcept
    {
        if constexpr (Config::UseAppendMap)
        {
            if (AppendSlot *slot = append_region_.find_with_index(append_index_, key, hash))
                return ResolvedSlot{slot, nullptr};
        }
        else
        {
            if (AppendSlot *slot = append_region_.find_linear(key, hash))
                return ResolvedSlot{slot, nullptr};
        }

        const auto sorted = sorted_region_.load(std::memory_order_acquire);
        if (const SortedSlot *slot = find_sorted(*sorted, key, hash))
            return ResolvedSlot{nullptr, slot};

        return ResolvedSlot{};
    }

    // ─── Helper Methods: Reorganize and Synchronization ────────────────────
    // Optimistic Concurrency Control retry helper (SeqLock pattern).
    template <typename ReaderFn>
    auto read_optimistic(ReaderFn&& reader) const
    {
        while (true)
        {
            const uint64_t start_seq = begin_reorg_read();
            auto result = reader();
            if (end_reorg_read(start_seq))
            {
                return result;
            }
        }
    }

    uint64_t begin_reorg_read() const noexcept
    {
        uint64_t seq;
        do
        {
            seq = reorg_seq_.load(std::memory_order_acquire);
        } while (seq & 1u);
        return seq;
    }

    bool end_reorg_read(uint64_t start_seq) const noexcept
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        return reorg_seq_.load(std::memory_order_acquire) == start_seq;
    }

    void publish_append_index(size_t index, Key key, uint64_t hash) noexcept
    {
        if constexpr (Config::UseAppendMap)
        {
            const auto slot_plus_one =
                static_cast<typename AppendIndex::slot_index_type>(index + 1);
            append_index_.publish_slot(key, hash, slot_plus_one,
                                       [&](size_t i) -> const AppendSlot &
                                       { return append_region_.data()[i]; });
        }
    }

    void reset_append_after_reorganize(size_t count)
    {
        if constexpr (Config::UseAppendMap)
        {
            append_index_.clear();
        }
        append_region_.clear(count);
    }

    std::vector<EntrySnapshot> collect_live_entries(
        const std::shared_ptr<const SortedRegion> &sorted,
        size_t append_n) const
    {
        std::vector<EntrySnapshot> merged;
        merged.reserve(sorted->size + append_n);

        for (size_t i = 0; i < sorted->size; ++i)
        {
            const auto &slot = sorted->slots[i];
            const Payload value =
                slot.payload_bits.load(std::memory_order_relaxed);
            if (is_live(value))
                merged.push_back(EntrySnapshot{slot.key, value, slot.hash});
        }

        append_region_.collect_live_entries(merged, append_n);
        return merged;
    }

    static void sort_and_dedup_entries(std::vector<EntrySnapshot> &entries)
    {
        std::stable_sort(entries.begin(), entries.end(),
                         [](const EntrySnapshot &lhs, const EntrySnapshot &rhs)
                         {
                             if (lhs.key != rhs.key)
                                  return lhs.key < rhs.key;
                             return lhs.hash < rhs.hash;
                         });

        auto write_it = entries.begin();
        for (auto read_it = entries.begin(); read_it != entries.end(); ++read_it)
        {
            if (write_it != read_it && write_it->key == read_it->key && write_it->hash == read_it->hash)
            {
                *write_it = *read_it;
            }
            else
            {
                if (write_it != read_it)
                     *(++write_it) = *read_it;
            }
        }
        if (!entries.empty())
            entries.erase(write_it + 1, entries.end());
    }

    // ─── Helper Methods: Memory Tuning ─────────────────────────────────────
    void apply_memory_hints_to_append()
    {
        if constexpr (Config::UseMemoryHints)
            t1_detail::apply_region_hints(append_region_.data(), append_region_.capacity_bytes());
    }

    void apply_memory_hints_to_sorted(const SortedRegion &sorted)
    {
        if constexpr (Config::UseMemoryHints)
            t1_detail::apply_region_hints(sorted.slots.get(), sorted.size * sizeof(SortedSlot));
    }

    void maybe_set_sequential_hints(size_t append_n)
    {
        if constexpr (Config::UseMemoryHints)
        {
            auto sorted = sorted_region_.load(std::memory_order_acquire);
            t1_detail::set_sequential_hint(sorted->slots.get(),
                                           sorted->size * sizeof(SortedSlot));
            t1_detail::set_sequential_hint(append_region_.data(), append_region_.bytes_for(append_n));
        }
    }

    // ─── Member Variables ──────────────────────────────────────────────────
    mutable std::atomic<bool> reorg_in_progress_{false};
    mutable std::atomic<uint64_t> reorg_seq_{0};
    std::atomic<uint64_t> write_version_{0};

    std::atomic<std::shared_ptr<const SortedRegion>> sorted_region_;
    AppendRegion append_region_;
    [[no_unique_address]] std::conditional_t<Config::UseAppendMap,
                                             AppendIndex,
                                             EmptyOption>
        append_index_{};
};

} // namespace vmemkv
