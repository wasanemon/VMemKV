// rocksdb_blobdb_store.hpp — Thin RocksDB "BlobDB" wrapper exposing byte-span APIs
// for comparison.
//
// Uses RocksDB's integrated blob-file support (enable_blob_files), not the legacy standalone
// blob_db.h API (unavailable in this project's RocksDB version). Key-value separation trades
// write amplification for reduced compaction I/O on large values -- the interesting comparison
// point against plain RocksDBStore for the LTM(64KB) scenarios.
//
// Thread safety: RocksDB is internally thread-safe.

#pragma once

#include <atomic>
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

#include "rival_store_disabled_stub.hpp"
#include "rocksdb_common.hpp"

class RocksDBBlobDBStore {
 public:
  static constexpr bool kIsEnabled =
#ifdef ENABLE_ROCKSDB
      true;
#else
      false;
#endif

#ifdef ENABLE_ROCKSDB
  // Opens a fresh DB at a unique subpath, preventing transient lock contention (ENOLCK) across thread sweeps.
  explicit RocksDBBlobDBStore(std::string path) {
    static std::atomic<uint64_t> instance_counter{0};
    path_ = std::move(path) + "_" + std::to_string(instance_counter.fetch_add(1, std::memory_order_relaxed));
    rocksdb::DestroyDB(path_, {});
    db_.reset(vmemkv::rivals::rocksdb_common::open_db(make_benchmark_db_options(), path_, "RocksDB(BlobDB)"));
  }

  // Closes and destroys the DB (cleans up temp files in bench/test usage).
  ~RocksDBBlobDBStore() {
    db_.reset();
    rocksdb::DestroyDB(path_, {});
  }

  RocksDBBlobDBStore(const RocksDBBlobDBStore &) = delete;
  auto operator=(const RocksDBBlobDBStore &) -> RocksDBBlobDBStore & = delete;

  // Tag type selecting the clone-from-master constructor below. Public so StoreAdapter's
  // variadic forwarding constructor can name it directly -- see
  // for_each_store_variant()'s make_fresh_corpus()/make_fresh_corpus_checkpoint() in bench_kv.cpp.
  struct CloneFromMasterTag {};

  // See rocksdb_common::clone_from()'s comment -- same reasoning applies here.
  template <typename KeyFn, typename ValueFn>
  RocksDBBlobDBStore(CloneFromMasterTag /*tag*/,
                     const std::string &master_path,
                     std::size_t key_count,
                     KeyFn &&make_key,
                     ValueFn &&make_value) {
    namespace common = vmemkv::rivals::rocksdb_common;
    common::ensure_master_built(make_benchmark_db_options(),
                                master_path,
                                key_count,
                                std::forward<KeyFn>(make_key),
                                std::forward<ValueFn>(make_value),
                                "RocksDB(BlobDB)");
    static std::atomic<uint64_t> clone_counter{0};
    path_ = master_path + "_clone_" + std::to_string(clone_counter.fetch_add(1, std::memory_order_relaxed));
    common::clone_from(make_benchmark_db_options(), master_path, path_, "RocksDB(BlobDB)");
    db_.reset(common::open_db(make_benchmark_db_options(), path_, "RocksDB(BlobDB) (clone)"));
  }

  // No-op: RocksDB self-compacts (including blob garbage collection during compaction).
  void reorganize() {}

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  template <typename Callback>
  auto get_impl(std::span<const std::byte> key, Callback callback) const -> bool {
    return vmemkv::rivals::rocksdb_common::get_from_db(db_.get(), key, callback);
  }

  auto insert_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    return vmemkv::rivals::rocksdb_common::insert_into_db(db_.get(), key, value);
  }

  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    return vmemkv::rivals::rocksdb_common::update_in_db(db_.get(), key, value);
  }

  auto remove_impl(std::span<const std::byte> key) -> bool {
    return vmemkv::rivals::rocksdb_common::remove_from_db(db_.get(), key);
  }

  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    vmemkv::rivals::rocksdb_common::bulk_load_into(
        db_.get(), key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value), "RocksDB(BlobDB)");
  }

  template <typename Cb>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] auto scan_impl(std::span<const std::byte> lower_bound,
                               std::span<const std::byte> upper_bound,
                               Cb callback) const -> size_t {
    return vmemkv::rivals::rocksdb_common::scan_db(db_.get(), lower_bound, upper_bound, callback);
  }

 private:
  static auto make_benchmark_db_options() -> rocksdb::Options {
    rocksdb::Options opts = vmemkv::rivals::rocksdb_common::make_base_benchmark_db_options();
    // Values below this size stay inline in SST files; larger ones go to blob files.
    opts.enable_blob_files = true;
    opts.min_blob_size = 256;
    opts.blob_file_size = 256ULL * 1024ULL * 1024ULL;
    opts.blob_compression_type = rocksdb::kNoCompression;
    opts.enable_blob_garbage_collection = true;
    return opts;
  }

  std::unique_ptr<rocksdb::DB> db_;
  std::string path_;
#else
  VMEMKV_RIVAL_DISABLED_STUB(RocksDBBlobDBStore, "RocksDB(BlobDB)")
#endif
};
