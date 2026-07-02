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

inline constexpr std::size_t kStoreKeyBytes = 16;

// Tier 1's internal 16-byte ordered key prefix.
using StoreKey = std::array<std::byte, kStoreKeyBytes>;

namespace t1_detail {
inline constexpr std::size_t kInlineValueByteCount = 8;
inline constexpr std::size_t kPrefixBytes = kStoreKeyBytes;
inline constexpr uint64_t kInlineFlagMask = 1ULL << 63;
inline constexpr uint64_t kMetadataShiftBits = 60;
inline constexpr uint64_t kInlineSizeMask = 7ULL;
inline constexpr uint64_t kCleanHashMask = ~(15ULL << 60);

inline auto prefix_from_bytes(std::span<const std::byte> key) noexcept -> StoreKey {
  StoreKey out{};
  const std::size_t byte_count = std::min(key.size(), kPrefixBytes);
  if (byte_count != 0) {
    std::copy_n(key.data(), byte_count, out.data());
  }
  return out;
}

// Hashes the key and masks out the upper 4 bits (Bits 60-63).
// This reserves space for inline metadata (is_inline and inline_size) and ensures
// that hash comparisons elsewhere (Bloom filters, hash tables) do not need to mask
// the hash repeatedly.
inline auto hash_full_key(std::span<const std::byte> key) noexcept -> uint64_t {
  constexpr uint64_t basis = 14695981039346656037ULL;
  constexpr uint64_t prime = 1099511628211ULL;
  uint64_t hash = basis;
  for (const std::byte byte_value : key) {
    hash ^= static_cast<uint8_t>(byte_value);
    hash *= prime;
  }
  return hash & kCleanHashMask;
}

inline auto is_inline(uint64_t hash) noexcept -> bool { return (hash & kInlineFlagMask) != 0; }

inline auto decode_size(uint64_t hash) noexcept -> std::size_t {
  uint64_t size_code = (hash >> kMetadataShiftBits) & kInlineSizeMask;
  return (size_code == 0) ? kInlineValueByteCount : static_cast<std::size_t>(size_code);
}

inline auto embed_metadata(uint64_t hash, uint8_t size) noexcept -> uint64_t {
  return (hash & kCleanHashMask) | kInlineFlagMask |
         (static_cast<uint64_t>(size & kInlineSizeMask) << kMetadataShiftBits);
}

}  // namespace t1_detail

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
class T1Index {
 public:
  using Key = StoreKey;
  using Payload = uint64_t;
  // Capacity of the temporary Lock-Free Append Region (acting like an LSM-tree MemTable).
  // This is NOT the limit for the entire KVS database. When the Append Region fills up,
  // a background/explicit reorganize() merges it into the Sorted Region and resets this to empty.
  // - Power of two (1 << 21 = 2,097,152): Enables fast bitwise modulo operation (hash & (APPEND_CAP - 1)) in
  // LockFreeHashTable.
  // - Memory overhead: 32B per slot * 2M slots = 64MB (plus ~16MB hash index), totaling ~80MB, which fits well in
  // cache.
  // - Reorganize Latency: Keeping it at ~2M entries bounds reorganize (merge-sort) latency to milliseconds.
  static constexpr size_t APPEND_CAP = 1U << 21;  // 2,097,152 entries

  T1Index() : sorted_region_(std::make_shared<SortedRegion>()) {
    if constexpr (Config::UseAppendMap) {
      append_index_.clear();
    }
    apply_memory_hints_to_append();
  }

  T1Index(const T1Index &) = delete;
  auto operator=(const T1Index &) -> T1Index & = delete;

  // Returned by get_with_hash. We return both the payload and the raw 64-bit hash.
  // The raw_hash is required because its upper 4 bits contain the inline metadata
  // (is_inline and inline_size). Returning it along with the payload prevents the caller
  // from having to re-compute the hash or perform a redundant, concurrent T1 slot lookup.
  struct LookupResult {
    Payload payload_bits;
    uint64_t raw_hash;
  };

