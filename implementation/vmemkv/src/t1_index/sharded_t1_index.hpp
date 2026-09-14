// sharded_t1_index.hpp -- Range-sharded routing layer over K independent T1Index instances.
//
// See docs/t1_sharding_design.md for the full design (directory RCU, split protocol, forwarding
// pointer state machine, scan consistency scope). T1Index is the per-shard leaf implementation,
// extended with sharding hooks (freeze/bypass, offset-mapped reorganize, sorted-region
// dump/load).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>
#include <vmemkv/config.hpp>

#include "../core/background_poll.hpp"
#include "../core/reference_tracker.hpp"
#include "../core/single_flight.hpp"
#include "../core/spin_backoff.hpp"
#include "t1_index.hpp"

namespace vmemkv {

template <typename Config = vmemkv::Config<>>
class ShardedT1Index {
 public:
  using Key = StoreKey;
  using Payload = uint64_t;
  using Shard = T1Index<Config>;
  using PutResult = typename Shard::PutResult;
  using LookupResult = typename Shard::LookupResult;
  using EntrySnapshot = typename Shard::EntrySnapshot;

  // `target_shard_size`: see docs/t1_sharding_design.md's "Splitting"/"APPEND_CAPのスケーリング"
  // sections -- the one knob this design intends callers to actually tune. A shard's background
  // maintenance reorganize() splits it once its live entry count reaches
  // Config::T1ShardSplitThresholdPercent% of this target.
  // `worker_threads`: size of the shared background maintenance pool (see "背景ワーカーの
  // スレッドプール" section); defaults to a quarter of hardware concurrency, reserving most
  // cores for foreground traffic, per that section's reasoning. Pass 0 to defer startup (see
  // start_workers()) -- needed by a caller that must finish a single-threaded setup step (e.g.
  // loading a checkpoint) before any background thread could safely touch this instance.
  explicit ShardedT1Index(size_t append_cap = Config::T1AppendCapacityEntries,
                          size_t target_shard_size = Config::T1ShardTargetSizeEntries,
                          size_t worker_threads = default_worker_count())
      : append_cap_(append_cap), target_shard_size_(target_shard_size) {
    auto *slot = new ShardSlot(std::make_unique<Shard>(append_cap));
    auto *dir = new Directory();
    dir->shards.push_back(slot);
    directory_.store(dir, std::memory_order_release);
    start_workers(worker_threads);
  }

  // Spawns `count` additional background workers. Only safe to call before this instance is
  // shared with any other thread (construction with worker_threads=0, then this, exactly once)
  // -- workers_ itself isn't synchronized, matching load_from_checkpoint()'s own
  // single-threaded-caller contract for the same reason.
  void start_workers(size_t count = default_worker_count()) {
    workers_.reserve(workers_.size() + count);
    for (size_t i = 0; i < count; ++i) {
      workers_.emplace_back([this](std::stop_token stop_token) { worker_loop(stop_token); });
    }
  }

  // Stops and joins every background worker *before* tearing down shards/directory below --
  // otherwise a worker could still be mid-reorganize() on a ShardSlot this destructor is about
  // to delete. jthread::request_stop() only sets a flag worker_loop() checks between iterations
  // (see its own comment for why: matches vmemkv_impl.hpp's reorg_worker_loop() polling
  // convention), so this can block until any in-flight maintenance finishes naturally.
  ~ShardedT1Index() {
    for (auto &worker : workers_) {
      worker.request_stop();
    }
    workers_.clear();  // Each jthread's destructor joins.

    Directory *dir = directory_.load(std::memory_order_relaxed);
    for (ShardSlot *slot : dir->shards) {
      delete slot;
    }
    delete dir;
  }

  ShardedT1Index(const ShardedT1Index &) = delete;
  auto operator=(const ShardedT1Index &) -> ShardedT1Index & = delete;

  auto get(std::span<const std::byte> key) const -> Payload { return get_with_hash(key).payload_bits; }

  auto get_with_hash(std::span<const std::byte> key) const -> LookupResult {
    const Key prefix = t1_detail::prefix_from_bytes(key);
    return with_routing_guard([&]() -> LookupResult {
      ShardSlot *slot = resolve(prefix);
      return slot->index->get_with_hash(key);
    });
  }

  // Requests background maintenance once the target shard crosses the soft threshold (mirrors
  // vmemkv_impl.hpp's maybe_reorganize_if_needed(), scoped to just this shard); if the shard's
  // append region is genuinely full, retries (spin-backoff) rather than blocking on an explicit
  // wait -- by the time it retries, either maintenance has drained room, or the shard has split
  // and resolve_for_write() routes to a fresh one. Backpressure is thus scoped to writers of
  // *this* shard's key range; writers to other shards are never affected (see
  // "キューが追いつかない場合の劣化特性" in docs/t1_sharding_design.md).
  //
  // After applying, re-checks the written slot's superseded: a split can retire the slot between
  // this call's resolve_for_write() and its T1Index::put(), landing the write in a shard that is
  // about to be deleted. In that case this call re-resolves (spinning past Closing, following a
  // completed Split{}) and re-applies the same key/value to the live shard before returning, so
  // every returned put is already in a live shard. Re-application is idempotent (same key, same
  // value, upserted) whether or not the retired slot's copy also reached the new shards via a
  // split snapshot, and it keeps this operation in flight until the value is live, so a newer
  // completed write to the same key can never be overwritten by this older one.
  auto put(std::span<const std::byte> key,
           Payload value,
           bool is_inline = false,
           uint8_t inline_size = 0) -> PutResult {
    const Key prefix = t1_detail::prefix_from_bytes(key);
    return with_routing_guard([&]() -> PutResult {
      SpinBackoff backoff;
      for (;;) {
        ShardSlot *slot = resolve_for_write(prefix);
        request_maintenance_if_needed(slot);
        const PutResult result = slot->index->put(key, value, is_inline, inline_size);
        if (result == PutResult::AppendRegionFull) {
          backoff.wait();
          continue;
        }
        if (slot->superseded.load(std::memory_order_acquire) != nullptr) {
          continue;
        }
        if (value == STORE_NOT_FOUND) {
          slot->tombstones_since_maintenance.fetch_add(1, std::memory_order_relaxed);
          // The pre-put request above only saw the pre-increment count -- re-check now that this
          // delete landed, so the final delete of a quiet workload still trips the trigger
          // without needing another write after it.
          request_maintenance_if_needed(slot);
        }
        return result;
      }
    });
  }

