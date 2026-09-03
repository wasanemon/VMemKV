// rocksdb_engine_store.hpp — Shared wiring for RocksDBStore/RocksDBBlobDBStore: both rivals wire
// up construction/cloning/impl-method delegation identically, differing only in engine label and
// rocksdb::Options -- see each concrete Policy struct (in rocksdb_store.hpp/
// rocksdb_blobdb_store.hpp) for what actually varies.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef ENABLE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#endif

#include "rocksdb_common.hpp"

namespace vmemkv::rivals {

// `Policy` supplies: kLabel, kCloneLabel (both `const char *`), and `static auto
// make_options() -> rocksdb::Options` (only required/compiled when ENABLE_ROCKSDB is defined).
template <typename Policy>
class RocksDBEngineStore {
 public:
  static constexpr bool kIsEnabled =
#ifdef ENABLE_ROCKSDB
      true;
#else
      false;
#endif

#ifdef ENABLE_ROCKSDB
  // Opens a fresh DB at a unique subpath, preventing transient lock contention (ENOLCK) across thread sweeps.
  explicit RocksDBEngineStore(std::string path) {
    static std::atomic<uint64_t> instance_counter{0};
    path_ = std::move(path) + "_" + std::to_string(instance_counter.fetch_add(1, std::memory_order_relaxed));
    rocksdb::DestroyDB(path_, {});
    db_.reset(rocksdb_common::open_db(Policy::make_options(), path_, Policy::kLabel));
  }

  // Closes and destroys the DB (cleans up temp files in bench/test usage).
  ~RocksDBEngineStore() {
    db_.reset();
    rocksdb::DestroyDB(path_, {});
  }

  RocksDBEngineStore(const RocksDBEngineStore &) = delete;
  auto operator=(const RocksDBEngineStore &) -> RocksDBEngineStore & = delete;

  // Tag type selecting the clone-from-master constructor below. Public so StoreAdapter's
  // variadic forwarding constructor can name it directly -- see
  // for_each_store_variant()'s make_fresh_corpus()/make_fresh_corpus_checkpoint() in bench_kv.cpp.
  struct CloneFromMasterTag {};

  // Builds `master_path` once via bulk_load, then clones it via RocksDB's Checkpoint API -- see
  // rocksdb_common::clone_from()'s comment for why this is cheap and safe.
  template <typename KeyFn, typename ValueFn>
  RocksDBEngineStore(CloneFromMasterTag /*tag*/,
                     const std::string &master_path,
                     std::size_t key_count,
                     KeyFn &&make_key,
                     ValueFn &&make_value) {
    rocksdb_common::ensure_master_built(Policy::make_options(),
                                        master_path,
                                        key_count,
                                        std::forward<KeyFn>(make_key),
                                        std::forward<ValueFn>(make_value),
                                        Policy::kLabel);
    // Fixed path, not an ever-incrementing counter -- see bench_kv.cpp's make_vmemkv_clone_from_
    // checkpoint() for why this is safe (at most one clone of a given master is ever live
    // process-wide) and necessary (an unbounded counter accumulates a fresh multi-GB clone per
    // benchmark case instead of reusing one). clone_from() below already destroys any stale
    // directory at this path before rebuilding it.
    path_ = master_path + "_clone";
    rocksdb_common::clone_from(Policy::make_options(), master_path, path_, Policy::kLabel);
    db_.reset(rocksdb_common::open_db(Policy::make_options(), path_, Policy::kCloneLabel));
  }

  // No-op: RocksDB self-compacts (including blob garbage collection during compaction, for the
  // BlobDB policy).
  void reorganize() {}

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  template <typename Callback>
  auto get_impl(std::span<const std::byte> key, Callback callback) const -> bool {
    return rocksdb_common::get_from_db(db_.get(), key, callback);
  }

  auto insert_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    return rocksdb_common::insert_into_db(db_.get(), key, value);
  }

  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    return rocksdb_common::update_in_db(db_.get(), key, value);
  }

  auto remove_impl(std::span<const std::byte> key) -> bool { return rocksdb_common::remove_from_db(db_.get(), key); }

  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    rocksdb_common::bulk_load_into(
        db_.get(), key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value), Policy::kLabel);
  }

  template <typename Cb>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] auto scan_impl(std::span<const std::byte> lower_bound,
                               std::span<const std::byte> upper_bound,
                               Cb callback) const -> size_t {
    return rocksdb_common::scan_db(db_.get(), lower_bound, upper_bound, callback);
  }

 private:
  std::unique_ptr<rocksdb::DB> db_;
  std::string path_;
#else
  // Same "backend not compiled in" stub surface as VMEMKV_RIVAL_DISABLED_STUB
  // (rival_store_disabled_stub.hpp) -- written directly rather than via that macro since the
  // macro's error-message text requires a preprocessor string-literal token, which `Policy::kLabel`
  // (a runtime-visible member, needed since this class is shared across two engine labels) isn't.
  explicit RocksDBEngineStore(const std::string &unused_path) {
    (void)unused_path;
    throw std::runtime_error(std::string(Policy::kLabel) + " not enabled in this build");
  }
  ~RocksDBEngineStore() = default;
  RocksDBEngineStore(const RocksDBEngineStore &) = delete;
  auto operator=(const RocksDBEngineStore &) -> RocksDBEngineStore & = delete;
  struct CloneFromMasterTag {};
  template <typename KeyFn, typename ValueFn>
  RocksDBEngineStore(CloneFromMasterTag /*tag*/,
                     const std::string &master_path,
                     std::size_t key_count,
                     KeyFn &&make_key,
                     ValueFn &&make_value) {
    (void)master_path;
    (void)key_count;
    (void)make_key;
    (void)make_value;
    throw std::runtime_error(std::string(Policy::kLabel) + " not enabled in this build");
  }
  void reorganize() {}
  template <typename Callback>
  auto get_impl(std::span<const std::byte> key, Callback callback) const -> bool {
    (void)key;
    (void)callback;
    return false;
  }
  auto insert_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    (void)key;
    (void)value;
    return false;
  }
  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    (void)key;
    (void)value;
    return false;
  }
  auto remove_impl(std::span<const std::byte> key) -> bool {
    (void)key;
    return false;
  }
  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    (void)key_count;
    (void)make_key;
    (void)make_value;
    throw std::runtime_error(std::string(Policy::kLabel) + " not enabled in this build");
  }
  template <typename Cb>
  auto scan_impl(std::span<const std::byte> low, std::span<const std::byte> high, Cb callback) const -> size_t {
    (void)low;
    (void)high;
    (void)callback;
    return 0;
  }
#endif
};

}  // namespace vmemkv::rivals
