// t1_index.hpp — Fixed-size two-region in-memory index with lock-free readers.
#pragma once

#include <sys/mman.h>

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
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <vmemkv/config.hpp>

#include "../api/utils.hpp"
#include "../core/lock_free_hash_table.hpp"
#include "../core/reference_tracker.hpp"
#include "../optimizations/bloom_filter.hpp"

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

// Masks out the upper 4 bits (reserved for inline metadata) so hash comparisons
// elsewhere never need to mask again.
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
//               |  sorted_region_ (raw ptr)   |
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
  // Capacity of the lock-free Append Region (an LSM-style MemTable), not the whole KVS.
  // reorganize() merges it into the Sorted Region and resets this to empty when full.
  static constexpr size_t APPEND_CAP = Config::T1AppendCapacityEntries;

  // RAII handle for Epoch-based Reclamation (EBR) of AppendRegion to prevent atomic shared_ptr load overhead.
  using T1ReadHandle = typename ThreadReferenceTracker<uint64_t>::Guard;

  T1Index() {
    auto *active_region = new AppendRegion();
    note_append_region_created();
    auto *active_index = new AppendIndex();
    append_active_.store(new AppendGeneration{active_region, active_index}, std::memory_order_release);
    sorted_region_.store(new SortedRegion(), std::memory_order_release);
  }

  ~T1Index() noexcept {
    if (AppendGeneration *active = append_active_.load(std::memory_order_relaxed); active != nullptr) {
      note_append_region_destroyed();
    }
    if (AppendGeneration *imm = append_immutable_.get(); imm != nullptr) {
      note_append_region_destroyed();
    }
    delete_generation(append_active_.load(std::memory_order_relaxed));
    delete_generation(append_immutable_.get());
    delete sorted_region_.load(std::memory_order_relaxed);
  }

  T1Index(const T1Index &) = delete;
  auto operator=(const T1Index &) -> T1Index & = delete;

  // Returned by get_with_hash(). raw_hash's upper 4 bits carry inline metadata (is_inline,
  // inline_size); returning it avoids the caller re-hashing or re-looking-up the slot.
  struct LookupResult {
    Payload payload_bits;
    uint64_t raw_hash;
  };

  enum class PutResult : uint8_t {
    Applied,
    AppendRegionFull,
  };

  // A live entry as reorganize() merges it (payload already offset-mapped by the time
  // chk_writer sees it). Exposed so a checkpoint writer can serialize sorted_region without
  // T1Index knowing about files.
  struct EntrySnapshot {
    Key key{};
    Payload payload_bits{STORE_NOT_FOUND};
    uint64_t hash{0};
  };

  // No-op default for reorganize()'s chk_writer parameter.
  struct NoOpChkWriter {
    void operator()(std::span<const EntrySnapshot> /*merged*/) const noexcept {}
  };

  // Retrieves the payload and raw hash for a key prefix.
  auto get_with_hash(std::span<const std::byte> key) const -> LookupResult {
    const auto [prefix, hash] = prepare_key_and_hash(key);
    return with_epoch_guard([&]() -> LookupResult { return lookup_by_prefix_hash(prefix, hash); });
  }

 private:
  // Lookup body for get_with_hash() -- must be called from inside with_epoch_guard().
  // load_slot_consistent() reads (hash, payload) as a consistent pair via the slot's seqlock --
  // independent loads here could feed update_impl() a torn pair and corrupt a live T2 write (see
  // SortedSlot::version's declaration).
  auto lookup_by_prefix_hash(const StoreKey &prefix, uint64_t hash) const -> LookupResult {
    const SortedRegion *sorted = sorted_region_.load(std::memory_order_acquire);
    const AppendGeneration *active_gen = append_active_.load(std::memory_order_acquire);
    const AppendRegion *active = active_gen->region;

    // 1. check active append region
    if (const AppendSlot *slot = active->find_with_index(*active_gen->index, prefix, hash)) {
      const auto [slot_hash, slot_payload] = load_slot_consistent(*slot);
      return LookupResult{slot_payload, slot_hash};
    }

    // 2. check immutable append region if exists
    if (const AppendGeneration *imm_gen = append_immutable_.get()) {
      const AppendRegion *imm = imm_gen->region;
      if (const AppendSlot *slot = imm->find_with_index(*imm_gen->index, prefix, hash)) {
        const auto [slot_hash, slot_payload] = load_slot_consistent(*slot);
        return LookupResult{slot_payload, slot_hash};
      }
    }

    // 3. check sorted region
    if (const SortedSlot *slot = find_sorted(*sorted, prefix, hash)) {
      const auto [slot_hash, slot_payload] = load_slot_consistent(*slot);
      return LookupResult{slot_payload, slot_hash};
    }

    return LookupResult{STORE_NOT_FOUND, 0};
  }

 public:
  // Retrieves the 64-bit payload associated with a key prefix.
  // - Thread-safety: Lock-free and concurrently readable while reorganization is in progress.
  // - Guarantees: Returns the latest visible value or STORE_NOT_FOUND if not found.
  auto get(std::span<const std::byte> key) const -> Payload { return get_with_hash(key).payload_bits; }

  [[nodiscard]] auto append_size() const noexcept -> size_t {
    return with_epoch_guard([&]() noexcept { return append_active_.load(std::memory_order_acquire)->region->size(); });
  }

  // Number of AppendRegion instances (active + any not-yet-EBR-reclaimed retiring generation)
  // currently resident, and the high-water mark across this instance's lifetime. Each carries a
  // fixed APPEND_CAP * sizeof(AppendSlot) mmap'd footprint that becomes almost fully resident
  // from even light occupancy (open-addressing page-scatter), independent of actual corpus size,
  // so how many pile up live at once directly bounds T1's own RSS contribution under sustained
  // reorganize() churn.
  [[nodiscard]] auto append_region_live_count() const noexcept -> int64_t {
    return append_region_live_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto append_region_peak_count() const noexcept -> int64_t {
    return append_region_peak_count_.load(std::memory_order_relaxed);
  }

  // Inserts or updates the 64-bit payload for a given key prefix.
  // - Thread-safety: Safe for concurrent writers (guarded internally by slot-level atomic operations or table locks).
  // - Guarantees: Writes to the append region if the key does not exist; updates the slot in-place if it does.
  auto put(std::span<const std::byte> key,
           Payload value,
           bool is_inline = false,
           uint8_t inline_size = 0) -> PutResult {
    return with_epoch_guard([&]() -> PutResult {
      const auto [prefix, hash] = prepare_key_and_hash(key);

      uint64_t stored_hash = is_inline ? t1_detail::embed_metadata(hash, inline_size) : hash;

      // Retries because the in-place write below can race a concurrent insert that displaces the
      // same slot (LockFreeHashTable::publish_slot()'s on_displaced). same_slot_as() (a second
      // resolve() after writing) distinguishes real displacement from an ordinary delete/reinsert;
      // on displacement this thread retires its own write and retries. Bounded by resolve()'s
      // immutable-region bypass window, so this always terminates. See regression test
      // "concurrent puts racing the same immutable-bypass window collapse to one entry".
      while (true) {
        ResolvedSlot slot = resolve(prefix, hash);
        if (slot.found()) {
          // begin_write()/end_write() bracket this store with the slot's seqlock so a reader
          // can't observe a payload from after try_store() paired with a stale hash from before
          // store_hash() (see ResolvedSlot::begin_write()).
          slot.begin_write();
          Payload current = slot.load();
          if (!slot.try_store(current, value)) {
            slot.end_write();
            continue;  // Stale snapshot (a benign concurrent update raced us) -- re-resolve.
          }
          if (resolve(prefix, hash).same_slot_as(slot)) {
            slot.store_hash(stored_hash);
            slot.end_write();
            return PutResult::Applied;
          }
          // Displaced right after our write landed -- may have resurrected an orphaned slot.
          // Best-effort retire (fine if this CAS loses to another cleanup) before retrying.
          Payload ours = value;
          std::ignore = slot.try_store(ours, STORE_NOT_FOUND);
          slot.end_write();
          continue;
        }

        AppendGeneration *active_gen = append_active_.load(std::memory_order_acquire);
        AppendRegion *active = active_gen->region;
        const size_t index = active->reserve();
        if (index >= APPEND_CAP) {
          return PutResult::AppendRegionFull;
        }

        active->publish(index, prefix, stored_hash, value);
        publish_append_index(*active_gen->index, *active, index, prefix, hash);
        return PutResult::Applied;
      }
    });
  }

  // Range scan over [lo_bytes, hi_bytes]. Lock-free; collects a consistent snapshot of both
  // regions, dedups, and invokes callback per match.
  // `Callback`: `(key, payload, hash) -> void`.
  template <typename Callback>
  auto scan(std::span<const std::byte> lo_bytes,
            std::span<const std::byte> hi_bytes,
            Callback callback) const -> size_t {
    const StoreKey lower_bound = t1_detail::prefix_from_bytes(lo_bytes);
    const StoreKey upper_bound = t1_detail::prefix_from_bytes(hi_bytes);

    return with_epoch_guard([&]() -> size_t {
      const SortedRegion *sorted = sorted_region_.load(std::memory_order_acquire);
      const AppendRegion *active = append_active_.load(std::memory_order_acquire)->region;
      const AppendGeneration *imm_gen = append_immutable_.get();
      const AppendRegion *imm = imm_gen != nullptr ? imm_gen->region : nullptr;

      // Fast path: if neither append region can contain a key in range, stream straight from the
      // sorted region via walk_sorted_region() -- no candidates buffer, no merge, no dedup pass
      // (sorted region has no duplicate keys by construction).
      auto region_disjoint = [&](const AppendRegion *region) {
        if (region == nullptr) return true;
        Key region_min{};
        Key region_max{};
        if (!region->bounds(region_min, region_max)) return true;
        return upper_bound < region_min || region_max < lower_bound;
      };
      if (region_disjoint(active) && region_disjoint(imm)) {
        size_t match_count = 0;
        walk_sorted_region(
            sorted, lower_bound, upper_bound, [&](const SortedSlot &slot, uint64_t hash, Payload payload) {
              callback(std::span<const std::byte>(slot.key), payload, hash);
              ++match_count;
            });
        return match_count;
      }

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
      walk_sorted_region(sorted, lower_bound, upper_bound, [&](const SortedSlot &slot, uint64_t hash, Payload payload) {
        candidates.push_back({slot.key, payload, hash, 0});
      });

      // 2. Extract from append active/immutable regions (manual scan to access slot.hash directly)
      auto extract_append = [&](const AppendRegion *region, int gen) {
        if (!region) return;
        size_t count = region->size();
        if (count == 0) return;
        // Skip this region's O(count) scan when the query range can't intersect its bounds (see
        // AppendRegion::update_bounds()/bounds() for why this can't miss a visible match).
        {
          Key region_min{};
          Key region_max{};
          if (region->bounds(region_min, region_max)) {
            if (upper_bound < region_min || region_max < lower_bound) {
              return;
            }
          }
        }
        for (size_t i = 0; i < count; ++i) {
          const AppendSlot &slot = region->data()[i];
          if (!slot.published.load(std::memory_order_acquire)) {
            continue;
          }
          // Paired read -- see the sorted-region loop above and SortedSlot::version's declaration.
          const auto [slot_hash, value] = load_slot_consistent(slot);
          if (value == vmemkv::STORE_NOT_FOUND) {
            continue;
          }
          if (!(slot.key < lower_bound) && !(upper_bound < slot.key)) {
            candidates.push_back({slot.key, value, slot_hash, gen});
          }
        }
      };

      const size_t sorted_count = candidates.size();

      extract_append(active, 2);  // active is generation 2 (newest)
      extract_append(imm, 1);     // imm is generation 1

      if (candidates.empty()) return 0;

      // 3. Sort candidates by Key ASC, then Generation DESC (newest first).
      // candidates[0, sorted_count) is already ascending (came from step 1's sorted-array walk),
      // so only the append-region tail needs sorting; the two runs are then merged manually
      // rather than std::inplace_merge, which can heap-allocate -- this function is stack-only
      // by design.
      auto candidate_less = [](const ScanCandidate &a, const ScanCandidate &b) {
        if (a.key != b.key) {
          return a.key < b.key;
        }
        return a.gen > b.gen;
      };
      if (candidates.size() > sorted_count) {
        std::sort(candidates.begin() + static_cast<std::ptrdiff_t>(sorted_count), candidates.end(), candidate_less);

        std::pmr::vector<ScanCandidate> merged(&mem_res);
        merged.reserve(candidates.size());
        size_t i = 0;
        size_t j = sorted_count;
        while (i < sorted_count && j < candidates.size()) {
          if (candidate_less(candidates[j], candidates[i])) {
            merged.push_back(candidates[j++]);
          } else {
            merged.push_back(candidates[i++]);
          }
        }
        while (i < sorted_count) {
          merged.push_back(candidates[i++]);
        }
        while (j < candidates.size()) {
          merged.push_back(candidates[j++]);
        }
        candidates.swap(merged);
      }

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
    });
  }

  // Reorganizes T1 by merging append_active_'s region into sorted_region_. Thread-safe; lock-free
  // readers (get/scan) can run concurrently.
  // - OffsetMapper: `void(std::span<EntrySnapshot> merged)`, called exactly once with the full,
  //   already-deduped, key-ordered live set. May rewrite any entry's `.payload_bits` in place
  //   (e.g. to relocate a T2 offset), in any order it likes, but must NOT reorder or resize
  //   `merged` itself -- chk_writer and the SortedRegion built from it right after both require
  //   key order to survive unchanged. `merged` is not published/visible to any other thread at
  //   the point this is called, so free in-place mutation of the field above is safe. Entries
  //   this mapper doesn't need to touch (inline, T1-only reorg) must be left untouched.
  // - ChkWriter: optional, called once with the finalized sorted entries right before publish,
  //   so a caller can serialize a checkpoint without T1Index knowing about files. No-op default.
  template <typename OffsetMapper, typename ChkWriter = NoOpChkWriter>
  void reorganize(OffsetMapper offset_mapper, ChkWriter chk_writer = ChkWriter{}) {
    bool expected = false;
    if (!reorg_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }

    // 1. Prepare new active buffers
    auto *next_active_region = new AppendRegion();
    note_append_region_created();
    auto *next_active_index = new AppendIndex();
    auto *next_active_gen = new AppendGeneration{next_active_region, next_active_index};

    // 2. Freeze the current active generation to immutable, publish a fresh one as active. Each
    // store is a single atomic pointer store, so a reader can never observe a region paired with
    // the wrong generation's index (see AppendGeneration's declaration).
    AppendGeneration *old_active_gen = append_active_.load(std::memory_order_acquire);
    append_immutable_.freeze(old_active_gen);
    append_active_.store(next_active_gen, std::memory_order_release);

    // Advance epoch and wait for all writers currently accessing the old append region to exit.
    // Once they do, we are guaranteed that:
    // 1. No more writers can call reserve() on old_active_gen (they will see next_active_gen).
    // 2. All slots currently reserved on old_active_gen are fully written and published.
    // This resolves all straggler writer and post-freeze reservation races.
    uint64_t freeze_epoch = reorg_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    active_epochs_.wait_until_epoch(freeze_epoch);

    // 3. Rebuild sorted region from sorted_region and append_immutable
    const auto sorted = sorted_region_.load(std::memory_order_acquire);
    // Freeze *before* the merge loop below reads a single slot: from here until the new
    // sorted_region_ publishes, `sorted`'s contents are frozen for writers -- see
    // FreezableRegion's own comment for why put()'s in-place path must bypass instead of
    // mutating this region once its values start feeding `merged`.
    sorted_write_frozen_.freeze(sorted);
    AppendRegion *old_active = old_active_gen->region;
    const size_t imm_n = old_active->size();

    // Extract append immutable entries (unsorted)
    std::vector<EntrySnapshot> imm_entries;
    imm_entries.reserve(imm_n);
    old_active->collect_live_entries(imm_entries, imm_n);

    // Ordered by *clean* hash, not raw: raw hash embeds transient inline-value metadata (see
    // t1_detail::embed_metadata) that shifts independently of key identity, which the merge's
    // dedup relies on not happening. Matches every other identity check in this file.
    auto comp = [](const EntrySnapshot &lhs, const EntrySnapshot &rhs) {
      if (lhs.key != rhs.key) {
        return lhs.key < rhs.key;
      }
      return (lhs.hash & t1_detail::kCleanHashMask) < (rhs.hash & t1_detail::kCleanHashMask);
    };

    // Sort the smaller append immutable entries in parallel
    std::sort(std::execution::par, imm_entries.begin(), imm_entries.end(), comp);

    // Merges sorted_region directly with imm_entries in one pass (rather than first extracting
    // sorted_region into its own vector) to avoid a redundant O(sorted->size) copy. Hand-written
    // merge loop, not std::merge, since one side (sorted_region) needs live-filtering interleaved
    // with the comparison.
    std::vector<EntrySnapshot> merged;
    merged.reserve(sorted->size + imm_entries.size());
    {
      size_t si = 0;
      size_t ii = 0;
      auto skip_dead = [&]() {
        while (si < sorted->size && !is_live(sorted->slots[si].payload_bits.load(std::memory_order_relaxed))) {
          ++si;
        }
      };
      skip_dead();
      // Reads (hash, payload_bits) as a consistent pair via load_slot_consistent() -- an
      // independent load of each field could pair a fresh payload with a stale hash, which
      // t1_detail::is_inline() would then misread, causing an inline value to be dereferenced as
      // a T2 offset (see SortedSlot::version's declaration).
      while (si < sorted->size && ii < imm_entries.size()) {
        const SortedSlot &s = sorted->slots[si];
        const EntrySnapshot &m = imm_entries[ii];
        const auto [s_hash, s_payload] = load_slot_consistent(s);
        const uint64_t s_clean_hash = s_hash & t1_detail::kCleanHashMask;
        const uint64_t m_clean_hash = m.hash & t1_detail::kCleanHashMask;
        if (s.key == m.key && s_clean_hash == m_clean_hash) {
          // Same logical entry on both sides (updated/reinserted since last reorg) -- imm_entries
          // (newer) wins. Compared by *clean* hash, not raw, since an inline<->non-inline
          // transition changes the raw hash (t1_detail::embed_metadata) without changing key
          // identity; matching on raw hash would leave a stale sorted-region copy unrecognized
          // as superseded, surviving as a permanent duplicate. See regression test
          // "inline-to-non-inline transition racing an in-flight reorganize...".
          merged.push_back(m);
          ++si;
          ++ii;
          skip_dead();
        } else if (s.key != m.key ? s.key < m.key : s_clean_hash < m_clean_hash) {
          merged.push_back(EntrySnapshot{s.key, s_payload, s_hash});
          ++si;
          skip_dead();
        } else {
          merged.push_back(m);
          ++ii;
        }
      }
      while (si < sorted->size) {
        const SortedSlot &s = sorted->slots[si];
        const auto [s_hash, s_payload] = load_slot_consistent(s);
        merged.push_back(EntrySnapshot{s.key, s_payload, s_hash});
        ++si;
        skip_dead();
      }
      while (ii < imm_entries.size()) {
        merged.push_back(imm_entries[ii]);
        ++ii;
      }
    }

    // Assert no duplicate entries exist in the merge output
    assert_no_duplicates(merged);

    offset_mapper(std::span<EntrySnapshot>(merged));

    chk_writer(std::span<const EntrySnapshot>(merged));

    auto *next_sorted = new SortedRegion(merged);

    // Unfreezes both guarded regions right after: from this point, resolve() sees the new
    // (complete, unfrozen) sorted region directly, so the bypass is no longer needed for either
    // one.
    sorted_region_.store(next_sorted, std::memory_order_release);
    sorted_write_frozen_.unfreeze();

    // 4. Safely retire the old buffers
    append_immutable_.unfreeze();

    // Advance epoch and wait for all epoch readers to exit
    uint64_t target_epoch = reorg_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    active_epochs_.wait_until_epoch(target_epoch);

    delete old_active_gen->region;
    note_append_region_destroyed();
    delete old_active_gen->index;
    delete old_active_gen;
    // `sorted` (loaded above, before the merge) is still the pointer sorted_region_ held until
    // the store() above just replaced it -- reorg_in_progress_'s single-flight guarantees nothing
    // else could have written sorted_region_ in between, so no second load is needed here.
    delete sorted;

    reorg_in_progress_.store(false, std::memory_order_release);
  }

  // Installs a pre-built sorted_region (from a T1 checkpoint file) as this index's entire live
  // state. Unlike reorganize(), no synchronization or old-region retirement -- only valid
  // before any concurrent access begins (VMemKVImpl construction). `entries` must already be
  // sorted by key ascending.
  void load_sorted_region_from_checkpoint(std::span<const EntrySnapshot> entries) {
    // Built before the old snapshot is torn down (matching reorganize()'s own ordering) so a
    // throw from SortedRegion's constructor (e.g. bad_alloc) leaves sorted_region_ pointing at
    // its original, still-valid value instead of a dangling already-deleted pointer.
    auto *next_sorted = new SortedRegion(entries);
    delete sorted_region_.load(std::memory_order_relaxed);
    sorted_region_.store(next_sorted, std::memory_order_relaxed);
  }

 private:
  // ─── Private Type Definitions ──────────────────────────────────────────

  // Guards a region that reorganize() is about to replace wholesale with a freshly-built
  // snapshot (append_immutable_'s region, and the sorted region while reorganize()'s merge loop
  // is reading it). Between the moment a region's contents are snapshotted into `merged` and the
  // moment the replacement is published, an in-place write landing on the *pre-snapshot* region
  // physically succeeds but is never reflected in the replacement -- a silent Lost Update,
  // invisible to the writer (put() returns Applied) and to every reader once the swap lands.
  //
  // This type is the single place that hazard is closed: while frozen, resolve()'s write-path
  // lookup must treat any key found via `get()`'s returned region as not-found instead, forcing
  // put() to insert into the live append region (whose own retry/displacement machinery already
  // handles a fresh insert safely -- see put()'s own comment). Any future region built the same
  // way (snapshot-then-atomic-swap) must route its write-path lookup through an instance of this
  // type rather than re-deriving its own bypass by hand -- exactly the mistake that originally
  // left the sorted region unguarded (see the regression test "T1Index: dedicated single writer
  // per key survives racing reorganize with exact last value").
  //
  // Read paths are unaffected: a frozen region's bytes are still the live, correct view of the
  // world until the replacement publishes, so get()/scan() keep reading through it directly
  // (e.g. lookup_by_prefix_hash() reads append_immutable_ itself, not through this guard) --
  // only put()'s write-target resolution needs to refuse to hand back a doomed slot.
  template <typename RegionPtr>
  class FreezableRegion {
   public:
    void freeze(RegionPtr region) noexcept { frozen_.store(region, std::memory_order_release); }
    void unfreeze() noexcept { frozen_.store(nullptr, std::memory_order_release); }

    // The frozen region snapshot if one is currently in effect, nullptr otherwise. Caller runs
    // its own region-specific find() against the result -- this type only owns the freeze/unfreeze
    // lifecycle, not the lookup itself (append and sorted regions have unrelated find shapes).
    auto get() const noexcept -> RegionPtr { return frozen_.load(std::memory_order_acquire); }

   private:
    std::atomic<RegionPtr> frozen_{nullptr};
  };

  struct SortedSlot {
    Key key{};
    // Atomic: put()'s in-place update path mutates a live, published SortedSlot's hash
    // concurrently with reorganize()'s merge loop and find_sorted()/get_with_hash() reading it. A
    // plain field was a real data race (UB). mutable so it's writable through ResolvedSlot's
    // `const SortedSlot*`.
    mutable std::atomic<uint64_t> hash{0};
    mutable std::atomic<Payload> payload_bits{STORE_NOT_FOUND};
    // Seqlock guarding the (hash, payload_bits) pair -- individually-atomic fields rule out a
    // torn *value* in either one, but not a reader observing them at different points in time
    // across put()'s multi-step update (see ResolvedSlot::begin_write()). Starts at 0 (even);
    // only put()'s in-place path touches it, always odd->even bracketed.
    mutable std::atomic<uint64_t> version{0};

    auto clean_hash() const noexcept -> uint64_t {
      return hash.load(std::memory_order_acquire) & t1_detail::kCleanHashMask;
    }
  };

  struct AppendSlot {
    Key key{};
    // Atomic for the same reason as SortedSlot::hash above.
    std::atomic<uint64_t> hash{0};
    // mutable: LockFreeHashTable::publish_slot()'s on_displaced callback tombstones a losing
    // slot's payload through a `const AppendSlot&` received for comparison only (same pattern as
    // SortedSlot::payload_bits above).
    mutable std::atomic<Payload> payload_bits{STORE_NOT_FOUND};
    std::atomic<bool> published{false};
    // Same reasoning as SortedSlot::version above. Not needed for the initial publish()
    // (published's acquire/release already makes hash/payload_bits visible together); only for
    // later in-place updates.
    mutable std::atomic<uint64_t> version{0};

    [[nodiscard]] auto clean_hash() const noexcept -> uint64_t {
      return hash.load(std::memory_order_acquire) & t1_detail::kCleanHashMask;
    }
  };

 public:
  static constexpr size_t APPEND_SLOT_SIZE = sizeof(AppendSlot);

 private:
  struct SortedRegion {
    size_t size = 0;
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    std::unique_ptr<SortedSlot[]> slots;
    [[no_unique_address]] std::conditional_t<Config::UseBloomFilter, t1_detail::BloomFilter, EmptyOption> bloom;

    SortedRegion() = default;
    explicit SortedRegion(std::span<const EntrySnapshot> entries)
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        : size(entries.size()), slots(size == 0 ? nullptr : std::make_unique<SortedSlot[]>(size)) {
      if constexpr (Config::UseBloomFilter) {
        bloom.reset(entries.size());
      }
      for (size_t i = 0; i < size; ++i) {
        slots[i].key = entries[i].key;
        slots[i].hash.store(entries[i].hash, std::memory_order_relaxed);
        slots[i].payload_bits.store(entries[i].payload_bits, std::memory_order_relaxed);
        if constexpr (Config::UseBloomFilter) {
          bloom.add(entries[i].hash & t1_detail::kCleanHashMask);
        }
      }
    }
  };

  // Walks the sorted region within [lower_bound, upper_bound], calling sink(slot, hash, payload)
  // for each live match. Shared by scan()'s fast (direct-delivery) and slow
  // (candidate-buffering) paths so the sorted-region walk has exactly one implementation; the
  // sink decides what happens per match. Templated rather than a std::function so each call
  // site's sink is fully inlined.
  template <typename Sink>
  static void walk_sorted_region(const SortedRegion *sorted, Key lower_bound, Key upper_bound, Sink &&sink) {
    if (sorted == nullptr || sorted->size == 0) {
      return;
    }
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
      // Paired read via load_slot_consistent() -- feeds scan_impl()'s T2 dereference, same
      // reasoning as get_with_hash() (see SortedSlot::version).
      const auto [hash, payload] = load_slot_consistent(*it);
      if (payload != vmemkv::STORE_NOT_FOUND) {
        sink(*it, hash, payload);
      }
    }
  }

  using AppendIndex = LockFreeHashTable<Key, AppendSlot, APPEND_CAP>;

  struct ResolvedSlot {
    AppendSlot *append = nullptr;
    const SortedSlot *sorted = nullptr;

    [[nodiscard]] auto found() const noexcept -> bool { return append != nullptr || sorted != nullptr; }

    // Identity comparison (which physical slot, not its contents) -- lets put()'s retry loop
    // distinguish an ordinary delete (resolve() still finds the same slot) from a concurrent
    // racer's displacement (resolve() now finds a different slot).
    [[nodiscard]] auto same_slot_as(const ResolvedSlot &other) const noexcept -> bool {
      return append == other.append && sorted == other.sorted;
    }

    [[nodiscard]] auto load() const noexcept -> Payload {
      if (append != nullptr) {
        return append->payload_bits.load(std::memory_order_acquire);
      }
      return sorted->payload_bits.load(std::memory_order_acquire);
    }

    // CAS from `expected_current` to `value`; on failure, `expected_current` is updated to the
    // actual value and the caller must not retry blindly (see put()'s call site). CAS, not a
    // plain store: a plain store could silently overwrite a concurrent displacement's tombstone
    // (LockFreeHashTable::publish_slot()'s on_displaced), resurrecting an already-orphaned slot
    // that collect_live_entries()/scan() would then carry forward as a permanent duplicate.
    [[nodiscard]] auto try_store(Payload &expected_current, Payload value) const noexcept -> bool {
      if (append != nullptr) {
        return append->payload_bits.compare_exchange_strong(
            expected_current, value, std::memory_order_release, std::memory_order_acquire);
      }
      return sorted->payload_bits.compare_exchange_strong(
          expected_current, value, std::memory_order_release, std::memory_order_acquire);
    }

    // No const_cast needed: hash is `mutable std::atomic`, same as payload_bits above.
    void store_hash(uint64_t hash) const noexcept {
      if (append != nullptr) {
        append->hash.store(hash, std::memory_order_release);
      } else {
        sorted->hash.store(hash, std::memory_order_release);
      }
    }

    [[nodiscard]] auto version_ref() const noexcept -> std::atomic<uint64_t> & {
      return append != nullptr ? append->version : sorted->version;
    }

    // Brackets put()'s in-place update with the seqlock (see SortedSlot::version). Safe
    // unconditionally: only one thread is ever in this bracket per slot (put() holds the
    // caller's per-key stripe lock, see vmemkv_impl.hpp), so only concurrent readers can
    // interleave. Bumping version on a failed CAS is harmless -- a reader just retries once.
    void begin_write() const noexcept { version_ref().fetch_add(1, std::memory_order_release); }
    void end_write() const noexcept { version_ref().fetch_add(1, std::memory_order_release); }
  };

  // Slot-type-generic seqlock reader for callers with a raw `const SortedSlot&`/`const
  // AppendSlot&` (reorganize()'s merge loop, AppendRegion::collect_live_entries()). Reads
  // (hash, payload_bits) as a consistent pair, immune to a racing begin_write()/end_write()
  // bracket (see SortedSlot::version).
  template <typename SlotT>
  [[nodiscard]] static auto load_slot_consistent(const SlotT &slot) noexcept -> std::pair<uint64_t, Payload> {
    while (true) {
      const uint64_t v1 = slot.version.load(std::memory_order_acquire);
      if (v1 % 2 != 0) {
        std::this_thread::yield();
        continue;
      }
      const uint64_t hash = slot.hash.load(std::memory_order_acquire);
      const Payload payload = slot.payload_bits.load(std::memory_order_acquire);
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint64_t v2 = slot.version.load(std::memory_order_acquire);
      if (v1 == v2) {
        return {hash, payload};
      }
    }
  }

  class AppendRegion {
   public:
    // AppendSlot's `published{false}` default is all-zero, matching MAP_ANONYMOUS memory's
    // initial state (see LockFreeHashTable's constructor) -- unpublished slots are never
    // faulted in or zeroed up front.
    AppendRegion()
        : slots_(static_cast<AppendSlot *>(::mmap(nullptr,
                                                  APPEND_CAP * sizeof(AppendSlot),
                                                  PROT_READ | PROT_WRITE,
                                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                                  -1,
                                                  0))) {
      if (slots_ == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap");
      }
    }

    ~AppendRegion() { ::munmap(slots_, APPEND_CAP * sizeof(AppendSlot)); }

    AppendRegion(const AppendRegion &) = delete;
    auto operator=(const AppendRegion &) -> AppendRegion & = delete;

    auto data() noexcept -> AppendSlot * { return slots_; }
    [[nodiscard]] auto data() const noexcept -> const AppendSlot * { return slots_; }

    [[nodiscard]] auto size() const noexcept -> size_t { return tail_.load(std::memory_order_acquire); }

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
      slots_[index].hash.store(hash, std::memory_order_relaxed);
      slots_[index].payload_bits.store(value, std::memory_order_relaxed);
      // update_bounds() before the published flag, not after: scan()'s extract_append() checks
      // bounds() up front to decide whether to skip this region entirely. Reversing the order
      // could let bounds() say "out of range" for a key already visible via published==true.
      update_bounds(key);
      slots_[index].published.store(true, std::memory_order_release);
    }

    // Conservative [min, max] envelope of keys published into this region, letting
    // T1Index::scan() skip this region's O(size()) scan when the query range can't intersect it.
    //
    // Key (16 bytes) isn't lock-free as std::atomic without -mcx16, so a dedicated spinlock
    // guards just this {min, max} pair, independent of the lock-free insert path (reserve()/
    // publish() elsewhere).
    void update_bounds(Key key) noexcept {
      while (bounds_lock_.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      if (!has_bounds_) {
        bounds_min_ = key;
        bounds_max_ = key;
        has_bounds_ = true;
      } else {
        if (key < bounds_min_) bounds_min_ = key;
        if (bounds_max_ < key) bounds_max_ = key;
      }
      bounds_lock_.clear(std::memory_order_release);
    }

    // Returns false if no key has ever been published (caller falls through to normal scan).
    [[nodiscard]] auto bounds(Key &out_min, Key &out_max) const noexcept -> bool {
      while (bounds_lock_.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const bool present = has_bounds_;
      if (present) {
        out_min = bounds_min_;
        out_max = bounds_max_;
      }
      bounds_lock_.clear(std::memory_order_release);
      return present;
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

    void collect_live_entries(std::vector<EntrySnapshot> &out, size_t count) const {
      for (size_t i = 0; i < count; ++i) {
        const AppendSlot &slot = slots_[i];
        if (!slot.published.load(std::memory_order_acquire)) {
          continue;
        }
        // Reads (hash, payload_bits) as a consistent pair via load_slot_consistent() -- every
        // entry here feeds reorganize()'s offset_mapper, so a torn pair could misdirect a T2
        // read (see SortedSlot::version's declaration).
        const auto [hash, value] = load_slot_consistent(slot);
        if (!is_live(value)) {
          continue;
        }
        out.push_back(EntrySnapshot{slot.key, value, hash});
      }
    }

   private:
    AppendSlot *slots_;
    std::atomic<size_t> tail_{0};

    // See update_bounds()/bounds() above. mutable: bounds() is logically const (just a read),
    // but must briefly take the lock like any other reader.
    mutable std::atomic_flag bounds_lock_;
    bool has_bounds_ = false;
    Key bounds_min_{};
    Key bounds_max_{};
  };

  // Bundles an AppendRegion with its paired hash index as a single, atomically-swappable unit
  // (append_active_/append_immutable_ each point to one). Region and index must always be
  // observed together -- two independent atomics could be read as a torn pair (region matched
  // with the wrong generation's index, including a null-pointer dereference). Bundling behind
  // one atomic makes every publish a single pointer store, ruling that out.
  struct AppendGeneration {
    AppendRegion *region;
    AppendIndex *index;
  };

  static void delete_generation(AppendGeneration *generation) noexcept {
    if (generation == nullptr) {
      return;
    }
    delete generation->region;
    delete generation->index;
    delete generation;
  }

  void note_append_region_created() noexcept {
    int64_t live = append_region_live_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t peak = append_region_peak_count_.load(std::memory_order_relaxed);
    while (live > peak && !append_region_peak_count_.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
  }
  void note_append_region_destroyed() noexcept { append_region_live_count_.fetch_sub(1, std::memory_order_relaxed); }

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

  // Runs `func` registered in active_epochs_. Every method dereferencing sorted_region_/
  // append_active_/append_immutable_ must go through this, or reorganize()'s wait_until_epoch()
  // can't know the buffers it's about to delete are still in use, letting a caller like put()
  // dereference a buffer reorganize() has already freed.
  //
  // Scoped to exactly `func`'s duration: nothing inside `func` may block on reorganize()
  // completing, or it would deadlock against wait_until_epoch().
  template <typename Func>
  auto with_epoch_guard(Func &&func) const noexcept(noexcept(func())) -> decltype(func()) {
    T1ReadHandle handle(active_epochs_, reorg_epoch_.load(std::memory_order_relaxed));
    return func();
  }

  auto resolve(Key key, uint64_t hash) noexcept -> ResolvedSlot {
    AppendGeneration *active_gen = append_active_.load(std::memory_order_acquire);
    if (AppendSlot *slot = active_gen->region->find_with_index(*active_gen->index, key, hash)) {
      return ResolvedSlot{slot, nullptr};
    }

    // Found in either frozen region means an in-place update would be a Lost Update this cycle
    // (see FreezableRegion's own comment) -- bypass to a new entry in the fresh active region
    // instead. Safe because T2 never relocates a record, so a bypass entry's offset stays valid
    // indefinitely; a later reorganize() dedups it against the frozen copy by key + clean hash
    // regardless of how many cycles pass before that happens.
    if (AppendGeneration *imm_gen = append_immutable_.get()) {
      if (imm_gen->region->find_with_index(*imm_gen->index, key, hash) != nullptr) {
        return ResolvedSlot{};
      }
    }

    if (const SortedRegion *frozen_sorted = sorted_write_frozen_.get()) {
      // While frozen, sorted_region_ points at this exact same region (reorganize() pairs the
      // freeze/unfreeze around the merge and publish), so checking it again below would be
      // redundant -- not found here means not found in the live sorted region either.
      if (find_sorted(*frozen_sorted, key, hash) != nullptr) {
        return ResolvedSlot{};
      }
    } else {
      const auto sorted = sorted_region_.load(std::memory_order_acquire);
      if (const SortedSlot *slot = find_sorted(*sorted, key, hash)) {
        return ResolvedSlot{nullptr, slot};
      }
    }

    return ResolvedSlot{};
  }

  void publish_append_index(
      AppendIndex &idx, const AppendRegion &region, size_t index, Key key, uint64_t hash) noexcept {
    const auto slot_plus_one = static_cast<typename AppendIndex::slot_index_type>(index + 1);
    idx.publish_slot(
        key,
        slot_plus_one,
        hash,
        [&](size_t slot_index) -> const AppendSlot & { return region.data()[slot_index]; },
        [](const AppendSlot &displaced) {
          // Loser of a same-key insert race, about to become unreachable via the hash index.
          // Tombstone it so collect_live_entries()/scan() (which don't consult the index) don't
          // carry it forward as a duplicate.
          displaced.payload_bits.store(vmemkv::STORE_NOT_FOUND, std::memory_order_release);
        });
  }

  static void assert_no_duplicates(const std::vector<EntrySnapshot> &entries) {
    if (entries.size() < 2) {
      return;
    }
    for (size_t i = 1; i < entries.size(); ++i) {
      // Clean hash, matching the merge's dedup identity -- raw hash would miss an
      // inline<->non-inline duplicate.
      assert(!(entries[i - 1].key == entries[i].key &&
               (entries[i - 1].hash & t1_detail::kCleanHashMask) == (entries[i].hash & t1_detail::kCleanHashMask)) &&
             "T1Index invariant violated: duplicate key prefix + clean hash detected in merge output");
    }
  }

  // ─── Member Variables ──────────────────────────────────────────────────
  mutable std::atomic<bool> reorg_in_progress_{false};
  std::atomic<uint64_t> reorg_epoch_{1};

  std::atomic<const SortedRegion *> sorted_region_{nullptr};
  std::atomic<AppendGeneration *> append_active_{nullptr};
  FreezableRegion<AppendGeneration *> append_immutable_;
  std::atomic<int64_t> append_region_live_count_{0};
  std::atomic<int64_t> append_region_peak_count_{0};
  // See FreezableRegion's own comment. Frozen for the duration of reorganize()'s merge loop
  // (the same SortedRegion sorted_region_ currently points at), unfrozen right after the
  // replacement publishes -- closes the sorted-region counterpart of the Lost Update hazard
  // append_immutable_ already guarded against.
  FreezableRegion<const SortedRegion *> sorted_write_frozen_;

  mutable ThreadReferenceTracker<uint64_t> active_epochs_;
};

}  // namespace vmemkv