  // Marks whether a scan is currently in flight (set by the caller driving scan(), cleared when
  // it finishes). While set, request_maintenance_if_needed() lowers each shard's soft threshold
  // to keep append regions L2-cache-sized, so the scan's linear pass over them stays
  // cache-resident.
  void set_scan_active(bool active) const noexcept { scan_active_.store(active, std::memory_order_relaxed); }

  // Entries a shard's append region may hold before background maintenance is requested: half
  // the append capacity normally, capped at the L2-cache-sized slot budget while a scan is
  // active. Pure function of its inputs so the scan-active lowering stays unit-testable at
  // production-scale capacities (in-tree test configs are far too small for the L2 cap to bind).
  static auto maintenance_soft_threshold(size_t append_cap, bool scan_active) -> size_t {
    constexpr size_t kSoftThresholdPercent = 50;
    constexpr size_t kL2CacheSizeBytes = 1024 * 1024;
    const size_t normal = (append_cap * kSoftThresholdPercent) / 100;
    if (!scan_active) {
      return normal;
    }
    return std::min(kL2CacheSizeBytes / Shard::append_slot_bytes(), normal);
  }

  // Range scan over [lo_bytes, hi_bytes]. Structural completeness + per-key freshness only --
  // see docs/t1_sharding_design.md's "Scanの一貫性モデルのスコープ" for what this deliberately
  // does not promise (cross-key real-time ordering) and why.
  template <typename Callback>
  auto scan(std::span<const std::byte> lo_bytes,
            std::span<const std::byte> hi_bytes,
            Callback callback) const -> size_t {
    const Key lo = t1_detail::prefix_from_bytes(lo_bytes);
    const Key hi = t1_detail::prefix_from_bytes(hi_bytes);
    return with_routing_guard([&]() -> size_t {
      Directory *dir = directory_.load(std::memory_order_acquire);
      if (dir->shards.size() == 1) {
        // No boundary search, no leaf collection, no per-call leaf vector allocation: a single
        // shard answers the whole range directly (a split in progress reads through Closing).
        ShardSlot *slot = dir->shards[0];
        Split *split = slot->superseded.load(std::memory_order_acquire);
        if (split == nullptr || split == kClosingSentinel) {
          return slot->index->scan(lo_bytes, hi_bytes, callback);
        }
      }
      std::vector<ShardSlot *> leaves;
      ShardSetView{dir}.for_each_leaf_in_range(lo, hi, [&](ShardSlot *leaf) { leaves.push_back(leaf); });
      size_t total = 0;
      for (ShardSlot *leaf : leaves) {
        total += leaf->index->scan(lo_bytes, hi_bytes, callback);
      }
      return total;
    });
  }

  // Dereferences directory_, so -- like get/put/scan -- this must run under with_routing_guard():
  // without it, a concurrent split_shard() could publish a new directory, drain (which only
  // waits for routing-guarded callers), and delete the old one out from under an unguarded read
  // here.
  [[nodiscard]] auto shard_count() const -> size_t {
    return with_routing_guard([&]() -> size_t { return directory_.load(std::memory_order_acquire)->shards.size(); });
  }

  // Cumulative count of shard splits completed over this store's lifetime (continue_split()'s
  // success path only -- not incremented when a maintenance pass finds too little to split, see
  // kMinSplitEntries there). The real signal for "how much background T1 maintenance activity has
  // happened," independent of whether it was organic (background workers) or driven by an explicit
  // split_shard_containing() call -- both go through the same continue_split().
  [[nodiscard]] auto total_splits() const -> uint64_t { return total_splits_.load(std::memory_order_relaxed); }

  // Duration of the most recently completed split's writer-visible pause: from the moment the
  // Closing sentinel makes the target shard unresolvable (any in-flight resolve_for_write() call
  // starts spin-waiting) to the moment superseded is set to the real Split{...} (spinning writers
  // wake up and redirect to the new shards) -- see continue_split()'s own comment on exactly where
  // this is measured. Deliberately *not* the whole continue_split() call's duration: the epoch
  // drain and straggler redistribution after that point run with writers already unblocked, so
  // including them would overstate the pause an actual caller experiences. "Last" (not
  // aggregated): concurrent splits of different shards would race this single field, acceptable
  // for its diagnostic purpose (matches last_checkpoint_* in VMemKVStatistics), and moot for any
  // workload -- like monotonic-key inserts -- where only one shard is ever hot enough to split at
  // a time.
  [[nodiscard]] auto last_split_pause_us() const -> uint64_t {
    return last_split_pause_us_.load(std::memory_order_relaxed);
  }