  // Retrieves the 64-bit payload and raw hash associated with a key prefix.
  auto get_with_hash(std::span<const std::byte> key) const -> LookupResult {
    const auto [prefix, hash] = prepare_key_and_hash(key);

    return read_optimistic([&]() -> LookupResult {
      const auto sorted = sorted_region_.load(std::memory_order_acquire);

      // 1. check append region
      if (const AppendSlot *slot = find_append(prefix, hash)) {
        return LookupResult{slot->payload_bits.load(std::memory_order_acquire), slot->hash};
      }

      // 2. check sorted region
      if (const SortedSlot *slot = find_sorted(*sorted, prefix, hash)) {
        return LookupResult{slot->payload_bits.load(std::memory_order_acquire), slot->hash};
      }

      return LookupResult{STORE_NOT_FOUND, 0};
    });
  }

  // Retrieves the 64-bit payload associated with a key prefix.
  // - Thread-safety: Lock-free and concurrently readable while reorganization is in progress.
  // - Guarantees: Returns the latest visible value or STORE_NOT_FOUND if not found.
  auto get(std::span<const std::byte> key) const -> Payload { return get_with_hash(key).payload_bits; }

  // Inserts or updates the 64-bit payload for a given key prefix.
  // - Thread-safety: Safe for concurrent writers (guarded internally by slot-level atomic operations or table locks).
  // - Guarantees: Writes to the append region if the key does not exist; updates the slot in-place if it does.
  // - Note on Concurrency: This write method does not require a SeqLock retry loop because
  //   concurrent writes with reorganize are reconciled by the re-check of write_version_ during Phase 2 of reorganize.
  auto put(std::span<const std::byte> key, Payload value, bool is_inline = false, uint8_t inline_size = 0) -> bool {
    const auto [prefix, hash] = prepare_key_and_hash(key);

    uint64_t stored_hash = is_inline ? t1_detail::embed_metadata(hash, inline_size) : hash;

    ResolvedSlot slot = resolve(prefix, hash);
    if (slot.found()) {
      slot.store(value);
      slot.store_hash(stored_hash);
      write_version_.fetch_add(1, std::memory_order_release);
      return true;
    }

    const size_t index = append_region_.reserve();
    if (index >= APPEND_CAP) {
      throw std::runtime_error("T1 Append Region Full");
    }

    append_region_.publish(index, prefix, stored_hash, value);
    publish_append_index(index, prefix, hash);
    write_version_.fetch_add(1, std::memory_order_release);
    return true;
  }

