// t1_index.hpp — Fixed-size two-region in-memory index with lock-free readers.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <execution>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include <vmemkv/config.hpp>

#include "../api/utils.hpp"
#include "../core/lock_free_hash_table.hpp"
#include "../core/reference_tracker.hpp"
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
//               |   append_active_ (raw ptr)  |
//               +--------------+--------------+
//                              |
//                              v
//             +----------------------------------+
//             | AppendSlot0 | AppendSlot1 | ...  |
//             +----------------------------------+
//                    O(1) Lock-free Hash Table
//                    (Hot / Active Write Region)
//
//               +-----------------------------+
//               | append_immutable_ (raw ptr) | -- (Only during reorganize)
//               +-----------------------------+
//
// ─── Read Concurrency Protocol (EBR & Double-Buffering) ──────────────────────
// 1. Enter epoch: register current reorg_epoch_ in thread-local slot (T1ReadHandle).
// 2. Search append_active_. If found, return payload.
// 3. Search append_immutable_ (if exists/not null). If found, return payload.
// 4. Search sorted_region_. If found, return payload.
// 5. Exit epoch: clear thread-local slot registration upon handle destruction.
//
template <typename Config = vmemkv::Config<>>
class T1Index {
 public:
  using Key = StoreKey;
  using Payload = uint64_t;
  // Capacity of the temporary Lock-Free Append Region (acting like an LSM-tree MemTable).
  // This is NOT the limit for the entire KVS database. When the Append Region fills up,
  // a background/explicit reorganize() merges it into the Sorted Region and resets this to empty.
  // - Defaults to 2^21 entries (2,097,152) via Config::T1AppendCapacityEntries.
  // - Memory overhead at default: 32B per slot * 2M slots = 64MB
  //   (plus ~16MB hash index), totaling ~80MB.
  // - Reorganize latency grows roughly with this capacity and should be tuned via ablation.
  static constexpr size_t APPEND_CAP = Config::T1AppendCapacityEntries;

  // RAII handle for Epoch-based Reclamation (EBR) of AppendRegion to prevent atomic shared_ptr load overhead.
  using T1ReadHandle = typename ThreadReferenceTracker<uint64_t>::Guard;

  T1Index() {
    auto *active = new AppendRegion();
    auto *active_index = new AppendIndex();
    active_index->clear();
    if constexpr (Config::UseMemoryHints) {
      t1_detail::apply_region_hints(active->data(), active->capacity_bytes());
    }
    append_active_.store(active, std::memory_order_release);
    append_active_index_.store(active_index, std::memory_order_release);
    sorted_region_.store(new SortedRegion(), std::memory_order_release);
  }