  // steady_clock::now().time_since_epoch().count() at the end of that same pause (see
  // last_split_pause_us()) -- lets a caller running in the same process (any std::chrono::
  // steady_clock is process-local and otherwise meaningless) place the pause precisely on its own
  // timeline via its own steady_clock::now() calls, rather than inferring a window from whenever
  // it happens to observe total_splits() increment (which lags this by the epoch drain + straggler
  // redistribution's own cost -- see continue_split()'s comment at the store site).
  [[nodiscard]] auto last_split_pause_end_ns() const -> uint64_t {
    return last_split_pause_end_ns_.load(std::memory_order_relaxed);
  }

  // Sums one per-shard counter across every shard under a single routing guard (for
  // dereferencing directory_ safely -- same reason shard_count() needs one).
  template <typename Total, typename Counter>
  auto reduce_shards(Counter counter) const -> Total {
    return with_routing_guard([&]() -> Total {
      Total total = 0;
      for (ShardSlot *slot : directory_.load(std::memory_order_acquire)->shards) {
        total += (slot->index.get()->*counter)();
      }
      return total;
    });
  }

  // Sum of every shard's *current* append-region occupancy -- "is there anything anywhere left to
  // merge" for a caller deciding whether a T1-only reorganize would be a no-op (e.g.
  // VMemKVImpl::run_reorganize()'s own skip-if-nothing-to-do check, mirroring what it did against
  // the old unsharded T1Index's own append_size()).
  [[nodiscard]] auto append_size() const -> size_t { return reduce_shards<size_t>(&Shard::append_size); }

  // Whole-store aggregates of each shard's own (per-T1Index) RSS/instance-churn counters -- see
  // T1Index::append_region_live_count()'s own comment. Sums are taken under one routing guard for
  // the same reason shard_count() needs one: dereferencing directory_ safely.
  [[nodiscard]] auto append_region_live_count() const -> int64_t {
    return reduce_shards<int64_t>(&Shard::append_region_live_count);
  }

  // Approximate: sums each *currently live* shard's own peak, so a shard retired by a split after
  // reaching a high peak doesn't contribute it here. Acceptable for its monitoring use (see
  // T1Index::append_region_live_count()'s comment) -- not a precise store-lifetime maximum.
  [[nodiscard]] auto append_region_peak_count() const -> int64_t {
    return reduce_shards<int64_t>(&Shard::append_region_peak_count);
  }

  // Test/maintenance hook: performs a full Closing -> (re-)reorganize -> Split on the shard
  // whose range currently contains `key`. No-op (single-flight) if that shard is already being
  // split. See docs/t1_sharding_design.md's "Splitting" section for why the second reorganize()
  // call is required for correctness, not just an optimization.
  //
  // resolve() and the Closing CAS happen together inside one with_routing_guard() call: the CAS
  // dereferences the resolved ShardSlot, so it needs the same protection any other read of one
  // does (see continue_split()'s comment) -- a gap between resolving and CASing would let a
  // concurrent split of the same shard complete and free it first, leaving this CAS touching
  // freed memory. continue_split() itself runs unguarded, once this thread has already won
  // exclusive ownership of the target.
  void split_shard_containing(std::span<const std::byte> key) {
    const Key prefix = t1_detail::prefix_from_bytes(key);
    bool claimed = false;
    ShardSlot *target = with_routing_guard([&]() -> ShardSlot * {
      ShardSlot *slot = resolve(prefix);
      if (splits_paused_.load(std::memory_order_acquire)) {
        return slot;  // checkpoint_all_shards() in progress -- see its own comment. No-op.
      }
      Split *expected = nullptr;
      claimed = slot->superseded.compare_exchange_strong(expected, kClosingSentinel, std::memory_order_acq_rel);
      return slot;
    });
    if (claimed) {
      continue_split(target);
    }
  }

  // Captures every shard's live entries (merging its append region first, unless it is
  // already empty -- see the loop body's own comment) and invokes
  // offset_mapper(merged) then per_shard_writer(merged) once per shard, in ascending key order --
  // mirroring T1Index::reorganize()'s own (OffsetMapper, ChkWriter) contract exactly, just once
  // per shard instead of once overall (e.g. for T2 offset relocation: shards are visited in
  // ascending key order, matching the single-T1 case's own ordering, so a stateful offset_mapper
  // written for that case works unmodified here). Returns the current boundary keys
  // (size shard_count()-1) so the caller can serialize enough to reconstruct the directory later
  // via load_from_checkpoint().
  //
  // Splits are paused for this whole call (see splits_paused_'s own comment) so the directory
  // can't gain shards while this iterates. An already-in-flight split (one that won its Closing
  // CAS just before the pause took effect) can still complete concurrently; each shard touched
  // below is therefore held alive by ShardSlot::outside_refs instead of one long routing guard,
  // and continue_split()'s own delete path waits for those refs. Per-shard work (dump or
  // reorganize_until_captured()) runs outside any guard; only the directory snapshot itself is
  // taken under a short guard (generation management: the returned boundaries match exactly the
  // snapshotted shard set this call serialized).
  //
  // Pausing splits does *not* pause ordinary (non-splitting) background maintenance: a worker's
  // own reorganize() on the same shard can still be in flight, or start, concurrently with this
  // loop's own reorganize() call for that shard. T1Index::reorganize()'s reorg_in_progress_ CAS
  // guarantees at most one of the two actually runs -- but the *loser* returns immediately
  // without ever invoking its chk_writer callback at all, which would silently make this method
  // skip that shard's data entirely if not retried; reorganize_until_captured() below is what
  // retries each shard's call until its callback actually fires.
  template <typename OffsetMapper, typename PerShardWriter>
  auto checkpoint_all_shards(OffsetMapper offset_mapper, PerShardWriter per_shard_writer) -> std::vector<Key> {
    FlagGuard pause(splits_paused_);

    std::vector<ShardSlot *> snapshot;
    std::vector<Key> boundaries;
    with_routing_guard([&] {
      Directory *dir = directory_.load(std::memory_order_acquire);
      boundaries = dir->boundaries;
      snapshot = dir->shards;
      for (ShardSlot *slot : snapshot) {
        slot->outside_refs.fetch_add(1, std::memory_order_relaxed);
      }
    });
    struct RefRelease {
      std::vector<ShardSlot *> *slots;
      ~RefRelease() {
        for (ShardSlot *slot : *slots) {
          slot->outside_refs.fetch_sub(1, std::memory_order_relaxed);
        }
      }
    } ref_release{&snapshot};

    std::vector<EntrySnapshot> dumped;
    for (ShardSlot *slot : snapshot) {
      // Merge-free shortcut for shards with nothing new to merge: with an empty append
      // region, a merge's output is this shard's sorted region as-is (same key order, same
      // tombstone-skipping), so serializing it directly yields byte-identical checkpoint
      // content at O(sorted) walk cost instead of O(merge) cost. In-place updates to sorted
      // slots and concurrent background merges stay consistent for the same reason a merge
      // snapshot does: anything concurrent with this read is WAL-covered past this cycle's
      // checkpoint_lsn and converges via replay (see checkpoint_internal()'s LSN discipline).
      // The tombstone counter is deliberately *not* reset here -- only a real merge carries
      // tombstones away, so the pressure correctly survives until one runs.
      if (slot->index->append_size() == 0) {
        slot->index->dump_sorted_region(dumped);
        offset_mapper(std::span<EntrySnapshot>(dumped));
        per_shard_writer(dumped);
        continue;
      }
      reorganize_until_captured(*slot->index, offset_mapper, per_shard_writer, /*parallel_sort=*/false);
      slot->tombstones_since_maintenance.store(0, std::memory_order_relaxed);
    }
    return boundaries;
  }

