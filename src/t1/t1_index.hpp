// t1_index.hpp — Fixed-size two-region in-memory index with lock-free readers.
//
// Public API:
// - get(key) -> uint64_t
// - insert(key, value) -> bool
// - update(key, value) -> bool
// - remove(key) -> bool
// - scan(lo, hi, callback) -> size_t
// - reorganize() -> void
//
// Concurrency model:
// - Readers (get/scan) are lock-free and retry only if they race with reorganize.
// - Writers (insert/update/remove) take a shared reorganize lock and a per-key stripe lock.
// - reorganize() is optimistic two-phase:
//   1. build a candidate snapshot without stopping writers
//   2. take the exclusive reorganize lock only for validate + publish
// - If writes raced with phase 1, reorganize falls back to rebuilding under the exclusive lock.
// - Optional append-map publication uses a short dedicated mutex.
//
// Optimizations:
// - `UseAppendMapV`:
//   maintain a lock-free append hash index so append-region point lookup avoids linear scan
// - `UseBloomFilterV`:
//   attach a Bloom filter to sorted_region for faster negative lookups
// - `UseSimdScanV`:
//   enable the SIMD append-region scan fast path on supported CPUs
// - `UseMemoryHintsV`:
//   enable OS memory hints (`mlock`, `madvise`) defined under `optimizations/`
//
// Design reference:
// - docs/specification/low_level_design.md

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <variant>
#include <vector>

#include "../core/lock_free_hash_table.hpp"
#include "../kvs/store_types.hpp"
#include "optimizations/bloom_filter.hpp"
#include "optimizations/memory_hints.hpp"
#include "optimizations/simd_scan.hpp"

template <bool UseAppendMapV = false,
          bool UseBloomFilterV = false,
          bool UseSimdScanV = false,
          bool UseMemoryHintsV = false>
struct T1Config
{
    static constexpr bool UseAppendMap = UseAppendMapV;
    static constexpr bool UseBloomFilter = UseBloomFilterV;
    static constexpr bool UseSimdScan = UseSimdScanV;
    static constexpr bool UseMemoryHints = UseMemoryHintsV;
};

template <typename Config = T1Config<>>
class BasicT1Index
{
public:
    using Key = StoreKey;
    static constexpr size_t APPEND_CAP = 1u << 21; // 2,097,152 entries

    BasicT1Index()
        : sorted_region_(std::make_shared<SortedRegion>()),
          instance_id_(next_instance_id())
    {
        if constexpr (Config::UseAppendMap)
        {
            append_index_.clear();
        }
        apply_memory_hints_to_append();
    }

    BasicT1Index(const BasicT1Index &) = delete;
    BasicT1Index &operator=(const BasicT1Index &) = delete;

    uint64_t get(Key key) const
    {
        const uint64_t hash = fnv(key);
        while (true)
        {
            // Readers are lock-free: they retry only if reorganize raced with the read.
            const uint64_t seq = begin_reorg_read();
            const auto sorted = cached_sorted_snapshot(seq);
            const ResolvedSlot resolved = resolve_slot(sorted, key, hash);

            if (resolved.append != nullptr)
            {
                const uint64_t value = resolved.append->value.load(std::memory_order_acquire);
                if (end_reorg_read(seq))
                    return value;
                continue;
            }

            if (resolved.sorted != nullptr)
            {
                const uint64_t value = resolved.sorted->value.load(std::memory_order_acquire);
                if (end_reorg_read(seq))
                    return value;
                continue;
            }

            if (end_reorg_read(seq))
                return STORE_NOT_FOUND;
        }
    }

    bool insert(Key key, uint64_t value)
    {
        const uint64_t hash = fnv(key);
        return with_key_write_lock(key, hash, [&](const ResolvedSlot &target)
                                   {
            if (target.found())
            {
                if (is_live(target.load()))
                    return false;
                target.store(value);
                return true;
            }

            const size_t index = append_region_.reserve();
            if (index == APPEND_CAP)
                return false;
            append_region_.publish(index, key, hash, value);
            if constexpr (Config::UseAppendMap)
                publish_append_index(key, index);
            mark_stripe_modified(hash);
            return true; });
    }