  ~T1Index() noexcept {
    delete append_active_.load(std::memory_order_relaxed);
    delete append_active_index_.load(std::memory_order_relaxed);
    delete append_immutable_.load(std::memory_order_relaxed);
    delete append_immutable_index_.load(std::memory_order_relaxed);
    delete sorted_region_.load(std::memory_order_relaxed);
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

  enum class PutResult : uint8_t {
    Applied,
    AppendRegionFull,
  };

  // Retrieves the 64-bit payload and raw hash associated with a key prefix.
  auto get_with_hash(std::span<const std::byte> key) const -> LookupResult {
    const auto [prefix, hash] = prepare_key_and_hash(key);

    T1ReadHandle handle(active_epochs_, reorg_epoch_.load(std::memory_order_relaxed));
    const auto sorted = sorted_region_.load(std::memory_order_acquire);
    const AppendRegion *active = append_active_.load(std::memory_order_acquire);
    const AppendIndex *active_idx = append_active_index_.load(std::memory_order_acquire);

    // 1. check active append region
    if (const AppendSlot *slot = active->find_with_index(*active_idx, prefix, hash)) {
      return LookupResult{slot->payload_bits.load(std::memory_order_acquire), slot->hash};
    }

    // 2. check immutable append region if exists
    if (const AppendRegion *imm = append_immutable_.load(std::memory_order_acquire)) {
      const AppendIndex *imm_idx = append_immutable_index_.load(std::memory_order_acquire);
      if (const AppendSlot *slot = imm->find_with_index(*imm_idx, prefix, hash)) {
        return LookupResult{slot->payload_bits.load(std::memory_order_acquire), slot->hash};
      }
    }

    // 3. check sorted region
    if (const SortedSlot *slot = find_sorted(*sorted, prefix, hash)) {
      return LookupResult{slot->payload_bits.load(std::memory_order_acquire), slot->hash};
    }

    return LookupResult{STORE_NOT_FOUND, 0};
  }

  // Retrieves the 64-bit payload associated with a key prefix.
  // - Thread-safety: Lock-free and concurrently readable while reorganization is in progress.
  // - Guarantees: Returns the latest visible value or STORE_NOT_FOUND if not found.
  auto get(std::span<const std::byte> key) const -> Payload { return get_with_hash(key).payload_bits; }

  [[nodiscard]] auto append_size() const noexcept -> size_t {
    return append_active_.load(std::memory_order_acquire)->size();
  }

  [[nodiscard]] auto sorted_size() const noexcept -> size_t {
    return sorted_region_.load(std::memory_order_acquire)->size;
  }

  [[nodiscard]] auto live_bytes() const noexcept -> uint64_t {
    uint64_t total_blocks = 0;
    const auto sorted = sorted_region_.load(std::memory_order_acquire);
    for (size_t i = 0; i < sorted->size; ++i) {
      const auto &slot = sorted->slots[i];
      const Payload val = slot.payload_bits.load(std::memory_order_relaxed);
      if (is_live(val)) {
        if constexpr (Config::UseT1InlineValue) {
          if (t1_detail::is_inline(slot.hash)) {
            continue;
          }
        }
        total_blocks += (val >> 48);
      }
    }
    const AppendRegion *active = append_active_.load(std::memory_order_acquire);
    size_t active_n = active->size();
    for (size_t i = 0; i < active_n; ++i) {
      const auto &slot = active->data()[i];
      const Payload val = slot.payload_bits.load(std::memory_order_relaxed);
      if (is_live(val)) {
        if constexpr (Config::UseT1InlineValue) {
          if (t1_detail::is_inline(slot.hash)) {
            continue;
          }
        }
        total_blocks += (val >> 48);
      }
    }
    return total_blocks * 16;
  }

  [[nodiscard]] static constexpr auto append_capacity() noexcept -> size_t { return APPEND_CAP; }

  // Inserts or updates the 64-bit payload for a given key prefix.
  // - Thread-safety: Safe for concurrent writers (guarded internally by slot-level atomic operations or table locks).
  // - Guarantees: Writes to the append region if the key does not exist; updates the slot in-place if it does.
  // - Note on Concurrency: This write method does not require a SeqLock retry loop because
  //   concurrent writes with reorganize are reconciled by the re-check of write_version_ during Phase 2 of reorganize.
  auto put(std::span<const std::byte> key,
           Payload value,
           bool is_inline = false,
           uint8_t inline_size = 0) -> PutResult {
    const auto [prefix, hash] = prepare_key_and_hash(key);

    uint64_t stored_hash = is_inline ? t1_detail::embed_metadata(hash, inline_size) : hash;

    ResolvedSlot slot = resolve(prefix, hash);
    if (slot.found()) {
      slot.store(value);
      slot.store_hash(stored_hash);
      return PutResult::Applied;
    }

    AppendRegion *active = append_active_.load(std::memory_order_acquire);
    AppendIndex *active_idx = append_active_index_.load(std::memory_order_acquire);
    const size_t index = active->reserve();
    if (index >= APPEND_CAP) {
      return PutResult::AppendRegionFull;
    }

    active->publish(index, prefix, stored_hash, value);
    publish_append_index(*active_idx, *active, index, prefix, hash);
    return PutResult::Applied;
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

    T1ReadHandle handle(active_epochs_, reorg_epoch_.load(std::memory_order_relaxed));
    const auto sorted = sorted_region_.load(std::memory_order_acquire);
    const AppendRegion *active = append_active_.load(std::memory_order_acquire);
    const AppendRegion *imm = append_immutable_.load(std::memory_order_acquire);

    struct ScanCandidate {
      Key key;
      Payload payload_bits;
      uint64_t hash;
      int gen;  // 0: sorted, 1: imm, 2: active (newest)
    };

    // Use a stack buffer of 16KB to hold up to ~500 scan candidates without any heap allocations
    std::array<std::byte, 16384> stack_buf;
    std::pmr::monotonic_buffer_resource mem_res(stack_buf.data(), stack_buf.size());
    std::pmr::vector<ScanCandidate> candidates(&mem_res);

    // 1. Extract from sorted_region using binary search (O(log S))
    if (sorted && sorted->size > 0) {
      const SortedSlot *slots_begin = sorted->slots.get();
      const SortedSlot *slots_end = slots_begin + sorted->size;
      const SortedSlot *it_start =
          std::lower_bound(slots_begin, slots_end, lower_bound, [](const SortedSlot &slot, const StoreKey &bound) {
            return slot.key < bound;
          });
      for (const SortedSlot *it = it_start; it < slots_end; ++it) {
        if (upper_bound < it->key) {
          break;
        }
        Payload val = it->payload_bits.load(std::memory_order_relaxed);
        if (val != vmemkv::STORE_NOT_FOUND) {
          candidates.push_back({it->key, val, it->hash, 0});
        }
      }
    }

    // 2. Extract from append active/immutable regions (manual scan to access slot.hash directly)
    auto extract_append = [&](const AppendRegion *region, int gen) {
      if (!region) return;
      size_t count = region->size();
      for (size_t i = 0; i < count; ++i) {
        const AppendSlot &slot = region->data()[i];
        if (!slot.published.load(std::memory_order_acquire)) {
          continue;
        }
        const Payload value = slot.payload_bits.load(std::memory_order_acquire);
        if (value == vmemkv::STORE_NOT_FOUND) {
          continue;
        }
        if (!(slot.key < lower_bound) && !(upper_bound < slot.key)) {
          candidates.push_back({slot.key, value, slot.hash, gen});
        }
      }
    };

    extract_append(active, 2);  // active is generation 2 (newest)
    extract_append(imm, 1);     // imm is generation 1

    if (candidates.empty()) return 0;

    // 3. Sort candidates by Key ASC, then Generation DESC (newest first)
    std::sort(candidates.begin(), candidates.end(), [](const ScanCandidate &a, const ScanCandidate &b) {
      if (a.key != b.key) {
        return a.key < b.key;
      }
      return a.gen > b.gen;
    });

    // 4. Deduplicate (keep only the newest generation) and run callbacks
    size_t match_count = 0;
    bool first = true;
    Key prev_key{};

    for (const auto &cand : candidates) {
      if (first || cand.key != prev_key) {
        first = false;
        prev_key = cand.key;
        if (cand.payload_bits != vmemkv::STORE_NOT_FOUND) {
          callback(std::span<const std::byte>(cand.key), cand.payload_bits, cand.hash);
          ++match_count;
        }
      }
    }

    return match_count;
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

    // 1. Prepare new active buffers
    auto *next_active = new AppendRegion();
    auto *next_active_idx = new AppendIndex();
    next_active_idx->clear();
    if constexpr (Config::UseMemoryHints) {
      t1_detail::apply_region_hints(next_active->data(), next_active->capacity_bytes());
    }

    // 2. Freeze the current active region to immutable
    AppendRegion *old_active = append_active_.load(std::memory_order_acquire);
    AppendIndex *old_active_idx = append_active_index_.load(std::memory_order_acquire);

    append_immutable_.store(old_active, std::memory_order_release);
    append_immutable_index_.store(old_active_idx, std::memory_order_release);

    // Swap writing path immediately to the fresh active buffers
    append_active_.store(next_active, std::memory_order_release);
    append_active_index_.store(next_active_idx, std::memory_order_release);

    // 3. Rebuild sorted region from sorted_region and append_immutable
    const auto sorted = sorted_region_.load(std::memory_order_acquire);
    const size_t imm_n = old_active->size();

    // Extract sorted entries (already sorted)
    std::vector<EntrySnapshot> sorted_entries;
    sorted_entries.reserve(sorted->size);
    for (size_t i = 0; i < sorted->size; ++i) {
      const auto &slot = sorted->slots[i];
      const Payload value = slot.payload_bits.load(std::memory_order_relaxed);
      if (is_live(value)) {
        sorted_entries.push_back(EntrySnapshot{slot.key, value, slot.hash});
      }
    }

    // Extract append immutable entries (unsorted)
    std::vector<EntrySnapshot> imm_entries;
    imm_entries.reserve(imm_n);
    old_active->collect_live_entries(imm_entries, imm_n);

    auto comp = [](const EntrySnapshot &lhs, const EntrySnapshot &rhs) {
      if (lhs.key != rhs.key) {
        return lhs.key < rhs.key;
      }
      return lhs.hash < rhs.hash;
    };

    // Sort the smaller append immutable entries in parallel
    std::sort(std::execution::par, imm_entries.begin(), imm_entries.end(), comp);

    // Perform a linear 2-Way Merge in parallel
    std::vector<EntrySnapshot> merged;
    merged.reserve(sorted_entries.size() + imm_entries.size());
    std::merge(std::execution::par,
               sorted_entries.begin(),
               sorted_entries.end(),
               imm_entries.begin(),
               imm_entries.end(),
               std::back_inserter(merged),
               comp);

    // Deduplicate merged output (keeping the newest snapshot from imm_entries)
    // std::merge is stable: elements from imm_entries (newest) will be merged after sorted_entries (oldest).
    if (!merged.empty()) {
      auto write_it = merged.begin();
      for (auto read_it = std::next(merged.begin()); read_it != merged.end(); ++read_it) {
        if (write_it->key == read_it->key && write_it->hash == read_it->hash) {
          *write_it = *read_it;  // Overwrite older with newer
        } else {
          *(++write_it) = *read_it;
        }
      }
      merged.erase(write_it + 1, merged.end());
    }

    // Assert no duplicate entries exist in the merge output
    assert_no_duplicates(merged);

    maybe_set_sequential_hints(old_active, imm_n);

    for (auto &entry : merged) {
      entry.payload_bits = offset_mapper(entry.payload_bits, entry.hash);
    }

    const SortedRegion *old_sorted = sorted_region_.load(std::memory_order_relaxed);

    auto *next_sorted = new SortedRegion(merged);
    if constexpr (Config::UseMemoryHints) {
      t1_detail::apply_region_hints(next_sorted->slots.get(), next_sorted->size * sizeof(SortedSlot));
    }

    // Publish the new sorted region
    sorted_region_.store(next_sorted, std::memory_order_release);

    // 4. Safely retire the old buffers
    append_immutable_.store(nullptr, std::memory_order_release);
    append_immutable_index_.store(nullptr, std::memory_order_release);

    // Advance epoch and wait for all epoch readers to exit
    uint64_t target_epoch = reorg_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    active_epochs_.wait_until_epoch(target_epoch);

    delete old_active;
    delete old_active_idx;
    delete old_sorted;

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

    auto find_with_index(const AppendIndex &index, Key key, uint64_t hash) noexcept -> AppendSlot * {
      return const_cast<AppendSlot *>(static_cast<const AppendRegion *>(this)->find_with_index(index, key, hash));
    }

    [[nodiscard]] auto find_with_index(const AppendIndex &index,
                                       Key key,
                                       uint64_t hash) const noexcept -> const AppendSlot * {
      const auto slot_plus_one =
          index.find_slot_index(key, hash, [&](size_t slot_index) -> const AppendSlot & { return slots_[slot_index]; });
      if (slot_plus_one == AppendIndex::kNotFound) {
        return nullptr;
      }
      const AppendSlot &slot = slots_[static_cast<size_t>(slot_plus_one - 1)];
      if (!slot.published.load(std::memory_order_acquire)) {
        return nullptr;
      }
      return (slot.key == key && slot.clean_hash() == hash) ? &slot : nullptr;
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
    return {prefix, t1_detail::hash_full_key(key)};
  }

  static auto is_live(Payload value) noexcept -> bool { return value != STORE_NOT_FOUND; }

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
    const AppendRegion *active = append_active_.load(std::memory_order_acquire);
    const AppendIndex *active_idx = append_active_index_.load(std::memory_order_acquire);
    if (AppendSlot *slot = const_cast<AppendRegion *>(active)->find_with_index(*active_idx, key, hash)) {
      return ResolvedSlot{slot, nullptr};
    }

    // Defensive: If the key exists in append_immutable_, we must not update it in-place
    // because append_immutable_ is currently being merged/sorted by reorganize.
    // Overwriting it in-place leads to Lost Updates. Instead, we bypass and let the write
    // append a new entry to the active region.
    if (const AppendRegion *imm = append_immutable_.load(std::memory_order_acquire)) {
      const AppendIndex *imm_idx = append_immutable_index_.load(std::memory_order_acquire);
      if (const AppendSlot *slot = const_cast<AppendRegion *>(imm)->find_with_index(*imm_idx, key, hash)) {
        std::ignore = slot;
        return ResolvedSlot{};
      }
    }

    const auto sorted = sorted_region_.load(std::memory_order_acquire);
    if (const SortedSlot *slot = find_sorted(*sorted, key, hash)) {
      return ResolvedSlot{nullptr, slot};
    }

    return ResolvedSlot{};
  }

  void publish_append_index(
      AppendIndex &idx, const AppendRegion &region, size_t index, Key key, uint64_t hash) noexcept {
    const auto slot_plus_one = static_cast<typename AppendIndex::slot_index_type>(index + 1);
    idx.publish_slot(
        key, slot_plus_one, hash, [&](size_t slot_index) -> const AppendSlot & { return region.data()[slot_index]; });
  }

  auto collect_live_entries(const SortedRegion *sorted,
                            const AppendRegion *active,     // NOLINT(bugprone-easily-swappable-parameters)
                            const AppendRegion *imm) const  // NOLINT(bugprone-easily-swappable-parameters)
      -> std::vector<EntrySnapshot> {
    size_t active_n = active ? active->size() : 0;
    size_t imm_n = imm ? imm->size() : 0;
    std::vector<EntrySnapshot> merged;
    merged.reserve(sorted->size + active_n + imm_n);

    for (size_t i = 0; i < sorted->size; ++i) {
      const auto &slot = sorted->slots[i];
      const Payload value = slot.payload_bits.load(std::memory_order_relaxed);
      if (is_live(value)) {
        merged.push_back(EntrySnapshot{slot.key, value, slot.hash});
      }
    }

    if (active) {
      active->collect_live_entries(merged, active_n);
    }
    if (imm) {
      imm->collect_live_entries(merged, imm_n);
    }
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

  static void assert_no_duplicates(const std::vector<EntrySnapshot> &entries) {
    if (entries.size() < 2) {
      return;
    }
    for (size_t i = 1; i < entries.size(); ++i) {
      assert(!(entries[i - 1].key == entries[i].key && entries[i - 1].hash == entries[i].hash) &&
             "T1Index invariant violated: duplicate key prefix + hash detected in merge output");
    }
  }

  void maybe_set_sequential_hints(const AppendRegion *region, size_t append_n) {
    if constexpr (Config::UseMemoryHints) {
      auto sorted = sorted_region_.load(std::memory_order_acquire);
      t1_detail::set_sequential_hint(sorted->slots.get(), sorted->size * sizeof(SortedSlot));
      t1_detail::set_sequential_hint(region->data(), region->bytes_for(append_n));
    }
  }

  // ─── Member Variables ──────────────────────────────────────────────────
  mutable std::atomic<bool> reorg_in_progress_{false};
  std::atomic<uint64_t> reorg_epoch_{1};

  std::atomic<const SortedRegion *> sorted_region_{nullptr};
  std::atomic<AppendRegion *> append_active_{nullptr};
  std::atomic<AppendIndex *> append_active_index_{nullptr};
  std::atomic<AppendRegion *> append_immutable_{nullptr};
  std::atomic<AppendIndex *> append_immutable_index_{nullptr};

  mutable ThreadReferenceTracker<uint64_t> active_epochs_;
};

}  // namespace vmemkv