  // Recovery-only: replaces this (freshly constructed, still-empty, not yet shared with any
  // other thread) instance's single default shard with a full directory reconstructed from
  // checkpoint data -- `boundaries.size() + 1 == per_shard_entries.size()`, both in the same
  // ascending order checkpoint_all_shards() produced them in. No synchronization: matches
  // T1Index::load_sorted_region_from_checkpoint()'s own single-threaded-caller contract, and must
  // be called before this instance's address is published to any other thread (in particular,
  // before any background worker could have anything to do, which is guaranteed here since a
  // freshly constructed instance's queue is always empty).
  void load_from_checkpoint(std::span<const Key> boundaries,
                            std::span<const std::vector<EntrySnapshot>> per_shard_entries) {
    auto *new_dir = new Directory();
    new_dir->boundaries.assign(boundaries.begin(), boundaries.end());
    new_dir->shards.reserve(per_shard_entries.size());
    for (const auto &entries : per_shard_entries) {
      auto shard = std::make_unique<Shard>(append_cap_);
      shard->load_sorted_region_from_checkpoint(entries);
      new_dir->shards.push_back(new ShardSlot(std::move(shard)));
    }

    Directory *old_dir = directory_.load(std::memory_order_relaxed);
    for (ShardSlot *slot : old_dir->shards) {
      delete slot;
    }
    delete old_dir;
    directory_.store(new_dir, std::memory_order_relaxed);
  }

 private:
  struct ShardSlot;

  // Published once, atomically, when a ShardSlot transitions Closing -> Split. `boundary`: keys
  // strictly less than it route to `low`, everything else to `high`.
  struct Split {
    Key boundary{};
    ShardSlot *low = nullptr;
    ShardSlot *high = nullptr;
  };

  // Sentinel distinguishing "being split, reject new writes, S1/S2 not decided yet" from a real
  // Split*. Never dereferenced -- only ever compared against.
  static Split *const kClosingSentinel;

  struct ShardSlot {
    explicit ShardSlot(std::unique_ptr<Shard> shard) : index(std::move(shard)) {}
    std::unique_ptr<Shard> index;
    // null: normal. kClosingSentinel: split/merge in progress, S1/S2 not yet decided (put/remove
    // must wait; get/scan may still read `index` directly). Anything else: a real Split*, redirect.
    mutable std::atomic<Split *> superseded{nullptr};
    // Dedups queue entries: only one outstanding maintenance request per shard at a time. Cleared
    // at the *start* of run_maintenance() (see its own comment), not after -- a fresh soft-
    // threshold trip during that run re-enqueues rather than being silently dropped.
    std::atomic<bool> maintenance_pending{false};
    // Tombstone puts applied to this shard since its last completed reorganize() (which merges
    // them away). Drives the delete-pressure half of request_maintenance_if_needed(); reset
    // wherever a reorganize() is known to have run (run_maintenance(), continue_split()'s abort
    // path, checkpoint_all_shards()). Relaxed: a heuristic trigger, exactness unnecessary -- a
    // concurrent put racing the reset only delays the next trigger by one round.
    std::atomic<uint64_t> tombstones_since_maintenance{0};
    // Hazard refs held while a worker/checkpoint touches this slot outside the routing guard
    // (see run_maintenance()/checkpoint_all_shards()). continue_split() waits for zero before
    // deleting the target.
    std::atomic<int> outside_refs{0};
  };

  // Pure split-size threshold shared by run_maintenance().
  static auto split_threshold(size_t target_shard_size, uint32_t percent) noexcept -> size_t {
    return (target_shard_size * percent) / 100;
  }

  // RAII true-while-held flag (splits_paused_). Shared helper lives in core/single_flight.hpp.
  using FlagGuard = vmemkv::FlagGuard;

