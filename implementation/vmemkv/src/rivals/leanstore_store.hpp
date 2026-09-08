// leanstore_store.hpp — Thin LeanStore rival wrapper exposing byte-span APIs.
//
// LeanStore (TUM, pointer-swizzling buffer-pool engine, pinned by commit in
// CMakeLists) speaks a typed, transactional API: every operation runs inside a TX on one of
// LeanStore's own worker threads. This wrapper bridges that to the harness's plain byte-span
// model by dispatching each call onto a worker via CRManager::scheduleJobSync() and retrying
// aborted transactions internally.
//
// Durability contract: WAL on with per-group fdatasync (FLAGS_wal_fsync), matching VMemKV's
// per-write fsync and RocksDB's sync=true as closely as LeanStore allows. Residual gap:
// LeanStore's commitTX enqueues to its group committer and returns without waiting for that
// group's fsync, so commit return precedes physical durability by up to one group interval.
// There is no synchronous-commit knob to close that gap.
//
// Value-size ceiling: BTreeVI lengths are u16, so values above 65535 bytes cannot be stored.
// The 64KB benchmark corpus (65536-byte values) is therefore excluded for this backend; see
// benchmark/common/benchmark_matrix.sh.
//
// Threading: LeanStore allows concurrent writers (OCC/SI); concurrent point operations
// serialize only on real conflicts (abort + internal retry), reported as-is. Dispatch onto a
// worker slot is guarded by a per-slot mutex: two caller threads hashing to the same worker
// serialize there instead of overwriting each other's job.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef ENABLE_LEANSTORE
#include <leanstore/KVInterface.hpp>
#include <leanstore/LeanStore.hpp>
#include <leanstore/concurrency-recovery/Worker.hpp>
#include <leanstore/utils/JumpMU.hpp>
#endif

#include "rival_store_disabled_stub.hpp"

class LeanStoreStore {
 public:
  static constexpr bool kIsEnabled =
#ifdef ENABLE_LEANSTORE
      true;
#else
      false;
#endif

#ifdef ENABLE_LEANSTORE
  // Number of LeanStore worker threads owned by each instance. Matches the harness's maximum
  // benchmark thread count so every caller thread can spread onto its own worker; fewer caller
  // threads simply leave workers idle.
  static constexpr uint64_t kWorkerThreads = 32;
  // BTreeVI key/value lengths are u16. Keys here are short; values above this cannot be stored.
  static constexpr std::size_t kMaxValueBytes = 65535;

  static auto checked_len(std::size_t n) -> ::u16 {
    if (n > kMaxValueBytes) {
      throw std::runtime_error("LeanStore key/value over u16 ceiling");
    }
    return static_cast<::u16>(n);
  }

  static void check_value_size(std::size_t n) {
    if (n > kMaxValueBytes) {
      throw std::runtime_error("LeanStore cannot store values over 65535 bytes (64KB corpus excluded)");
    }
  }

  // BTreeVI orders keys by raw memcmp, mirroring LMDBStore::compare_to_bound exactly.
  static auto compare_to_bound(const ::u8 *key, ::u16 key_len,
                               std::span<const std::byte> upper_bound) noexcept -> int {
    const std::size_t klen = key_len;
    const size_t min_len = std::min(klen, upper_bound.size());
    const int cmp = min_len == 0 ? 0 : std::memcmp(key, upper_bound.data(), min_len);
    if (cmp != 0) {
      return cmp;
    }
    if (klen == upper_bound.size()) {
      return 0;
    }
    return klen < upper_bound.size() ? -1 : 1;
  }

