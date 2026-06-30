// lock_free_hash_table.hpp -- Fixed-capacity open-addressing hash table.
//
// This table is designed for append-only publication:
// - writers publish a slot index after the slot itself is fully visible
// - readers perform lock-free lookup via atomic bucket loads
// - clearing the table is allowed only when external synchronization guarantees
//   that no concurrent readers/writers still depend on the previous contents
//
// The intended use in T1 is append-region indexing:
// - key -> append slot index
// - inserts are O(1) expected
// - deletes do not physically remove buckets
// - reorganize clears the whole table

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bit>

template <typename Key,
          typename Slot,
          size_t SlotCapacity>
class LockFreeHashTable
{
public:
    using slot_index_type = uint32_t;
    static constexpr slot_index_type kEmpty = 0;
    static constexpr slot_index_type kNotFound = 0;
    static constexpr size_t kBucketCount =
        std::bit_ceil(SlotCapacity * 2);

    LockFreeHashTable() = default;

    LockFreeHashTable(const LockFreeHashTable &) = delete;
    LockFreeHashTable &operator=(const LockFreeHashTable &) = delete;

    void clear() noexcept
    {
        for (Bucket &bucket : buckets_)
            bucket.slot_plus_one.store(kEmpty, std::memory_order_release);
    }

    // Caller-provided hash, e.g. hash(full_key) for T1 append indexing.
    template <typename SlotAccessor>
    slot_index_type find_slot_index(const Key &key,
                                    uint64_t hash,
                                    SlotAccessor &&slot_at) const noexcept
    {
        size_t pos = bucket_index(hash);
        for (size_t probe = 0; probe < kBucketCount; ++probe)
        {
            const slot_index_type observed =
                buckets_[pos].slot_plus_one.load(std::memory_order_acquire);
            if (observed == kEmpty)
                return kNotFound;

            const Slot &slot = slot_at(static_cast<size_t>(observed - 1));
            if (slot.published.load(std::memory_order_acquire) &&
                slot.clean_hash() == hash && slot.key == key)
            {
                return observed;
            }
            pos = next_pos(pos);
        }
        return kNotFound;
    }

    template <typename SlotAccessor>
    bool publish_slot(const Key &key,
                      uint64_t hash,
                      slot_index_type slot_plus_one,
                      SlotAccessor &&slot_at) noexcept
    {
        size_t pos = bucket_index(hash);
        for (size_t probe = 0; probe < kBucketCount; ++probe)
        {
            slot_index_type expected = kEmpty;
            if (buckets_[pos].slot_plus_one.compare_exchange_strong(
                    expected, slot_plus_one,
                    std::memory_order_release,
                    std::memory_order_relaxed))
            {
                return true;
            }
            if (expected == kEmpty)
            {
                pos = next_pos(pos);
                continue;
            }

            const Slot &slot = slot_at(static_cast<size_t>(expected - 1));
            if (slot.published.load(std::memory_order_acquire) &&
                slot.clean_hash() == hash && slot.key == key)
            {
                buckets_[pos].slot_plus_one.store(slot_plus_one,
                                                  std::memory_order_release);
                return true;
            }
            pos = next_pos(pos);
        }
        return false;
    }

private:
    struct Bucket
    {
        std::atomic<slot_index_type> slot_plus_one{kEmpty};
    };

    static size_t bucket_index(uint64_t hash) noexcept
    {
        return static_cast<size_t>(hash) & (kBucketCount - 1);
    }

    static size_t next_pos(size_t pos) noexcept
    {
        return (pos + 1) & (kBucketCount - 1);
    }

    std::array<Bucket, kBucketCount> buckets_{};
};