  // Performs a range scan over keys between lo_bytes and hi_bytes (inclusive).
  // - Thread-safety: Lock-free and concurrently readable.
  // - Guarantees: Collects a consistent snapshot of both sorted and append regions,
  //   sorts/deduplicates them, and invokes the callback for each match.
  template <typename Callback>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  auto scan(std::span<const std::byte> lo_bytes,
            std::span<const std::byte> hi_bytes,
            Callback callback) const -> size_t {
    const StoreKey lower_bound = t1_detail::prefix_from_bytes(lo_bytes);
    const StoreKey upper_bound = t1_detail::prefix_from_bytes(hi_bytes);

    return read_optimistic([&]() -> size_t {
      const auto sorted = sorted_region_.load(std::memory_order_acquire);

      // Fetch the atomic size bounds first to validate optimistic concurrency check
      const size_t append_n = append_region_.size();

      std::vector<EntrySnapshot> merged = collect_live_entries(sorted, append_n);
      sort_and_dedup_entries(merged);

      size_t match_count = 0;
      for (const auto &entry : merged) {
        if (!(entry.key < lower_bound) && !(upper_bound < entry.key)) {
          callback(prefix_to_span(entry.key), entry.payload_bits, entry.hash);
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
  //     `uint64_t (uint64_t payload, uint64_t hash)`.
  //   - Semantics: Must map an old T2 record offset (payload) to its new compiled offset.
  //     We pass both payload and raw_hash to the mapper lambda so the implementation
  //     can detect inline entries (via raw_hash) and bypass Tier 2 storage writebacks.
  // - Thread-safety: Thread-safe. Lock-free readers (get/scan) can run concurrently.
  template <typename OffsetMapper>
  void reorganize(OffsetMapper offset_mapper) {
    bool expected = false;
    if (!reorg_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
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
    reorg_seq_.fetch_add(1, std::memory_order_release);  // enter odd

    const auto current_sorted = sorted_region_.load(std::memory_order_acquire);
    const size_t current_append_n = append_region_.size();

    if (write_version_.load(std::memory_order_acquire) != version_snapshot || current_sorted != sorted_snapshot) {
      merged = collect_live_entries(current_sorted, current_append_n);
      maybe_set_sequential_hints(current_append_n);
    }

    sort_and_dedup_entries(merged);

    for (auto &entry : merged) {
      entry.payload_bits = offset_mapper(entry.payload_bits, entry.hash);
    }

    std::shared_ptr<const SortedRegion> next_sorted = std::make_shared<SortedRegion>(merged);
    apply_memory_hints_to_sorted(*next_sorted);

    sorted_region_.store(std::move(next_sorted), std::memory_order_release);
    reset_append_after_reorganize(current_append_n);

    reorg_seq_.fetch_add(1, std::memory_order_release);  // leave even
    reorg_in_progress_.store(false, std::memory_order_release);
  }

 private:
  // ─── Private Type Definitions ──────────────────────────────────────────
  struct EntrySnapshot {
    Key key{};
    Payload payload_bits{STORE_NOT_FOUND};
    uint64_t hash{0};
  };

  struct SortedSlot {
    Key key{};
    uint64_t hash{0};
    mutable std::atomic<Payload> payload_bits{STORE_NOT_FOUND};

    auto clean_hash() const noexcept -> uint64_t { return hash & t1_detail::kCleanHashMask; }
  };

  struct AppendSlot {
    Key key{};
    uint64_t hash{0};
    std::atomic<Payload> payload_bits{STORE_NOT_FOUND};
    std::atomic<bool> published{false};

    [[nodiscard]] auto clean_hash() const noexcept -> uint64_t { return hash & t1_detail::kCleanHashMask; }
  };

  struct SortedRegion {
    size_t size = 0;
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    std::unique_ptr<SortedSlot[]> slots;
    [[no_unique_address]] std::conditional_t<Config::UseBloomFilter, t1_detail::BloomFilter, EmptyOption> bloom;

    SortedRegion() = default;
    explicit SortedRegion(const std::vector<EntrySnapshot> &entries)
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        : size(entries.size()), slots(size == 0 ? nullptr : std::make_unique<SortedSlot[]>(size)) {
      if constexpr (Config::UseBloomFilter) {
        bloom.reset(entries.size());
      }
      for (size_t i = 0; i < size; ++i) {
        slots[i].key = entries[i].key;
        slots[i].hash = entries[i].hash;
        slots[i].payload_bits.store(entries[i].payload_bits, std::memory_order_relaxed);
        if constexpr (Config::UseBloomFilter) {
          bloom.add(entries[i].hash & t1_detail::kCleanHashMask);
        }
      }
    }
  };

  using AppendIndex = LockFreeHashTable<Key, AppendSlot, APPEND_CAP>;

  struct ResolvedSlot {
    AppendSlot *append = nullptr;
    const SortedSlot *sorted = nullptr;

    [[nodiscard]] auto found() const noexcept -> bool { return append != nullptr || sorted != nullptr; }

    [[nodiscard]] auto load() const noexcept -> Payload {
      if (append != nullptr) {
        return append->payload_bits.load(std::memory_order_acquire);
      }
      return sorted->payload_bits.load(std::memory_order_acquire);
    }

    void store(Payload value) const noexcept {
      if (append != nullptr) {
        append->payload_bits.store(value, std::memory_order_release);
      } else {
        sorted->payload_bits.store(value, std::memory_order_release);
      }
    }

    void store_hash(uint64_t hash) const noexcept {
      if (append != nullptr) {
        append->hash = hash;
      } else {
        const_cast<SortedSlot *>(sorted)->hash = hash;
      }
    }
  };

  class AppendRegion {
   public:
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    AppendRegion() : slots_(std::make_unique<AppendSlot[]>(APPEND_CAP)) {}

    auto data() noexcept -> AppendSlot * { return slots_.get(); }
    [[nodiscard]] auto data() const noexcept -> const AppendSlot * { return slots_.get(); }

    [[nodiscard]] auto size() const noexcept -> size_t { return tail_.load(std::memory_order_acquire); }

    [[nodiscard]] auto capacity_bytes() const noexcept -> size_t { return APPEND_CAP * sizeof(AppendSlot); }

    [[nodiscard]] auto bytes_for(size_t count) const noexcept -> size_t { return count * sizeof(AppendSlot); }

    auto reserve() noexcept -> size_t {
      size_t index = tail_.load(std::memory_order_relaxed);
      while (true) {
        if (index >= APPEND_CAP) {
          return APPEND_CAP;
        }
        if (tail_.compare_exchange_weak(index, index + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
          return index;
        }
      }
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void publish(size_t index, Key key, uint64_t hash, Payload value) noexcept {
      slots_[index].key = key;
      slots_[index].hash = hash;
      slots_[index].payload_bits.store(value, std::memory_order_relaxed);
      slots_[index].published.store(true, std::memory_order_release);
    }

    void clear(size_t count) noexcept {
      for (size_t i = 0; i < count; ++i) {
        slots_[i].published.store(false, std::memory_order_release);
      }
      tail_.store(0, std::memory_order_release);
    }

    template <typename Index>
    auto find_with_index(const Index &index, Key key, uint64_t hash) noexcept -> AppendSlot * {
      return const_cast<AppendSlot *>(static_cast<const AppendRegion *>(this)->find_with_index(index, key, hash));
    }

    template <typename Index>
    [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] auto
    find_with_index(const Index &index, Key key, uint64_t hash) const noexcept -> const AppendSlot * {
      const auto slot_plus_one =
          index.find_slot_index(key, hash, [&](size_t slot_index) -> const AppendSlot & { return slots_[slot_index]; });
      if (slot_plus_one == Index::kNotFound) {
        return nullptr;
      }
      const AppendSlot &slot = slots_[static_cast<size_t>(slot_plus_one - 1)];
      if (!slot.published.load(std::memory_order_acquire)) {
        return nullptr;
      }
      return (slot.key == key && slot.clean_hash() == hash) ? &slot : nullptr;
    }

    auto find_linear(Key key, uint64_t hash) noexcept -> AppendSlot * {
      return const_cast<AppendSlot *>(static_cast<const AppendRegion *>(this)->find_linear(key, hash));
    }

    [[nodiscard]] auto find_linear(Key key, uint64_t hash) const noexcept -> const AppendSlot * {
      const size_t slot_count = size();
      for (size_t slot_index = slot_count; slot_index > 0; --slot_index) {
        const AppendSlot &slot = slots_[slot_index - 1];
        if (!slot.published.load(std::memory_order_acquire)) {
          continue;
        }
        if (slot.clean_hash() == hash && slot.key == key) {
          return &slot;
        }
      }
      return nullptr;
    }

    template <bool UseSimdScan, typename Callback>
    auto scan(Key lower_bound, Key upper_bound, Callback &callback) const -> size_t {
      return t1_detail::scan_append<UseSimdScan>(slots_.get(), size(), lower_bound, upper_bound, callback, is_live);
    }

    void collect_live_entries(std::vector<EntrySnapshot> &out, size_t count) const {
      for (size_t i = 0; i < count; ++i) {
        const AppendSlot &slot = slots_[i];
        if (!slot.published.load(std::memory_order_acquire)) {
          continue;
        }
        const Payload value = slot.payload_bits.load(std::memory_order_acquire);
        if (!is_live(value)) {
          continue;
        }
        out.push_back(EntrySnapshot{slot.key, value, slot.hash});
      }
    }

   private:
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    std::unique_ptr<AppendSlot[]> slots_;
    std::atomic<size_t> tail_{0};
  };

  // ─── Helper Methods: Key and Lookup ────────────────────────────────────
  // Helper to generate prefix and hash representation of a key span.
  auto prepare_key_and_hash(std::span<const std::byte> key) const noexcept -> std::pair<StoreKey, uint64_t> {
    const StoreKey prefix = t1_detail::prefix_from_bytes(key);
    return {prefix, key_hash(key, prefix)};
  }

  static auto is_live(Payload value) noexcept -> bool { return value != STORE_NOT_FOUND; }

  static auto key_hash(std::span<const std::byte> key, const StoreKey &prefix) noexcept -> uint64_t {
    (void)prefix;
    return t1_detail::hash_full_key(key);
  }

  static auto prefix_to_span(const Key &key) noexcept -> std::span<const std::byte> {
    return {key.data(), t1_detail::kPrefixBytes};
  }

  auto find_append(Key key, uint64_t hash) const noexcept -> const AppendSlot * {
    if constexpr (Config::UseAppendMap) {
      return append_region_.find_with_index(append_index_, key, hash);
    } else {
      return append_region_.find_linear(key, hash);
    }
  }

  auto find_sorted(const SortedRegion &sorted, Key key, uint64_t hash) const noexcept -> const SortedSlot * {
    if constexpr (Config::UseBloomFilter) {
      if (!sorted.bloom.maybe_contains(hash)) {
        return nullptr;
      }
    }

    const size_t slot_count = sorted.size;
    if (slot_count == 0) {
      return nullptr;
    }

    const std::span<const SortedSlot> slots_span(sorted.slots.get(), sorted.size);
    auto found_it = std::ranges::lower_bound(slots_span, key, {}, &SortedSlot::key);
    const SortedSlot *end = sorted.slots.get() + sorted.size;
    const SortedSlot *found = (found_it != slots_span.end()) ? &(*found_it) : end;

    while (found != end && found->key == key) {
      if (found->clean_hash() == hash) {
        return found;
      }
      ++found;
    }
    return nullptr;
  }

  auto resolve(Key key, uint64_t hash) noexcept -> ResolvedSlot {
    if constexpr (Config::UseAppendMap) {
      if (AppendSlot *slot = append_region_.find_with_index(append_index_, key, hash)) {
        return ResolvedSlot{slot, nullptr};
      }
    } else {
      if (AppendSlot *slot = append_region_.find_linear(key, hash)) {
        return ResolvedSlot{slot, nullptr};
      }
    }

    const auto sorted = sorted_region_.load(std::memory_order_acquire);
    if (const SortedSlot *slot = find_sorted(*sorted, key, hash)) {
      return ResolvedSlot{nullptr, slot};
    }

    return ResolvedSlot{};
  }

  // ─── Helper Methods: Reorganize and Synchronization ────────────────────
  // Optimistic Concurrency Control retry helper (SeqLock pattern).
  template <typename ReaderFn>
  auto read_optimistic(ReaderFn &&reader) const {
    while (true) {
      const uint64_t start_seq = begin_reorg_read();
      auto result = reader();
      if (end_reorg_read(start_seq)) {
        return result;
      }
    }
  }

  auto begin_reorg_read() const noexcept -> uint64_t {
    uint64_t seq;
    do {
      seq = reorg_seq_.load(std::memory_order_acquire);
    } while ((seq & 1U) != 0U);
    return seq;
  }

  auto end_reorg_read(uint64_t start_seq) const noexcept -> bool {
    std::atomic_thread_fence(std::memory_order_acquire);
    return reorg_seq_.load(std::memory_order_acquire) == start_seq;
  }

  void publish_append_index(size_t index, Key key, uint64_t hash) noexcept {
    if constexpr (Config::UseAppendMap) {
      const auto slot_plus_one = static_cast<typename AppendIndex::slot_index_type>(index + 1);
      append_index_.publish_slot(key, slot_plus_one, hash, [&](size_t slot_index) -> const AppendSlot & {
        return append_region_.data()[slot_index];
      });
    }
  }

  void reset_append_after_reorganize(size_t count) {
    if constexpr (Config::UseAppendMap) {
      append_index_.clear();
    }
    append_region_.clear(count);
  }

  auto collect_live_entries(const std::shared_ptr<const SortedRegion> &sorted,
                            size_t append_n) const -> std::vector<EntrySnapshot> {
    std::vector<EntrySnapshot> merged;
    merged.reserve(sorted->size + append_n);

    for (size_t i = 0; i < sorted->size; ++i) {
      const auto &slot = sorted->slots[i];
      const Payload value = slot.payload_bits.load(std::memory_order_relaxed);
      if (is_live(value)) {
        merged.push_back(EntrySnapshot{slot.key, value, slot.hash});
      }
    }

    append_region_.collect_live_entries(merged, append_n);
    return merged;
  }

  static void sort_and_dedup_entries(std::vector<EntrySnapshot> &entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const EntrySnapshot &lhs, const EntrySnapshot &rhs) {
      if (lhs.key != rhs.key) {
        return lhs.key < rhs.key;
      }
      return lhs.hash < rhs.hash;
    });

    auto write_it = entries.begin();
    for (auto read_it = entries.begin(); read_it != entries.end(); ++read_it) {
      if (write_it != read_it && write_it->key == read_it->key && write_it->hash == read_it->hash) {
        *write_it = *read_it;
      } else {
        if (write_it != read_it) {
          *(++write_it) = *read_it;
        }
      }
    }
    if (!entries.empty()) {
      entries.erase(write_it + 1, entries.end());
    }
  }

  // ─── Helper Methods: Memory Tuning ─────────────────────────────────────
  void apply_memory_hints_to_append() {
    if constexpr (Config::UseMemoryHints) {
      t1_detail::apply_region_hints(append_region_.data(), append_region_.capacity_bytes());
    }
  }

  void apply_memory_hints_to_sorted(const SortedRegion &sorted) {
    if constexpr (Config::UseMemoryHints) {
      t1_detail::apply_region_hints(sorted.slots.get(), sorted.size * sizeof(SortedSlot));
    }
  }

  void maybe_set_sequential_hints(size_t append_n) {
    if constexpr (Config::UseMemoryHints) {
      auto sorted = sorted_region_.load(std::memory_order_acquire);
      t1_detail::set_sequential_hint(sorted->slots.get(), sorted->size * sizeof(SortedSlot));
      t1_detail::set_sequential_hint(append_region_.data(), append_region_.bytes_for(append_n));
    }
  }

  // ─── Member Variables ──────────────────────────────────────────────────
  mutable std::atomic<bool> reorg_in_progress_{false};
  mutable std::atomic<uint64_t> reorg_seq_{0};
  // Incremented on every T1 update. Used during Phase 2 of reorganize to detect
  // if any concurrent writes occurred while the sorted region was being rebuilt,
  // ensuring no updates are lost when swapping regions.
  std::atomic<uint64_t> write_version_{0};

  std::atomic<std::shared_ptr<const SortedRegion>> sorted_region_;
  AppendRegion append_region_;
  [[no_unique_address]] std::conditional_t<Config::UseAppendMap, AppendIndex, EmptyOption> append_index_{};
};

}  // namespace vmemkv