    bool update(Key key, uint64_t value)
    {
        const uint64_t hash = fnv(key);
        return with_key_write_lock(key, hash, [&](const ResolvedSlot &target)
                                   {
            if (!target.found() || !is_live(target.load()))
                return false;
            target.store(value);
            mark_stripe_modified(hash);
            return true; });
    }

    bool remove(Key key)
    {
        const uint64_t hash = fnv(key);
        return with_key_write_lock(key, hash, [&](const ResolvedSlot &target)
                                   {
            if (!target.found() || !is_live(target.load()))
                return false;
            target.store(STORE_NOT_FOUND);
            mark_stripe_modified(hash);
            return true; });
    }

    template <typename Callback>
    size_t scan(Key lo, Key hi, Callback cb) const
    {
        while (true)
        {
            const uint64_t seq = begin_reorg_read();
            const auto sorted = cached_sorted_snapshot(seq);

            size_t count = append_region_.template scan<Config::UseSimdScan>(lo, hi, cb);

            const size_t begin = lower_bound_sorted(sorted, lo);
            for (size_t i = begin; i < sorted->size; ++i)
            {
                const SortedSlot &slot = sorted->slots[i];
                if (hi < slot.key)
                    break;
                const uint64_t value = slot.value.load(std::memory_order_acquire);
                if (!is_live(value))
                    continue;
                cb(slot.key, value);
                ++count;
            }

            if (end_reorg_read(seq))
                return count;
        }
    }

    void reorganize()
    {
        // Phase 1: build a candidate snapshot without stopping writers.
        auto sorted_snapshot =
            std::atomic_load_explicit(&sorted_region_, std::memory_order_acquire);
        const size_t append_snapshot_n = append_region_.size();
        const StripeVersions version_snapshot = capture_stripe_versions();

        auto merged = collect_live_entries(sorted_snapshot, append_snapshot_n);
        maybe_set_sequential_hints(append_snapshot_n);

        // Phase 2: briefly stop writes, validate the candidate, and publish.
        std::unique_lock reorg_lock(reorg_mu_);
        reorg_seq_.fetch_add(1, std::memory_order_release); // enter odd

        const auto current_sorted =
            std::atomic_load_explicit(&sorted_region_, std::memory_order_acquire);
        const size_t current_append_n = append_region_.size();

        if (!snapshot_is_current(sorted_snapshot, version_snapshot))
        {
            merged = collect_live_entries(current_sorted, current_append_n);
            maybe_set_sequential_hints(current_append_n);
        }

        sort_and_dedup_entries(merged);

        std::shared_ptr<const SortedRegion> next_sorted =
            std::make_shared<SortedRegion>(merged);
        apply_memory_hints_to_sorted(*next_sorted);

        std::atomic_store_explicit(&sorted_region_, std::move(next_sorted),
                                   std::memory_order_release);
        reset_append_after_reorganize(current_append_n);

        reorg_seq_.fetch_add(1, std::memory_order_release); // leave even
    }

private:
    static constexpr size_t kKeyStripeCount = 256;
    using StripeVersions = std::array<uint64_t, kKeyStripeCount>;

    // Temporary value type used only while rebuilding sorted_region during reorganize.
    struct EntrySnapshot
    {
        Key key{};
        uint64_t value{STORE_NOT_FOUND};
        uint64_t hash{0};
    };

    // Sorted entries are immutable except for the value field, which supports in-place updates.
    struct SortedSlot
    {
        Key key{};
        uint64_t hash{0};
        mutable std::atomic<uint64_t> value{STORE_NOT_FOUND};
    };