  // Queue ownership shared by request_maintenance_if_needed()/run_maintenance()/continue_split().
  struct ShardQueue {
    std::mutex mutex;
    std::deque<ShardSlot *> queue;
    // Enqueues slot if live: CASes pending false->true, then under lock re-checks superseded
    // (same mutex continue_split() purges under, making push vs purge race-free).
    void push_if_live(ShardSlot *slot) {
      bool expected = false;
      if (!slot->maintenance_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
      }
      const std::lock_guard<std::mutex> lock(mutex);
      if (slot->superseded.load(std::memory_order_acquire) != nullptr) {
        return;
      }
      queue.push_back(slot);
    }
    // Pops one entry (or nullptr) and clears its pending flag for the next round.
    auto pop() -> ShardSlot * {
      const std::lock_guard<std::mutex> lock(mutex);
      if (queue.empty()) {
        return nullptr;
      }
      ShardSlot *slot = queue.front();
      queue.pop_front();
      slot->maintenance_pending.store(false, std::memory_order_release);
      return slot;
    }
    void purge(ShardSlot *target) {
      const std::lock_guard<std::mutex> lock(mutex);
      queue.erase(std::remove(queue.begin(), queue.end(), target), queue.end());
    }
  };

  // Immutable once published; replaced wholesale (RCU-style) on every split. boundaries.size()
  // == shards.size() - 1; shards[i] owns [boundaries[i-1], boundaries[i]) (boundaries[-1] = -inf,
  // boundaries[boundaries.size()] = +inf).
  struct Directory {
    std::vector<Key> boundaries;
    std::vector<ShardSlot *> shards;

    [[nodiscard]] auto shard_for(const Key &key) const -> ShardSlot * {
      const auto it = std::upper_bound(boundaries.begin(), boundaries.end(), key);
      return shards[static_cast<size_t>(it - boundaries.begin())];
    }
  };

  // Read vs write handling of the Closing sentinel, shared by resolve()/resolve_for_write()
  // so the three-way branch exists once.
  enum class ResolveMode { Read, Write };

  // Single Closing branch: Read treats Closing as a usable leaf, Write signals retry.
  // Returns nullptr only for Write-on-Closing (caller spins); otherwise the slot to use or
  // follow, with Split* redirects resolved by the caller.
  static auto closing_slot_for_mode(ShardSlot *slot, ResolveMode mode) -> ShardSlot * {
    if (mode == ResolveMode::Read) {
      return slot;
    }
    return nullptr;
  }

  // View over one Directory snapshot: initial routing plus leaf fan-out in range.
  struct ShardSetView {
    Directory *dir;
    void for_each_leaf_in_range(const Key &lo, const Key &hi, auto &&visit) const {
      const size_t start_idx =
          dir->boundaries.empty()
              ? 0
              : static_cast<size_t>(std::upper_bound(dir->boundaries.begin(), dir->boundaries.end(), lo) -
                                    dir->boundaries.begin());
      for (size_t i = start_idx; i < dir->shards.size(); ++i) {
        if (i > 0 && hi < dir->boundaries[i - 1]) {
          break;
        }
        collect_into(dir->shards[i], lo, hi, visit);
      }
    }
    static void collect_into(ShardSlot *slot, const Key &lo, const Key &hi, auto &&visit) {
      Split *split = slot->superseded.load(std::memory_order_acquire);
      if (split == nullptr || split == kClosingSentinel) {
        visit(closing_slot_for_mode(slot, ResolveMode::Read));
        return;
      }
      if (lo < split->boundary) {
        collect_into(split->low, lo, hi, visit);
      }
      if (!(hi < split->boundary)) {
        collect_into(split->high, lo, hi, visit);
      }
    }
  };

  // Read-path resolution: chases Split{} redirects but treats Closing as "still safe to read
  // this slot directly" (see class comment / docs/t1_sharding_design.md's scan consistency
  // scope -- get/scan never need to wait on a split in progress). The single-shard case skips
  // the directory's binary search: with one shard the routing is trivial, and this is the
  // hottest shape (every op on a corpus below the first split threshold). The routing epoch
  // guard itself is never skipped -- see with_routing_guard()'s contract.
  [[nodiscard]] auto resolve(const Key &key) const -> ShardSlot * {
    Directory *dir = directory_.load(std::memory_order_acquire);
    ShardSlot *slot = dir->shards.size() == 1 ? dir->shards[0] : dir->shard_for(key);
    for (;;) {
      Split *split = slot->superseded.load(std::memory_order_acquire);
      if (split == nullptr) {
        return slot;
      }
      if (split == kClosingSentinel) {
        return closing_slot_for_mode(slot, ResolveMode::Read);
      }
      slot = (key < split->boundary) ? split->low : split->high;
    }
  }

  // Write-path resolution: spins past Closing (never writes into a shard mid-split) until it
  // resolves to either a normal (null) slot or follows a completed Split{} redirect.
  [[nodiscard]] auto resolve_for_write(const Key &key) const -> ShardSlot * {
    Directory *dir = directory_.load(std::memory_order_acquire);
    ShardSlot *slot = dir->shards.size() == 1 ? dir->shards[0] : dir->shard_for(key);
    SpinBackoff backoff;
    for (;;) {
      Split *split = slot->superseded.load(std::memory_order_acquire);
      if (split == nullptr) {
        return slot;
      }
      if (split == kClosingSentinel) {
        if (closing_slot_for_mode(slot, ResolveMode::Write) == nullptr) {
          backoff.wait();
          continue;
        }
      }
      slot = (key < split->boundary) ? split->low : split->high;
    }
  }