  // Exact-match existence probe with optional value capture. Scan-based rather than
  // lookup()-based (see get_impl): only the scan path skips removed entries. ABORT_TX
  // funnels through abortTX() for the caller's retry loop.
  template <typename ValueCb>
  static void scan_exact(leanstore::KVInterface &btree, std::span<const std::byte> key, ValueCb &&on_value,
                         bool &found) {
    ::u8 *k = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(key.data()));
    const ::u16 klen = checked_len(key.size());
    const auto res = btree.scanAsc(
        k, klen,
        [&](const ::u8 *sk, ::u16 sklen, const ::u8 *sv, ::u16 svlen) {
          if (sklen != klen || std::memcmp(sk, key.data(), klen) != 0) {
            return false;
          }
          on_value(sv, svlen);
          found = true;
          return false;
        },
        [] {});
    if (res == leanstore::OP_RESULT::ABORT_TX) {
      leanstore::cr::Worker::my().abortTX();
    }
  }

  // Opens a fresh database at a unique subpath, mirroring LMDBStore's per-instance uniquing.
  explicit LeanStoreStore(std::string path) {
    static std::atomic<uint64_t> instance_counter{0};
    const std::string stem =
        std::move(path) + "_" + std::to_string(instance_counter.fetch_add(1, std::memory_order_relaxed)) + ".leanstore";
    open_fresh(stem);
  }

  ~LeanStoreStore() {
    close();
    std::error_code ignored;
    std::filesystem::remove(ssd_path_, ignored);
    std::filesystem::remove(json_path_, ignored);
  }

  LeanStoreStore(const LeanStoreStore &) = delete;
  auto operator=(const LeanStoreStore &) -> LeanStoreStore & = delete;

  // Tag type selecting the clone-from-master constructor below. Public so StoreAdapter's
  // variadic forwarding constructor can name it directly.
  struct CloneFromMasterTag {};

  // Builds `master_path` once via bulk_load, then clones it into this instance's own path with
  // a plain file copy -- much cheaper than re-running bulk_load_impl against an empty
  // environment each time. Must be a real copy, not a hardlink: LeanStore writes its B-Tree
  // pages in place, so a hardlinked clone and its master would corrupt each other on the
  // first write (same reasoning as LMDBStore's clone_from()).
  template <typename KeyFn, typename ValueFn>
  LeanStoreStore(CloneFromMasterTag /*tag*/,
                 const std::string &master_path,
                 std::size_t key_count,
                 KeyFn &&make_key,
                 ValueFn &&make_value) {
    ensure_master_built(master_path, key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
    // Fixed path, not an ever-incrementing counter: every clone of the same master is
    // interchangeable, and a fixed name keeps at most one clone generation on disk.
    const std::string stem = master_path + "_clone.leanstore";
    clone_from(master_path, stem);
    open_recover(stem);
  }

  // No-op: LeanStore reclaims via its buffer manager/page recycling internally; there is no
  // manual reorganize/compaction concept during normal operation.
  void reorganize() {}

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  template <typename Callback>
  auto get_impl(std::span<const std::byte> key, Callback callback) const -> bool {
    // Neither primitive alone is correct on this backend version (verified empirically):
    // lookupOptimistic returns the head payload without consulting is_removed (deleted keys
    // read as live), while the scan fast path overstates chained value lengths by
    // sizeof(ChainedTuple) (correct pointer, length includes the header). Hence liveness via
    // scan_exact plus the value via lookup; both inside one TX.
    // The value is copied into heap state before commit (a thread_local buffer would belong
    // to the worker thread, not the caller).
    struct GetState {
      std::vector<std::byte> value;
      bool found = false;
    };
    auto state = run_on_worker<GetState>(
        [&](leanstore::KVInterface &btree) {
          GetState out;
          bool live = false;
          scan_exact(btree, key, [&](const ::u8 *, ::u16) {}, live);
          if (!live) {
            return out;
          }
          ::u8 *k = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(key.data()));
          const auto res = btree.lookup(k, checked_len(key.size()),
                                        [&](const ::u8 *payload, ::u16 len) {
                                          out.value.assign(reinterpret_cast<const std::byte *>(payload),
                                                           reinterpret_cast<const std::byte *>(payload) + len);
                                        });
          if (res == leanstore::OP_RESULT::ABORT_TX) {
            leanstore::cr::Worker::my().abortTX();
          }
          // A concurrent remover may have deleted the key between the probe and the lookup;
          // linearize the delete first.
          out.found = (res == leanstore::OP_RESULT::OK);
          return out;
        },
        /*read_only=*/true);
    if (state.found) {
      callback(std::span<const std::byte>(state.value.data(), state.value.size()));
    }
    return state.found;
  }

  auto insert_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    check_value_size(value.size());
    // BTreeVI::insert on an existing key hits an unimplemented path, so existence is checked
    // first within the same TX: a concurrent inserter wins the race as ABORT_TX and this call
    // retries, observing the key the second time.
    return run_on_worker<bool>([&](leanstore::KVInterface &btree) {
      ::u8 *k = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(key.data()));
      ::u8 *v = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(value.data()));
      bool seen = false;
      scan_exact(btree, key, [&](const ::u8 *, ::u16) {}, seen);
      if (seen) {
        return false;
      }
      const auto res = btree.insert(k, checked_len(key.size()), v, checked_len(value.size()));
      if (res == leanstore::OP_RESULT::ABORT_TX) {
        leanstore::cr::Worker::my().abortTX();
      }
      return res == leanstore::OP_RESULT::OK;
    });
  }

  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    check_value_size(value.size());
    // Same-size in-place update: remove + insert is unimplemented in this backend version
    // ("Implement inserts after remove cases" TODO in BTreeVI), so size-changing updates have
    // no correct path. The harness always updates with the key's own index-derived size, hence
    // same-size; anything else fails fast instead of hitting ensure(false) inside the engine.
    return run_on_worker<bool>([&](leanstore::KVInterface &btree) {
      ::u8 *k = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(key.data()));
      // Liveness via scan (lookup is tombstone-blind); the exact length via lookup (the scan
      // fast path overstates chained value lengths). Both in one TX.
      ::u16 old_len = 0;
      bool seen = false;
      scan_exact(
          btree, key, [&](const ::u8 *, ::u16) {}, seen);
      if (!seen) {
        return false;
      }
      const auto lres = btree.lookup(k, checked_len(key.size()), [&](const ::u8 *, ::u16 len) { old_len = len; });
      if (lres == leanstore::OP_RESULT::ABORT_TX) {
        leanstore::cr::Worker::my().abortTX();
      }
      if (lres != leanstore::OP_RESULT::OK) {
        return false;
      }
      if (old_len != value.size()) {
        throw std::runtime_error("LeanStore update with changed value size is unsupported by the engine");
      }
      ::u8 desc_buf[sizeof(leanstore::UpdateSameSizeInPlaceDescriptor) +
                    sizeof(leanstore::UpdateSameSizeInPlaceDescriptor::Slot)];
      auto &desc = *reinterpret_cast<leanstore::UpdateSameSizeInPlaceDescriptor *>(desc_buf);
      desc.count = 1;
      desc.slots[0].offset = 0;
      desc.slots[0].length = checked_len(value.size());
      const auto res = btree.updateSameSizeInPlace(
          k, checked_len(key.size()),
          [&](::u8 *payload, ::u16) {
            std::memcpy(payload, value.data(), value.size());
          },
          desc);
      if (res == leanstore::OP_RESULT::ABORT_TX) {
        leanstore::cr::Worker::my().abortTX();
      }
      return res == leanstore::OP_RESULT::OK;
    });
  }

  auto remove_impl(std::span<const std::byte> key) -> bool {
    // Existence is checked first within the same TX: removing an already-removed key hits an
    // engine ensure() instead of returning NOT_FOUND.
    return run_on_worker<bool>([&](leanstore::KVInterface &btree) {
      ::u8 *k = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(key.data()));
      bool seen = false;
      scan_exact(btree, key, [&](const ::u8 *, ::u16) {}, seen);
      if (!seen) {
        return false;
      }
      const auto res = btree.remove(k, checked_len(key.size()));
      if (res == leanstore::OP_RESULT::ABORT_TX) {
        leanstore::cr::Worker::my().abortTX();
      }
      return res == leanstore::OP_RESULT::OK;
    });
  }

  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    if (key_count == 0) {
      return;
    }
    // No bulk-insert fast path exists in this backend version, so load goes through regular
    // transactions: strided across loader threads (content is index-derived, hence identical
    // regardless of insertion order) with a commit every batch to bound TX WAL size. Each key
    // probes before inserting: a batch that aborts mid-way retries the whole chunk, and only
    // the probe makes that retry idempotent (blind re-insert would hit the engine's
    // duplicate path).
    constexpr std::size_t kBatchKeys = 10000;
    const auto loader_threads = static_cast<unsigned>(
        std::min<std::size_t>(worker_threads(), std::max<std::size_t>(1, key_count / kBatchKeys)));
    std::atomic<bool> failed{false};
    std::vector<std::thread> loaders;
    for (unsigned t = 0; t < loader_threads; ++t) {
      loaders.emplace_back([&, t] {
        for (std::size_t base = t; base < key_count && !failed.load(std::memory_order_relaxed);) {
          const std::size_t chunk_end = std::min(key_count, base + kBatchKeys * loader_threads);
          run_on_worker<int>(
              [&](leanstore::KVInterface &btree) {
                for (std::size_t index = base; index < chunk_end; index += loader_threads) {
                  const std::string key = make_key(index);
                  const std::string value = make_value(index);
                  if (value.size() > kMaxValueBytes) {
                    failed.store(true, std::memory_order_relaxed);
                    return 0;
                  }
                  bool present = false;
                  scan_exact(
                      btree, std::span<const std::byte>(reinterpret_cast<const std::byte *>(key.data()), key.size()),
                      [&](const ::u8 *, ::u16) {}, present);
                  if (present) {
                    continue;
                  }
                  const auto res = btree.insert(reinterpret_cast<::u8 *>(const_cast<char *>(key.data())),
                                                checked_len(key.size()),
                                                reinterpret_cast<::u8 *>(const_cast<char *>(value.data())),
                                                checked_len(value.size()));
                  if (res == leanstore::OP_RESULT::ABORT_TX) {
                    leanstore::cr::Worker::my().abortTX();
                  }
                }
                return 0;
              },
              /*read_only=*/false, static_cast<uint64_t>(t));
          base = chunk_end;
        }
      });
    }
    for (auto &th : loaders) {
      th.join();
    }
    if (failed.load()) {
      throw std::runtime_error("LeanStore bulk load failed (value over u16 ceiling or backend error)");
    }
  }

  template <typename Cb>
  [[nodiscard]] auto scan_impl(std::span<const std::byte> lower_bound,
                               std::span<const std::byte> upper_bound,
                               Cb callback) const -> size_t {
    // Values resolve through lookup, not the scan callback (see get_impl). Two phases in one
    // TX: collect in-range keys first, then look each up. The lookup must not nest inside the
    // scan callback (latch-order risk across leaves); sequential phases hold no iterator
    // latch while looking up.
    struct ScanState {
      std::vector<std::string> keys;
      size_t count = 0;
    };
    auto state = run_on_worker<ScanState>(
        [&](leanstore::KVInterface &btree) {
          ScanState out;
          ::u8 *lo = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(lower_bound.data()));
          const auto res = btree.scanAsc(
              lo, checked_len(lower_bound.size()),
              [&](const ::u8 *sk, ::u16 sklen, const ::u8 *, ::u16) {
                if (compare_to_bound(sk, sklen, upper_bound) > 0) {
                  return false;
                }
                out.keys.emplace_back(reinterpret_cast<const char *>(sk), sklen);
                return true;
              },
              [] {});
          if (res == leanstore::OP_RESULT::ABORT_TX) {
            leanstore::cr::Worker::my().abortTX();
          }
          std::vector<std::byte> value_buf;
          for (const std::string &key : out.keys) {
            value_buf.clear();
            bool live = false;
            ::u8 *lk = const_cast<::u8 *>(reinterpret_cast<const ::u8 *>(key.data()));
            const auto lres = btree.lookup(lk, checked_len(key.size()),
                                           [&](const ::u8 *payload, ::u16 len) {
                                             value_buf.assign(reinterpret_cast<const std::byte *>(payload),
                                                              reinterpret_cast<const std::byte *>(payload) + len);
                                             live = true;
                                           });
            if (lres == leanstore::OP_RESULT::ABORT_TX) {
              leanstore::cr::Worker::my().abortTX();
            }
            if (!live) {
              continue;
            }
            callback(std::span<const std::byte>(reinterpret_cast<const std::byte *>(key.data()), key.size()),
                     std::span<const std::byte>(value_buf.data(), value_buf.size()));
            ++out.count;
          }
          return out;
        },
        /*read_only=*/true);
    return state.count;
  }

 private:
  // ─── Instance lifecycle ───

  static auto dram_gib() -> double {
    if (const char *env = std::getenv("LEANSTORE_DRAM_GIB")) {
      return std::strtod(env, nullptr);
    }
    // Mirror the harness's memory posture: constrained under LTM, generous in-memory.
    // VMEMKV_BENCH_LTM=1 is set by benchmark/common/benchmark_matrix.sh for LTM scenarios.
    if (std::getenv("VMEMKV_BENCH_LTM") != nullptr) {
      return 1.0;
    }
    return 16.0;
  }

  static auto worker_threads() -> uint32_t {
    if (const char *env = std::getenv("LEANSTORE_WORKER_THREADS")) {
      // Clamped to the slot-mutex array bound; fewer workers only (bisect/debug knob).
      return std::min(static_cast<uint32_t>(kWorkerThreads), static_cast<uint32_t>(std::strtoul(env, nullptr, 10)));
    }
    return static_cast<uint32_t>(kWorkerThreads);
  }

  // LeanStore derives recover/persist from non-default file paths at construction
  // (recover_file != default forces recover), so every flag is reset explicitly on each open:
  // bench/test processes construct many instances sequentially sharing these globals.
  static void reset_flags(const std::string &ssd) {
    FLAGS_ssd_path = ssd;
    FLAGS_trunc = false;
    FLAGS_persist = false;
    FLAGS_persist_file = "./leanstore.json";
    FLAGS_recover = false;
    FLAGS_recover_file = "./leanstore.json";
    FLAGS_wal = true;
    FLAGS_wal_fsync = true;
    FLAGS_worker_threads = worker_threads();
    FLAGS_dram_gib = dram_gib();
    FLAGS_pin_threads = false;
    FLAGS_cpu_counters = false;
    if (const char *pp = std::getenv("LEANSTORE_PP_THREADS")) {
      FLAGS_pp_threads = static_cast<uint32_t>(std::strtoul(pp, nullptr, 10));
    }
  }

  // Blocks until every worker has executed one job. Construction returns while workers
  // are still starting otherwise, and a fast destroy then races their startup barrier against
  // the teardown spin (hang). A completed round trip proves all workers -- and the group
  // committer they rendezvous with -- are past startup.
  void warmup_workers() {
    for (uint64_t t = 0; t < worker_threads(); ++t) {
      db_->getCRManager().scheduleJobSync(t, [] {});
    }
  }

  void open_fresh(const std::string &stem) {
    ssd_path_ = stem;
    json_path_ = stem + ".json";
    std::error_code ignored;
    std::filesystem::remove(ssd_path_, ignored);
    std::filesystem::remove(json_path_, ignored);
    reset_flags(ssd_path_);
    FLAGS_trunc = true;
    db_ = std::make_unique<leanstore::LeanStore>();
    // Table registration runs on a worker (as in upstream drivers): B-tree creation touches
    // worker-local state and segfaults on a foreign thread.
    db_->getCRManager().scheduleJobSync(0, [&] {
      table_ = &db_->registerBTreeVI("kv", {.enable_wal = true, .use_bulk_insert = false});
    });
    warmup_workers();
  }

  void open_recover(const std::string &stem) {
    ssd_path_ = stem;
    json_path_ = stem + ".json";
    reset_flags(ssd_path_);
    FLAGS_recover = true;
    FLAGS_recover_file = json_path_;
    db_ = std::make_unique<leanstore::LeanStore>();
    db_->getCRManager().scheduleJobSync(0, [&] { table_ = &db_->retrieveBTreeVI("kv"); });
    warmup_workers();
  }

  void close() {
    table_ = nullptr;
    db_.reset();
  }

  // Builds a fresh master at a ".building" sibling path, closed (persist flushes all buffer
  // frames, so the file copy below is self-consistent) before anything copies it, so a crash
  // mid-build never leaves a partial master for a later call to trust. WAL is disabled for
  // the build itself, like the other engines' non-durable bulk loaders.
  template <typename KeyFn, typename ValueFn>
  static void ensure_master_built(const std::string &master_path,
                                  std::size_t key_count,
                                  KeyFn &&make_key,
                                  ValueFn &&make_value) {
    const std::string master_json = master_path + ".json";
    if (std::filesystem::exists(master_path) && std::filesystem::exists(master_json)) {
      return;
    }
    const std::string building_ssd = master_path + ".building";
    const std::string building_json = building_ssd + ".json";
    std::error_code ignored;
    std::filesystem::remove(building_ssd, ignored);
    std::filesystem::remove(building_json, ignored);

    LeanStoreStore tmp;
    tmp.ssd_path_ = building_ssd;
    tmp.json_path_ = building_json;
    reset_flags(building_ssd);
    FLAGS_trunc = true;
    FLAGS_persist = true;
    FLAGS_persist_file = building_json;
    tmp.db_ = std::make_unique<leanstore::LeanStore>();
    tmp.db_->getCRManager().scheduleJobSync(0, [&] {
      tmp.table_ = &tmp.db_->registerBTreeVI("kv", {.enable_wal = false, .use_bulk_insert = false});
    });
    tmp.warmup_workers();
    tmp.bulk_load_impl(key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
    tmp.close();

    std::filesystem::remove(master_path, ignored);
    std::filesystem::remove(master_json, ignored);
    std::error_code rename_error;
    std::filesystem::rename(building_ssd, master_path, rename_error);
    if (rename_error) {
      throw std::runtime_error("LeanStore master build rename failed: " + rename_error.message());
    }
    std::filesystem::rename(building_json, master_json, rename_error);
    if (rename_error) {
      throw std::runtime_error("LeanStore master json rename failed: " + rename_error.message());
    }
  }

  static void clone_from(const std::string &source_stem, const std::string &dest_stem) {
    std::error_code ignored;
    std::filesystem::remove(dest_stem, ignored);
    std::filesystem::remove(dest_stem + ".json", ignored);
    std::filesystem::copy_file(source_stem, dest_stem, ignored);
    if (ignored) {
      throw std::runtime_error("LeanStore clone copy failed: " + ignored.message());
    }
    std::filesystem::copy_file(source_stem + ".json", dest_stem + ".json", ignored);
    if (ignored) {
      throw std::runtime_error("LeanStore clone json copy failed: " + ignored.message());
    }
  }

  // ─── Worker-dispatched transactions ───

  auto worker_slot() const -> uint64_t {
    return std::hash<std::thread::id>{}(std::this_thread::get_id()) % worker_threads();
  }

  // Runs `body` inside a single TX on a LeanStore worker and returns its value, retrying
  // internally on abort. ABORT_TX from any backend call must funnel through abortTX() (which
  // longjmps into the catch below). Results travel in heap state, never in setjmp-crossing
  // locals.
  template <typename T, typename Body>
  auto run_on_worker(Body &&body, bool read_only, uint64_t slot_hint) const -> T {
    struct Slot {
      T value{};
      bool done = false;
    };
    auto state = std::make_unique<Slot>();
    Slot *s = state.get();
    std::lock_guard<std::mutex> slot_lock(slot_mutex_[slot_hint]);
    db_->getCRManager().scheduleJobSync(slot_hint, [&] {
      while (!s->done) {
        jumpmuTry() {
          leanstore::cr::Worker::my().startTX(leanstore::TX_MODE::OLTP,
                                              leanstore::TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION, read_only);
          s->value = body(*table_);
          leanstore::cr::Worker::my().commitTX();
          s->done = true;
        }
        jumpmuCatch() {}
      }
    });
    return std::move(state->value);
  }

  template <typename T, typename Body>
  auto run_on_worker(Body &&body, bool read_only = false) const -> T {
    return run_on_worker<T>(std::forward<Body>(body), read_only, worker_slot());
  }

  template <typename T, typename Body>
  auto run_on_worker(Body &&body, uint64_t slot_hint) const -> T {
    return run_on_worker<T>(std::forward<Body>(body), /*read_only=*/false, slot_hint);
  }

  // Private default ctor for ensure_master_built()'s scratch instance.
  LeanStoreStore() = default;

  std::unique_ptr<leanstore::LeanStore> db_;
  leanstore::KVInterface *table_ = nullptr;
  mutable std::mutex slot_mutex_[kWorkerThreads];
  std::string ssd_path_;
  std::string json_path_;
#else
  VMEMKV_RIVAL_DISABLED_STUB(LeanStoreStore, "LeanStore")
#endif
};