    // Append entries are published in two phases:
    // 1. fill key/hash/value
    // 2. set published=true
    // Readers ignore unpublished slots.
    struct AppendSlot
    {
        Key key{};
        uint64_t hash{0};
        std::atomic<uint64_t> value{STORE_NOT_FOUND};
        std::atomic<bool> published{false};
    };

    // Immutable snapshot of the sorted half. Readers pin this via shared_ptr while scanning.
    struct SortedRegion
    {
        size_t size = 0;
        std::unique_ptr<SortedSlot[]> slots;
        [[no_unique_address]] std::conditional_t<Config::UseBloomFilter,
                                                 t1_detail::BloomFilter,
                                                 std::monostate>
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
                slots[i].value.store(entries[i].value, std::memory_order_relaxed);
                if constexpr (Config::UseBloomFilter)
                    bloom.add(entries[i].hash);
            }
        }
    };

    using AppendIndex = LockFreeHashTable<Key, AppendSlot, APPEND_CAP>;

    struct TlsCache
    {
        uint64_t instance_id = 0;
        uint64_t seq = std::numeric_limits<uint64_t>::max();
        std::shared_ptr<const SortedRegion> sorted;
    };

    // Result of resolving a key on the write path.
    // The current mutable entry may live either in append_region or in sorted_region.
    // This wrapper lets insert/update/remove share the same control flow.
    struct ResolvedSlot
    {
        AppendSlot *append = nullptr;
        const SortedSlot *sorted = nullptr;

        bool found() const noexcept
        {
            return append != nullptr || sorted != nullptr;
        }

        uint64_t load() const noexcept
        {
            if (append != nullptr)
                return append->value.load(std::memory_order_acquire);
            return sorted->value.load(std::memory_order_acquire);
        }

        void store(uint64_t value) const noexcept
        {
            if (append != nullptr)
                append->value.store(value, std::memory_order_release);
            else
                sorted->value.store(value, std::memory_order_release);
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
            // Reserve one slot by advancing the append tail atomically.
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

        void publish(size_t index, Key key, uint64_t hash, uint64_t value) noexcept
        {
            // Publish order matters: readers may observe tail_ before this slot is visible.
            slots_[index].key = key;
            slots_[index].hash = hash;
            slots_[index].value.store(value, std::memory_order_relaxed);
            slots_[index].published.store(true, std::memory_order_release);
        }

        void clear(size_t count) noexcept
        {
            for (size_t i = 0; i < count; ++i)
                slots_[i].published.store(false, std::memory_order_release);
            tail_.store(0, std::memory_order_release);
        }

        template <typename Index>
        AppendSlot *find_with_index(const Index &index, Key key) noexcept
        {
            return const_cast<AppendSlot *>(
                static_cast<const AppendRegion *>(this)->find_with_index(index, key));
        }

        template <typename Index>
        const AppendSlot *find_with_index(const Index &index,
                                          Key key) const noexcept
        {
            const auto slot_plus_one =
                index.find_slot_index(key,
                                      [&](size_t i) -> const AppendSlot &
                                      { return slots_[i]; });
            if (slot_plus_one == Index::kNotFound)
                return nullptr;
            const AppendSlot &slot = slots_[static_cast<size_t>(slot_plus_one - 1)];
            if (!slot.published.load(std::memory_order_acquire))
                return nullptr;
            return slot.key == key ? &slot : nullptr;
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
                const uint64_t value = slot.value.load(std::memory_order_acquire);
                if (!is_live(value))
                    continue;
                out.push_back(EntrySnapshot{slot.key, value, slot.hash});
            }
        }

    private:
        std::unique_ptr<AppendSlot[]> slots_;
        std::atomic<size_t> tail_{0};
    };

    static bool is_live(uint64_t value) noexcept
    {
        return value != STORE_NOT_FOUND;
    }

    static uint64_t fnv(const Key &key) noexcept
    {
        constexpr uint64_t basis = 14695981039346656037ULL;
        constexpr uint64_t prime = 1099511628211ULL;
        uint64_t hash = basis;
        const auto *bytes = reinterpret_cast<const uint8_t *>(key.data());
        for (int i = 0; i < 16; ++i)
        {
            hash ^= bytes[i];
            hash *= prime;
        }
        return hash;
    }

    static uint64_t next_instance_id() noexcept
    {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t begin_reorg_read() const noexcept
    {
        uint64_t seq;
        do
        {
            // Odd means reorganize is publishing a new generation right now.
            seq = reorg_seq_.load(std::memory_order_acquire);
        } while (seq & 1u);
        return seq;
    }

    bool end_reorg_read(uint64_t start_seq) const noexcept
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        return reorg_seq_.load(std::memory_order_acquire) == start_seq;
    }

    std::shared_ptr<const SortedRegion> cached_sorted_snapshot(uint64_t seq) const
    {
        thread_local TlsCache cache;
        if (cache.instance_id != instance_id_ || cache.seq != seq)
        {
            // Reuse the same shared_ptr across repeated reads in the same generation.
            cache.sorted =
                std::atomic_load_explicit(&sorted_region_, std::memory_order_acquire);
            cache.instance_id = instance_id_;
            cache.seq = seq;
        }
        return cache.sorted;
    }

    template <typename Fn>
    auto with_key_write_lock(Key key, uint64_t hash, Fn &&fn)
    {
        // Shared reorg lock blocks only against reorganize.
        // Per-key stripe lock serializes conflicting writes on the same key bucket.
        std::shared_lock reorg_lock(reorg_mu_);
        std::lock_guard key_lock(key_mutex(hash));
        auto sorted = std::atomic_load_explicit(&sorted_region_, std::memory_order_acquire);
        return fn(resolve_slot(sorted, key, hash));
    }

    template <typename SortedPtr>
    ResolvedSlot resolve_slot(const SortedPtr &sorted, Key key, uint64_t hash) const
    {
        if (const AppendSlot *slot = find_append_slot_const(key, hash))
            return ResolvedSlot{const_cast<AppendSlot *>(slot), nullptr};

        if (const SortedSlot *slot = find_sorted(sorted, key, hash))
            return ResolvedSlot{nullptr, slot};

        return ResolvedSlot{};
    }

    const AppendSlot *find_append_slot_const(Key key, uint64_t hash) const
    {
        if constexpr (Config::UseAppendMap)
        {
            return append_region_.find_with_index(append_index_, key);
        }
        return append_region_.find_linear(key, hash);
    }

    static const SortedSlot *find_sorted(const std::shared_ptr<const SortedRegion> &sorted,
                                         Key key,
                                         uint64_t hash)
    {
        if constexpr (Config::UseBloomFilter)
        {
            if (!sorted->bloom.maybe_contains(hash))
                return nullptr;
        }

        const size_t pos = lower_bound_sorted(sorted, key);
        if (pos >= sorted->size)
            return nullptr;
        const SortedSlot &slot = sorted->slots[pos];
        if (slot.key != key || slot.hash != hash)
            return nullptr;
        return &slot;
    }

    static size_t lower_bound_sorted(const std::shared_ptr<const SortedRegion> &sorted, Key key)
    {
        size_t lo = 0;
        size_t hi = sorted->size;
        while (lo < hi)
        {
            const size_t mid = lo + (hi - lo) / 2;
            if (sorted->slots[mid].key < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    std::vector<EntrySnapshot> collect_live_entries(
        const std::shared_ptr<const SortedRegion> &old_sorted,
        size_t append_n) const
    {
        // Reorganize copies only currently reachable live entries.
        std::vector<EntrySnapshot> merged;
        merged.reserve(old_sorted->size + append_n);

        for (size_t i = 0; i < old_sorted->size; ++i)
        {
            const SortedSlot &slot = old_sorted->slots[i];
            const uint64_t value = slot.value.load(std::memory_order_acquire);
            if (!is_live(value))
                continue;
            merged.push_back(EntrySnapshot{slot.key, value, slot.hash});
        }

        append_region_.collect_live_entries(merged, append_n);
        return merged;
    }

    static void sort_and_dedup_entries(std::vector<EntrySnapshot> &entries)
    {
        // Keep only the latest version for each key after merge.
        std::stable_sort(
            entries.begin(), entries.end(),
            [](const EntrySnapshot &lhs, const EntrySnapshot &rhs)
            { return lhs.key < rhs.key; });

        size_t out = 0;
        for (size_t i = 0; i < entries.size();)
        {
            size_t j = i + 1;
            while (j < entries.size() && entries[j].key == entries[i].key)
                ++j;
            entries[out++] = std::move(entries[j - 1]);
            i = j;
        }
        entries.resize(out);
    }

    void reset_append_after_reorganize(size_t append_n)
    {
        append_region_.clear(append_n);
        if constexpr (Config::UseAppendMap)
        {
            append_index_.clear();
        }
    }

    void publish_append_index(Key key, size_t index)
    {
        if constexpr (Config::UseAppendMap)
        {
            // The append slot is already fully published before its index becomes visible.
            const auto slot_plus_one =
                static_cast<typename AppendIndex::slot_index_type>(index + 1);
            append_index_.publish_slot(key, slot_plus_one,
                                       [&](size_t i) -> const AppendSlot &
                                       { return append_region_.data()[i]; });
        }
    }

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
            auto sorted = std::atomic_load_explicit(&sorted_region_, std::memory_order_acquire);
            t1_detail::set_sequential_hint(sorted->slots.get(),
                                           sorted->size * sizeof(SortedSlot));
            t1_detail::set_sequential_hint(append_region_.data(), append_region_.bytes_for(append_n));
        }
    }

    StripeVersions capture_stripe_versions() const
    {
        StripeVersions versions{};
        for (size_t i = 0; i < kKeyStripeCount; ++i)
            versions[i] = stripe_versions_[i].load(std::memory_order_acquire);
        return versions;
    }

    bool snapshot_is_current(const std::shared_ptr<const SortedRegion> &sorted_snapshot,
                             const StripeVersions &version_snapshot) const
    {
        if (std::atomic_load_explicit(&sorted_region_, std::memory_order_acquire) != sorted_snapshot)
            return false;

        for (size_t i = 0; i < kKeyStripeCount; ++i)
        {
            if (stripe_versions_[i].load(std::memory_order_acquire) != version_snapshot[i])
                return false;
        }
        return true;
    }

    void mark_stripe_modified(uint64_t hash) noexcept
    {
        stripe_versions_[stripe_index(hash)].fetch_add(1, std::memory_order_release);
    }

    std::mutex &key_mutex(uint64_t hash) noexcept
    {
        return key_locks_[stripe_index(hash)];
    }

    static size_t stripe_index(uint64_t hash) noexcept
    {
        return static_cast<size_t>(hash) & (kKeyStripeCount - 1);
    }

    // Concurrency layout:
    // - reorg_mu_: shared for writes, exclusive for reorganize
    // - key_locks_: striped write serialization
    // - stripe_versions_: optimistic validation for phase-1 reorganize snapshots
    // - reorg_seq_: lock-free reader retry coordination
    // - append_index_: lock-free append-region point-lookup index
    mutable std::shared_mutex reorg_mu_;
    std::array<std::mutex, kKeyStripeCount> key_locks_{};
    std::array<std::atomic<uint64_t>, kKeyStripeCount> stripe_versions_{};
    mutable std::atomic<uint64_t> reorg_seq_{0};
    mutable std::shared_ptr<const SortedRegion> sorted_region_;
    AppendRegion append_region_;
    [[no_unique_address]] std::conditional_t<Config::UseAppendMap,
                                             AppendIndex,
                                             std::monostate>
        append_index_{};
    const uint64_t instance_id_;
};

using T1Index = BasicT1Index<>;
