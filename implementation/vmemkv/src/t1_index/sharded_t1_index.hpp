// sharded_t1_index.hpp -- Range-sharded routing layer over K independent T1Index instances.
//
// See docs/t1_sharding_design.md for the full design (directory RCU, split protocol, forwarding
// pointer state machine, scan consistency scope). T1Index itself is reused unchanged as the
// per-shard leaf implementation.
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

#include "../core/reference_tracker.hpp"
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
        if (result != PutResult::AppendRegionFull) {
          return result;
        }
        backoff.wait();
      }
    });
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
      const size_t start_idx =
          dir->boundaries.empty()
              ? 0
              : static_cast<size_t>(std::upper_bound(dir->boundaries.begin(), dir->boundaries.end(), lo) -
                                    dir->boundaries.begin());
      std::vector<ShardSlot *> leaves;
      for (size_t i = start_idx; i < dir->shards.size(); ++i) {
        if (i > 0 && hi < dir->boundaries[i - 1]) {
          break;
        }
        collect_leaves(dir->shards[i], lo, hi, leaves);
      }
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

  // Sum of every shard's *current* append-region occupancy -- "is there anything anywhere left to
  // merge" for a caller deciding whether a T1-only reorganize would be a no-op (e.g.
  // VMemKVImpl::run_reorganize()'s own skip-if-nothing-to-do check, mirroring what it did against
  // the old unsharded T1Index's own append_size()).
  [[nodiscard]] auto append_size() const -> size_t {
    return with_routing_guard([&]() -> size_t {
      size_t total = 0;
      for (ShardSlot *slot : directory_.load(std::memory_order_acquire)->shards) {
        total += slot->index->append_size();
      }
      return total;
    });
  }

  // Whole-store aggregates of each shard's own (per-T1Index) RSS/instance-churn counters -- see
  // T1Index::append_region_live_count()'s own comment. Sums are taken under one routing guard for
  // the same reason shard_count() needs one: dereferencing directory_ safely.
  [[nodiscard]] auto append_region_live_count() const -> int64_t {
    return with_routing_guard([&]() -> int64_t {
      int64_t total = 0;
      for (ShardSlot *slot : directory_.load(std::memory_order_acquire)->shards) {
        total += slot->index->append_region_live_count();
      }
      return total;
    });
  }

  // Approximate: sums each *currently live* shard's own peak, so a shard retired by a split after
  // reaching a high peak doesn't contribute it here. Acceptable for its monitoring use (see
  // T1Index::append_region_live_count()'s comment) -- not a precise store-lifetime maximum.
  [[nodiscard]] auto append_region_peak_count() const -> int64_t {
    return with_routing_guard([&]() -> int64_t {
      int64_t total = 0;
      for (ShardSlot *slot : directory_.load(std::memory_order_acquire)->shards) {
        total += slot->index->append_region_peak_count();
      }
      return total;
    });
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

  // Forces every shard's reorganize() (fully draining its append region) and invokes
  // offset_mapper(merged) then per_shard_writer(merged) once per shard, in ascending key order --
  // mirroring T1Index::reorganize()'s own (OffsetMapper, ChkWriter) contract exactly, just once
  // per shard instead of once overall (e.g. for T2 offset relocation: shards are visited in
  // ascending key order, matching the single-T1 case's own ordering, so a stateful offset_mapper
  // written for that case works unmodified here). Returns the current boundary keys
  // (size shard_count()-1) so the caller can serialize enough to reconstruct the directory later
  // via load_from_checkpoint().
  //
  // Splits are paused for this whole call (see splits_paused_'s own comment) so the directory
  // can't gain or lose shards while this iterates -- matching how the unsharded T1Index's own
  // checkpoint()/reorg_worker_loop() already coordinate via reorg_running_. This alone isn't
  // sufficient, though: an already-in-flight split (one that won its Closing CAS just before the
  // pause took effect) isn't stopped by it and could still complete concurrently, so the whole
  // loop below also stays inside one with_routing_guard() call -- that's what actually keeps any
  // ShardSlot this loop touches alive for its whole duration (see continue_split()'s comment on
  // why the CAS itself needs this same protection). The one cost: such an in-flight split's own
  // final cleanup (its `delete old_dir; delete target;`) is delayed until this call returns,
  // since that split's own drain must wait for this call's guard to release first -- harmless,
  // since that split doesn't depend on this call to make progress.
  //
  // Pausing splits does *not* pause ordinary (non-splitting) background maintenance: a worker's
  // own reorganize() on the same shard can still be in flight, or start, concurrently with this
  // loop's own reorganize() call for that shard. T1Index::reorganize()'s reorg_in_progress_ CAS
  // guarantees at most one of the two actually runs -- but the *loser* returns immediately
  // without ever invoking its chk_writer callback at all, which would silently make this method
  // skip that shard's data entirely if not retried. So each shard's call is retried (with
  // backoff) until its callback actually fires, guaranteeing every shard contributes real data
  // regardless of transient contention with the background pool.
  template <typename OffsetMapper, typename PerShardWriter>
  auto checkpoint_all_shards(OffsetMapper offset_mapper, PerShardWriter per_shard_writer) -> std::vector<Key> {
    splits_paused_.store(true, std::memory_order_release);
    struct ResumeSplits {
      std::atomic<bool> *flag;
      ~ResumeSplits() { flag->store(false, std::memory_order_release); }
    } resume_splits{&splits_paused_};

    return with_routing_guard([&]() -> std::vector<Key> {
      Directory *dir = directory_.load(std::memory_order_acquire);
      for (ShardSlot *slot : dir->shards) {
        bool captured = false;
        SpinBackoff backoff;
        while (!captured) {
          slot->index->reorganize(
              offset_mapper,
              [&](std::span<const EntrySnapshot> merged) {
                per_shard_writer(merged);
                captured = true;
              },
              /*parallel_sort=*/false);
          if (!captured) {
            backoff.wait();
          }
        }
      }
      return dir->boundaries;
    });
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

  // Read-path resolution: chases Split{} redirects but treats Closing as "still safe to read
  // this slot directly" (see class comment / docs/t1_sharding_design.md's scan consistency
  // scope -- get/scan never need to wait on a split in progress).
  [[nodiscard]] auto resolve(const Key &key) const -> ShardSlot * {
    ShardSlot *slot = directory_.load(std::memory_order_acquire)->shard_for(key);
    for (;;) {
      Split *split = slot->superseded.load(std::memory_order_acquire);
      if (split == nullptr || split == kClosingSentinel) {
        return slot;
      }
      slot = (key < split->boundary) ? split->low : split->high;
    }
  }

  // Write-path resolution: spins past Closing (never writes into a shard mid-split) until it
  // resolves to either a normal (null) slot or follows a completed Split{} redirect.
  [[nodiscard]] auto resolve_for_write(const Key &key) const -> ShardSlot * {
    ShardSlot *slot = directory_.load(std::memory_order_acquire)->shard_for(key);
    SpinBackoff backoff;
    for (;;) {
      Split *split = slot->superseded.load(std::memory_order_acquire);
      if (split == nullptr) {
        return slot;
      }
      if (split == kClosingSentinel) {
        backoff.wait();
        continue;
      }
      slot = (key < split->boundary) ? split->low : split->high;
    }
  }

  // Appends every leaf reachable from `slot` whose range could intersect [lo, hi]. Fans out into
  // both sides of a Split{} when the query range straddles its internal boundary. Recursion
  // depth is bounded by how many times the same slot has been re-split since any reader could
  // have observed it -- one hop in every realistic case.
  void collect_leaves(ShardSlot *slot, const Key &lo, const Key &hi, std::vector<ShardSlot *> &out) const {
    Split *split = slot->superseded.load(std::memory_order_acquire);
    if (split == nullptr || split == kClosingSentinel) {
      out.push_back(slot);
      return;
    }
    if (lo < split->boundary) {
      collect_leaves(split->low, lo, hi, out);
    }
    if (!(hi < split->boundary)) {
      collect_leaves(split->high, lo, hi, out);
    }
  }

  template <typename Func>
  auto with_routing_guard(Func &&func) const noexcept(noexcept(func())) -> decltype(func()) {
    typename ThreadReferenceTracker<uint64_t>::Guard handle(routing_epochs_,
                                                            routing_epoch_.load(std::memory_order_relaxed));
    return func();
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

    // Purge any queue entry for `target` now, under the same mutex request_maintenance_if_needed()
    // re-checks `superseded` under (see that method's comment): together these guarantee no
    // worker can ever dequeue `target` after this point, which is what makes `delete target`
    // below safe. Without this, a duplicate queue entry (from a soft-threshold trip racing
    // run_maintenance()'s own clear-then-reorganize) could sit in the queue past this shard's
    // physical deletion and be dequeued by a worker as a dangling pointer -- workers, unlike
    // put()/get()/scan(), aren't routing_epochs_-guarded, so that guard doesn't protect them.
    {
      const std::lock_guard<std::mutex> lock(queue_mutex_);
      queue_.erase(std::remove(queue_.begin(), queue_.end(), target), queue_.end());
    }

    // Second reorganize(), taken *after* Closing is visible: captures target's state as of "no
    // new writer can resolve target anymore" (anything already inside T1Index::put() when Closing
    // was set is drained by T1Index's own existing reorg_epoch_/active_epochs_ mechanism, same as
    // any ordinary reorganize()). This is *not* yet target's truly-final state, though -- a writer
    // that resolved target just before Closing but hasn't reached T1Index::put() yet can still
    // land a write after this snapshot; see the post-split drain further below (right before
    // `delete target`) for how that residual window is closed.
    //
    // Retried until the callback actually fires (same pattern as checkpoint_all_shards()'s own
    // per-shard loop, for the identical reason): T1Index::reorganize()'s reorg_in_progress_ CAS
    // can make this call a no-op if a redundant, concurrently-dequeued run_maintenance() attempt
    // for this same shard (a duplicate queue entry -- see run_maintenance()'s own comment) is
    // *also* mid-reorganize() right now, in which case the callback never fires and
    // merged_entries stays empty -- not because the shard is actually small, but because this
    // call lost the race. Checking merged_entries.empty() can't tell those two cases apart; a
    // `captured` flag set only inside the callback can. Without this retry, a shard could lose
    // this race on every single split attempt for as long as write pressure keeps regenerating
    // duplicate queue entries, silently aborting each time (the code below already resets
    // superseded to null on "too small," making a lost race indistinguishable from a genuinely
    // tiny shard) and growing without bound -- exactly the unsharded O(corpus) behavior this
    // design exists to avoid.
    std::vector<EntrySnapshot> merged_entries;
    bool captured = false;
    SpinBackoff second_reorganize_backoff;
    while (!captured) {
      target->index->reorganize(
          [](std::span<EntrySnapshot> /*merged*/) {},
          [&](std::span<const EntrySnapshot> merged) {
            merged_entries.assign(merged.begin(), merged.end());
            captured = true;
          },
          /*parallel_sort=*/false);  // Many shards' reorganize() run concurrently; see
                                     // T1Index::reorganize()'s own doc comment on this parameter.
      if (!captured) {
        second_reorganize_backoff.wait();
      }
    }

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
    // or `old_dir` from before this split (see class comment on with_routing_guard()).
    const uint64_t bumped = routing_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    routing_epochs_.wait_until_epoch(bumped);

    // Catches a real data-loss window the second reorganize() above cannot: put()'s retry loop
    // (see its own definition) resolves a slot once via resolve_for_write(), then calls
    // slot->index->put() with no re-check of `superseded` in between. A writer that read
    // `superseded == null` a moment before this function set it to Closing can still be sitting
    // between those two steps when the second reorganize() above takes its snapshot, and only
    // reach slot->index->put() afterward -- landing in target's *post-snapshot* active
    // generation. T1Index's own reorg_epoch_/active_epochs_ drain can't see this writer (it isn't
    // inside T1Index::put() yet when the snapshot is taken), so without this step that write
    // would be silently destroyed by `delete target` below.
    //
    // The epoch drain just above is what makes this safe to do *now*, and specifically not any
    // earlier: it guarantees every routing-guarded caller registered before the bump -- including
    // any writer still holding a stale (pre-Closing) reference to `target` -- has completed its
    // own slot->index->put() call before this line runs. Attempting this same drain *before* the
    // snapshot (rather than reusing this one) would self-deadlock the split against exactly the
    // writers it is blocking: a writer that has already observed Closing spins inside
    // resolve_for_write() until this function's own superseded.store() above resolves it, so a
    // drain positioned earlier would be waiting on guards that can only ever be released by this
    // function making further progress. And no *new* writer can reach target from here on either:
    // resolve_for_write() only ever returns target while superseded reads null, which stopped
    // being possible the moment Closing was set, long before this point. So target's state is now
    // permanently final, and one more reorganize() sees all of it.
    std::vector<EntrySnapshot> stragglers;
    bool stragglers_captured = false;
    SpinBackoff straggler_backoff;
    while (!stragglers_captured) {
      target->index->reorganize(
          [](std::span<EntrySnapshot> /*merged*/) {},
          [&](std::span<const EntrySnapshot> merged) {
            stragglers.assign(merged.begin(), merged.end());
            stragglers_captured = true;
          },
          /*parallel_sort=*/false);
      if (!stragglers_captured) {
        straggler_backoff.wait();
      }
    }
    for (const EntrySnapshot &entry : stragglers) {
      ShardSlot *destination = entry.key < boundary ? low_slot : high_slot;
      destination->index->put_with_final_hash(entry.key, entry.hash, entry.payload_bits);
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
  // and its append region is more than half full. Mirrors vmemkv_impl.hpp's
  // maybe_reorganize_if_needed(), scoped to one shard instead of the whole (unsharded) store.
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
    constexpr size_t kSoftThresholdPercent = 50;
    if (size * 100 < cap * kSoftThresholdPercent) {
      return;
    }
    bool expected = false;
    if (!slot->maintenance_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }
    const std::lock_guard<std::mutex> lock(queue_mutex_);
    if (slot->superseded.load(std::memory_order_acquire) != nullptr) {
      // Lost the race to a concurrent split: `slot` is retired (or about to be). Leave
      // maintenance_pending=true permanently -- harmless, nothing will ever check it again on a
      // retired slot -- and, critically, do NOT enqueue it.
      return;
    }
    queue_.push_back(slot);
  }

  // Background worker body. Polls on a short, fixed interval rather than blocking on a condition
  // variable -- same rationale as vmemkv_impl.hpp's reorg_worker_loop(): bounds shutdown latency
  // without needing a wakeup signal, and the interval is imperceptible against a single
  // maintenance cycle's own multi-millisecond-plus duration.
  void worker_loop(const std::stop_token &stop_token) {
    constexpr auto kIdlePollInterval = std::chrono::milliseconds(10);
    while (!stop_token.stop_requested()) {
      if (!run_maintenance()) {
        std::this_thread::sleep_for(kIdlePollInterval);
      }
    }
  }

  // Only called from inside with_routing_guard() (run_maintenance() below) -- see that method's
  // own comment for why the dequeue itself, not just what's done with the result, must happen
  // under the guard.
  auto pop_queue() -> ShardSlot * {
    const std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) {
      return nullptr;
    }
    ShardSlot *slot = queue_.front();
    queue_.pop_front();
    return slot;
  }

  // One maintenance cycle: dequeue a slot (if any), a normal reorganize(), then split if the
  // result is big enough. Returns false only when the queue was empty (so worker_loop() knows to
  // poll-sleep); a dequeued slot that turned out to need no action still returns true.
  //
  // The dequeue itself runs inside with_routing_guard(), not just the reorganize/CAS-attempt that
  // follows it -- this is load-bearing, not just consistent style. A raw ShardSlot* obtained from
  // the queue is otherwise unprotected the instant pop_queue() returns it: unlike
  // resolve()/resolve_for_write() (which look up a slot *while already holding* the guard, so
  // continue_split()'s routing_epoch_ bump + wait_until_epoch() is guaranteed to drain them before
  // any delete), a pointer obtained *before* entering the guard could already be stale by the time
  // the guard is entered, and entering the guard afterward can't retroactively make a
  // already-freed access safe. Concretely, without this: a worker could pop `slot` off the queue,
  // then a concurrent split_shard_containing() (which resolves its own target independently of
  // the queue) could win the Closing CAS on that same slot, run continue_split() to completion,
  // bump routing_epoch_, and delete slot -- all invisible to routing_epochs_, since the worker
  // never registered a guard before touching it. Found via TSan on a stress test combining real
  // worker-driven splitting with a concurrent explicit splitter targeting the same shards (see
  // that test's own comment); reproduced both a TSan data race and outright data loss before this
  // fix.
  //
  // With the dequeue inside the guard: if it observes `slot` still queued, that's only possible if
  // it ran (per queue_mutex_'s total order over all queue operations) before any concurrent
  // continue_split()'s purge of that same slot -- which itself runs before that continue_split()'s
  // routing_epoch_ bump. So this guard's registration, with the pre-bump epoch, is guaranteed to
  // predate the bump, and wait_until_epoch() will wait for it. If the dequeue instead runs after
  // the purge, it simply never observes `slot` in the queue at all. Either way, no race.
  //
  // Multiple workers can still end up processing the same slot (a duplicate queue entry from a
  // fresh soft-threshold trip racing this method's own clear-then-reorganize) -- harmless:
  // T1Index::reorganize()'s own reorg_in_progress_ CAS makes the second call a no-op with an
  // empty merged span, and the Closing CAS below makes a resulting split attempt single-flight.
  // continue_split() (called only once we've actually won the CAS) runs unguarded, after the
  // guard above has been released, as our own exclusive property -- same as
  // split_shard_containing().
  auto run_maintenance() -> bool {
    ShardSlot *claimed_target = nullptr;
    const bool dequeued = with_routing_guard([&]() -> bool {
      ShardSlot *slot = pop_queue();
      if (slot == nullptr) {
        return false;
      }
      slot->maintenance_pending.store(false, std::memory_order_release);

      if (slot->superseded.load(std::memory_order_acquire) != nullptr) {
        return true;  // Already split/being split by the time this was dequeued -- no-op.
      }
      std::vector<EntrySnapshot> merged_entries;
      slot->index->reorganize(
          [](std::span<EntrySnapshot> /*merged*/) {},
          [&](std::span<const EntrySnapshot> merged) { merged_entries.assign(merged.begin(), merged.end()); },
          /*parallel_sort=*/false);  // See T1Index::reorganize()'s own doc comment on this parameter.

      const size_t split_threshold = (target_shard_size_ * Config::T1ShardSplitThresholdPercent) / 100;
      if (merged_entries.size() < split_threshold) {
        return true;
      }
      if (splits_paused_.load(std::memory_order_acquire)) {
        return true;  // checkpoint_all_shards() in progress -- see its own comment.
      }
      Split *expected = nullptr;
      if (slot->superseded.compare_exchange_strong(expected, kClosingSentinel, std::memory_order_acq_rel)) {
        claimed_target = slot;
      }
      return true;
    });
    if (claimed_target != nullptr) {
      continue_split(claimed_target);
    }
    return dequeued;
  }

  mutable ThreadReferenceTracker<uint64_t> routing_epochs_;
  std::atomic<uint64_t> routing_epoch_{1};
  std::atomic<uint64_t> total_splits_{0};
  std::atomic<uint64_t> last_split_pause_us_{0};
  std::atomic<uint64_t> last_split_pause_end_ns_{0};
  std::atomic<Directory *> directory_{nullptr};
  size_t append_cap_;
  size_t target_shard_size_;
  std::mutex queue_mutex_;
  std::deque<ShardSlot *> queue_;
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