  template <typename Func>
  auto with_routing_guard(Func &&func) const noexcept(noexcept(func())) -> decltype(func()) {
    typename ThreadReferenceTracker<uint64_t>::Guard handle(routing_epochs_,
                                                            routing_epoch_.load(std::memory_order_relaxed));
    return func();
  }

  // Retries shard.reorganize() until `on_merged` actually runs. T1Index::reorganize()'s
  // reorg_in_progress_ CAS can make a call a no-op without invoking the callback at all when it
  // races a concurrent reorganize() of the same shard (see that method's own comment) -- a bare
  // merged_entries.empty() check can't distinguish that from a genuinely empty shard, so callers
  // that need the real merge output retry through here instead.
  template <typename OffsetMapper, typename OnMerged>
  static void reorganize_until_captured(Shard &shard,
                                        OffsetMapper offset_mapper,
                                        OnMerged on_merged,
                                        bool parallel_sort) {
    bool captured = false;
    SpinBackoff backoff;
    while (!captured) {
      shard.reorganize(
          offset_mapper,
          [&](std::span<const EntrySnapshot> merged) {
            on_merged(merged);
            captured = true;
          },
          parallel_sort);
      if (!captured) {
        backoff.wait();
      }
    }
  }

  // Rest of the split protocol, run *after* the caller has already won `target`'s Closing CAS
  // (see docs/t1_sharding_design.md's "Splitting" section) -- deliberately unguarded (not called
  // from inside with_routing_guard()): winning that CAS makes `target` this thread's exclusive
  // property (no one else can free it until this function itself does), so no further protection
  // is needed here, and wrapping this slow part in a guard would deadlock this method's own
  // wait_until_epoch() call against the very guard it's waiting to see released.
  //
  // Callers must win the Closing CAS *while still routing-guarded* (see split_shard_containing()
  // and run_maintenance()): the CAS itself dereferences `target`, so it needs the same protection
  // as any other read of a ShardSlot, or a concurrent split completing between "resolve" and "CAS
  // attempt" could leave the CAS touching already-freed memory.
  void continue_split(ShardSlot *target) {
    // Marks the start of the window writers targeting `target` actually spend spin-waiting on
    // Closing (see resolve_for_write()) -- run_maintenance()'s own Closing CAS, just before this
    // call, is close enough to call this the start with negligible error (nothing of substance
    // runs between them). Stopped right after superseded.store(split_info, ...) below, the exact
    // point spinning writers wake up and redirect -- see last_split_pause_us()'s own comment for
    // why this range, not continue_split()'s full duration, is what a caller cares about.
    const auto split_pause_start = std::chrono::steady_clock::now();

    // Purge any queue entry for `target` now, under the same mutex push_if_live()
    // re-checks `superseded` under: together these guarantee no worker can ever dequeue
    // `target` after this point. Workers that already dequeued it hold outside_refs across
    // their unguarded reorganize(), which the wait before `delete target` below drains.
    queue_.purge(target);

    // Second reorganize(), taken *after* Closing is visible: captures target's state as of "no
    // new writer can resolve target anymore" (anything already inside T1Index::put() when Closing
    // was set is drained by T1Index's own existing reorg_epoch_/active_epochs_ mechanism, same as
    // any ordinary reorganize()). A writer that resolved target just before Closing but hasn't
    // reached T1Index::put() yet can still land a write after this snapshot; that residual window
    // is closed by the writer itself -- put() re-checks its slot's superseded and forwards its
    // own write to the live shard before returning (see its own comment).
    //
    // Retried via reorganize_until_captured() for the same reason as checkpoint_all_shards():
    // a redundant, concurrently-dequeued run_maintenance() attempt for this same shard (a
    // duplicate queue entry -- see run_maintenance()'s own comment) can be *also* mid-reorganize()
    // right now, making this call a no-op via T1Index::reorganize()'s reorg_in_progress_ CAS.
    // Without the retry, a shard could lose this race on every single split attempt for as long
    // as write pressure keeps regenerating duplicate queue entries, silently aborting each time
    // (the code below already resets superseded to null on "too small," making a lost race
    // indistinguishable from a genuinely tiny shard) and growing without bound -- exactly the
    // unsharded O(corpus) behavior this design exists to avoid.
    std::vector<EntrySnapshot> merged_entries;
    reorganize_until_captured(
        *target->index,
        [](std::span<EntrySnapshot> /*merged*/) {},
        [&](std::span<const EntrySnapshot> merged) { merged_entries.assign(merged.begin(), merged.end()); },
        /*parallel_sort=*/false);  // Many shards' reorganize() run concurrently; see
                                   // T1Index::reorganize()'s own doc comment on this parameter.

    constexpr size_t kMinSplitEntries = 2;
    if (merged_entries.size() < kMinSplitEntries) {
      // Genuinely too small to split (the callback did fire, so this reflects the shard's real
      // state, not a lost race) -- resume normal operation.
      //
      // superseded must be reset *before* maintenance_pending, not after: request_maintenance_if_
      // needed() first CASes maintenance_pending false->true, then checks superseded to decide
      // whether to actually enqueue. If maintenance_pending went false first, a concurrent caller
      // could win that CAS while superseded was still Closing here, see it, and back out --
      // leaving maintenance_pending stuck true forever on a shard that (a moment later) becomes
      // perfectly normal again, since nothing else will ever clear it outside a real dequeue.
      // Resetting superseded first closes that window: by the time maintenance_pending can be
      // won, superseded already reads null, so a concurrent winner always sees a shard safe to
      // enqueue.
      //
      // Clearing maintenance_pending here at all is still necessary despite that reordering: it
      // was left `true` by whichever request got purged above (see this function's own comment
      // on the queue purge), and nothing else will ever clear a purged (never dequeued) entry's
      // flag.
      target->superseded.store(nullptr, std::memory_order_release);
      target->maintenance_pending.store(false, std::memory_order_relaxed);
      target->tombstones_since_maintenance.store(0, std::memory_order_relaxed);
      return;
    }

    const size_t mid = merged_entries.size() / 2;
    const Key boundary = merged_entries[mid].key;
    auto low_shard = std::make_unique<Shard>(target->index->append_capacity());
    auto high_shard = std::make_unique<Shard>(target->index->append_capacity());
    low_shard->load_sorted_region_from_checkpoint(std::span<const EntrySnapshot>(merged_entries.data(), mid));
    high_shard->load_sorted_region_from_checkpoint(
        std::span<const EntrySnapshot>(merged_entries.data() + mid, merged_entries.size() - mid));

    auto *low_slot = new ShardSlot(std::move(low_shard));
    auto *high_slot = new ShardSlot(std::move(high_shard));
    auto *split_info = new Split{boundary, low_slot, high_slot};
    // Closing -> Split{...}: any put/remove spin-waiting on Closing wakes up and redirects.
    target->superseded.store(split_info, std::memory_order_release);

    const auto split_pause_end = std::chrono::steady_clock::now();
    last_split_pause_us_.store(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(split_pause_end - split_pause_start).count()),
        std::memory_order_relaxed);
    // Absolute steady_clock timestamp (not wall-clock -- unrelated to any real epoch), valid to
    // compare only against other steady_clock::now() calls within this same process. Lets a caller
    // that also calls steady_clock::now() itself (e.g. a benchmark probe timing its own workload
    // threads) place this pause precisely on its own timeline instead of guessing a window around
    // whenever it happens to observe total_splits() having incremented -- which, notably, is *not*
    // the same instant as split_pause_end: total_splits_ increments only once this whole function
    // returns, after the epoch drain and straggler redistribution below, both of which run *after*
    // writers are already unblocked.
    last_split_pause_end_ns_.store(static_cast<uint64_t>(split_pause_end.time_since_epoch().count()),
                                   std::memory_order_relaxed);

    Directory *old_dir = publish_directory_after_split(target, low_slot, high_slot, boundary);

    // Drain every routing-guarded call that could still be holding a raw reference to `target`
    // or `old_dir` from before this split, then free both. No post-drain redistribution of
    // target's leftovers is needed: put() re-checks its written slot's superseded and forwards
    // its own write to the live shard before returning (see its own comment), so any write that
    // landed in target after the second reorganize() above is re-applied by its own writer while
    // still in flight -- and, unlike a bulk re-application here, can never overwrite a newer
    // completed write to the same key. Draining before freeing (rather than before the snapshot)
    // avoids self-deadlock against writers spinning on Closing inside resolve_for_write(), which
    // only this function's own superseded.store() above can release.
    const uint64_t bumped = routing_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    routing_epochs_.wait_until_epoch(bumped);

    // Drain unguarded maintenance/checkpoint users holding outside_refs before freeing.
    {
      SpinBackoff backoff;
      while (target->outside_refs.load(std::memory_order_acquire) != 0) {
        backoff.wait();
      }
    }

    delete old_dir;
    delete target;
    total_splits_.fetch_add(1, std::memory_order_relaxed);
  }

  // Replaces `target` in the directory with {low_slot, high_slot} split at `boundary`, via a
  // CAS-retry loop (concurrent splits of *other* shards retry independently; `target` itself
  // can't be touched by another split since its own Closing CAS already made it single-flight).
  // Returns the retired old Directory* for the caller to free after its own epoch drain.
  auto publish_directory_after_split(ShardSlot *target,
                                     ShardSlot *low_slot,
                                     ShardSlot *high_slot,
                                     const Key &boundary) -> Directory * {
    for (;;) {
      Directory *old_dir = directory_.load(std::memory_order_acquire);
      const auto it = std::find(old_dir->shards.begin(), old_dir->shards.end(), target);
      const auto idx = static_cast<size_t>(it - old_dir->shards.begin());

      auto *new_dir = new Directory(*old_dir);
      new_dir->shards[idx] = low_slot;
      new_dir->shards.insert(new_dir->shards.begin() + static_cast<std::ptrdiff_t>(idx) + 1, high_slot);
      new_dir->boundaries.insert(new_dir->boundaries.begin() + static_cast<std::ptrdiff_t>(idx), boundary);

      if (directory_.compare_exchange_weak(old_dir, new_dir, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return old_dir;
      }
      delete new_dir;  // Lost the race to a concurrent split of a different shard; retry.
    }
  }

  static auto default_worker_count() -> size_t {
    const unsigned int hw = std::thread::hardware_concurrency();
    return std::max<size_t>(1, (hw == 0 ? 4 : hw) / 4);
  }

  // Soft-threshold check: enqueues `slot` for background maintenance if it isn't already queued
  // and either its append region crossed the soft threshold (scan-aware, see
  // maintenance_soft_threshold()) or it accumulated a shard's worth of tombstones since its last
  // maintenance -- the latter mirrors the old unsharded maybe_reorganize_if_needed_for_delete():
  // deletes only retarget T1 offsets, so without this a delete-heavy workload fills shards with
  // dead entries no occupancy threshold ever fires on. target_shard_size_ doubles as the live-size
  // proxy the old per-stripe live_count played, keeping this design's single tuning knob.
  //
  // Re-checks `superseded` *inside* the queue_mutex_ critical section, the same mutex
  // split_shard() purges under right after its Closing CAS -- this is what actually makes the
  // two operations race-free: whichever of "this push" and "that purge" the mutex serializes
  // first is authoritative (a push ordered before the purge gets cleaned up by it; a push
  // ordered after it sees superseded already set and backs out), so a queue entry for a shard
  // that's being/been split can never exist after this method returns. See split_shard()'s own
  // comment for why that guarantee is required for correctness, not just cleanliness.
  void request_maintenance_if_needed(ShardSlot *slot) {
    const size_t size = slot->index->append_size();
    const size_t cap = slot->index->append_capacity();
    const bool over_soft = size >= maintenance_soft_threshold(cap, scan_active_.load(std::memory_order_relaxed));
    const bool delete_heavy = slot->tombstones_since_maintenance.load(std::memory_order_relaxed) >= target_shard_size_;
    if (!over_soft && !delete_heavy) {
      return;
    }
    queue_.push_if_live(slot);
  }

  // Background worker body. Polls on a short, fixed interval rather than blocking on a condition
  // variable -- same rationale as vmemkv_impl.hpp's reorg_worker_loop(): bounds shutdown latency
  // without needing a wakeup signal, and the interval is imperceptible against a single
  // maintenance cycle's own multi-millisecond-plus duration.
  void worker_loop(const std::stop_token &stop_token) {
    constexpr auto kIdlePollInterval = kDefaultPoll10ms;
    while (!stop_token.stop_requested()) {
      if (!run_maintenance()) {
        std::this_thread::sleep_for(kIdlePollInterval);
      }
    }
  }

  // Only called from inside with_routing_guard() (run_maintenance() below) -- see that method's
  // own comment for why the dequeue itself, not just what's done with the result, must happen
  // under the guard.
  auto pop_queue() -> ShardSlot * { return queue_.pop(); }

  // One maintenance cycle: dequeue a slot (if any), a normal reorganize(), then split if the
  // result is big enough. Returns false only when the queue was empty (so worker_loop() knows to
  // poll-sleep); a dequeued slot that turned out to need no action still returns true.
  //
  // Lock scope: only the dequeue and the Closing CAS run inside with_routing_guard(); the
  // reorganize() between them runs outside it. A dequeued slot is kept alive across the
  // unguarded window by ShardSlot::outside_refs (taken while still guarded): continue_split()
  // waits for zero before deleting its target, so the pointer stays valid without holding the
  // routing guard for the whole merge. Multiple workers can still process the same slot --
  // harmless via T1Index::reorganize()'s reorg_in_progress_ CAS plus the single-flight Closing
  // CAS below. continue_split() (called only once we've actually won the CAS) runs unguarded,
  // as our own exclusive property -- same as split_shard_containing().
  auto run_maintenance() -> bool {
    struct Claimed {
      ShardSlot *slot = nullptr;
      bool held = false;
    };
    Claimed claimed = with_routing_guard([&]() -> Claimed {
      ShardSlot *slot = pop_queue();
      if (slot == nullptr) {
        return {};
      }
      if (slot->superseded.load(std::memory_order_acquire) != nullptr) {
        return {slot, false};
      }
      slot->outside_refs.fetch_add(1, std::memory_order_relaxed);
      return {slot, true};
    });
    if (claimed.slot == nullptr) {
      return false;
    }
    if (!claimed.held) {
      return true;
    }
    ShardSlot *slot = claimed.slot;

    std::vector<EntrySnapshot> merged_entries;
    bool reorganized = false;
    slot->index->reorganize([](std::span<EntrySnapshot> /*merged*/) {},
                            [&](std::span<const EntrySnapshot> merged) {
                              merged_entries.assign(merged.begin(), merged.end());
                              reorganized = true;
                            },
                            /*parallel_sort=*/false);
    if (reorganized) {
      slot->tombstones_since_maintenance.store(0, std::memory_order_relaxed);
    }

    ShardSlot *claimed_target = nullptr;
    with_routing_guard([&] {
      const bool big_enough =
          merged_entries.size() >= split_threshold(target_shard_size_, Config::T1ShardSplitThresholdPercent);
      if (big_enough && !splits_paused_.load(std::memory_order_acquire) &&
          slot->superseded.load(std::memory_order_acquire) == nullptr) {
        Split *expected = nullptr;
        if (slot->superseded.compare_exchange_strong(expected, kClosingSentinel, std::memory_order_acq_rel)) {
          claimed_target = slot;
        }
      }
      slot->outside_refs.fetch_sub(1, std::memory_order_relaxed);
    });
    if (claimed_target != nullptr) {
      continue_split(claimed_target);
    }
    return true;
  }

  // See set_scan_active().
  mutable std::atomic<bool> scan_active_{false};
  mutable ThreadReferenceTracker<uint64_t> routing_epochs_;
  std::atomic<uint64_t> routing_epoch_{1};
  std::atomic<uint64_t> total_splits_{0};
  std::atomic<uint64_t> last_split_pause_us_{0};
  std::atomic<uint64_t> last_split_pause_end_ns_{0};
  std::atomic<Directory *> directory_{nullptr};
  size_t append_cap_;
  size_t target_shard_size_;
  ShardQueue queue_;
  std::vector<std::jthread> workers_;
  // Set for the duration of checkpoint_all_shards() so the directory stays stable throughout
  // (see that method's own comment). Checked only at the point a split would actually be
  // initiated (winning the Closing CAS); already in-flight splits are unaffected.
  std::atomic<bool> splits_paused_{false};
};

template <typename Config>
typename ShardedT1Index<Config>::Split *const ShardedT1Index<Config>::kClosingSentinel =
    reinterpret_cast<typename ShardedT1Index<Config>::Split *>(static_cast<uintptr_t>(1));

}  // namespace vmemkv
