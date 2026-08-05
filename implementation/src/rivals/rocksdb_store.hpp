// rocksdb_store.hpp — Thin RocksDB wrapper exposing byte-span APIs for comparison.
//
// Thread safety: RocksDB is internally thread-safe.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef ENABLE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/write_batch.h>
#endif

#include "../t1_index/t1_index.hpp"  // For STORE_NOT_FOUND definition

class RocksDBStore {
 public:
  static constexpr bool kIsEnabled =
#ifdef ENABLE_ROCKSDB
      true;
#else
      false;
#endif

#ifdef ENABLE_ROCKSDB
  // Opens a fresh DB at a unique subpath, preventing transient lock contention (ENOLCK) across thread sweeps.
  explicit RocksDBStore(std::string path) {
    static std::atomic<uint64_t> instance_counter{0};
    path_ = std::move(path) + "_" + std::to_string(instance_counter.fetch_add(1, std::memory_order_relaxed));

    rocksdb::DestroyDB(path_, {});
    rocksdb::Options opts = make_benchmark_db_options();
    rocksdb::DB *db_handle = nullptr;
    auto status = rocksdb::DB::Open(opts, path_, &db_handle);
    if (!status.ok()) {
      throw std::runtime_error("RocksDB Open failed: " + status.ToString());
    }
    db_.reset(db_handle);
  }

  // Closes and destroys the DB (cleans up temp files in bench/test usage).
  ~RocksDBStore() {
    db_.reset();
    rocksdb::DestroyDB(path_, {});
  }

  RocksDBStore(const RocksDBStore &) = delete;
  auto operator=(const RocksDBStore &) -> RocksDBStore & = delete;

  // Tag type selecting the clone-from-master constructor below. Public so StoreAdapter's
  // variadic forwarding constructor can name it directly -- see
  // for_each_store_variant()'s make_fresh_corpus()/make_fresh_corpus_checkpoint() in bench_kv.cpp.
  struct CloneFromMasterTag {};

  // Builds `master_path` once via bulk_load, then clones it via RocksDB's Checkpoint API --
  // much cheaper than re-running bulk_load_impl against an empty DB each time. Checkpoint
  // hardlinks unchanged SST files instead of copying bytes, and this is safe because SST files
  // are immutable once written: compaction produces new files rather than editing a shared
  // hardlink in place, so writes against a clone can never corrupt the master or a sibling clone.
  template <typename KeyFn, typename ValueFn>
  RocksDBStore(CloneFromMasterTag /*tag*/,
               const std::string &master_path,
               std::size_t key_count,
               KeyFn &&make_key,
               ValueFn &&make_value) {
    ensure_master_built(master_path, key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
    static std::atomic<uint64_t> clone_counter{0};
    path_ = master_path + "_clone_" + std::to_string(clone_counter.fetch_add(1, std::memory_order_relaxed));
    clone_from(master_path, path_);
    rocksdb::Options opts = make_benchmark_db_options();
    rocksdb::DB *db_handle = nullptr;
    auto status = rocksdb::DB::Open(opts, path_, &db_handle);
    if (!status.ok()) {
      throw std::runtime_error("RocksDB Open (clone) failed: " + status.ToString());
    }
    db_.reset(db_handle);
  }

  // No-op: RocksDB self-compacts.
  void reorganize() {}

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  template <typename Callback>
  auto get_impl(std::span<const std::byte> key, Callback callback) const -> bool {
    rocksdb::PinnableSlice pinned_value;
    auto status = db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pinned_value);
    if (!status.ok()) {
      return false;
    }
    std::span<const std::byte> val_span(reinterpret_cast<const std::byte *>(pinned_value.data()), pinned_value.size());
    callback(val_span);
    return true;
  }

  auto insert_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    rocksdb::PinnableSlice pinned_value;
    if (db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pinned_value).ok()) {
      return false;  // already exists
    }
    return db_->Put(make_durable_write_options(), to_slice(key), to_slice(value)).ok();
  }

  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    rocksdb::PinnableSlice pinned_value;
    if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pinned_value).ok()) {
      return false;  // not found
    }
    return db_->Put(make_durable_write_options(), to_slice(key), to_slice(value)).ok();
  }

  auto remove_impl(std::span<const std::byte> key) -> bool {
    rocksdb::PinnableSlice pinned_value;
    if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pinned_value).ok()) {
      return false;  // not found
    }
    return db_->Delete(make_durable_write_options(), to_slice(key)).ok();
  }

  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    bulk_load_into(db_.get(), key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
  }

  template <typename KeyFn, typename ValueFn>
  static void bulk_load_into(rocksdb::DB *db, std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    if (key_count == 0) {
      return;
    }
    // Keep each WriteBatch within a fixed byte budget so many small values can be grouped
    // much more aggressively than few large ones without needing to know sizes up front.
    constexpr std::size_t kBatchBytes = 64ULL * 1024ULL * 1024ULL;
    rocksdb::WriteOptions write_opts = make_bulk_load_write_options();

    std::size_t next_index = 0;
    while (next_index < key_count) {
      rocksdb::WriteBatch batch;
      std::size_t batch_bytes = 0;
      std::size_t index = next_index;
      for (; index < key_count && batch_bytes < kBatchBytes; ++index) {
        const std::string key = make_key(index);
        const std::string value = make_value(index);
        batch_bytes += key.size() + value.size();
        batch.Put(key, value);
      }

      auto status = db->Write(write_opts, &batch);
      if (!status.ok()) {
        throw std::runtime_error("RocksDB WriteBatch failed: " + status.ToString());
      }
      next_index = index;
    }
  }

  template <typename Cb>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] auto scan_impl(std::span<const std::byte> lower_bound,
                               std::span<const std::byte> upper_bound,
                               Cb callback) const -> size_t {
    auto iterator = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator({}));
    rocksdb::Slice lower_bound_slice = to_slice(lower_bound);
    rocksdb::Slice upper_bound_slice = to_slice(upper_bound);
    size_t count = 0;
    for (iterator->Seek(lower_bound_slice); iterator->Valid() && iterator->key().compare(upper_bound_slice) <= 0;
         iterator->Next()) {
      callback(to_bytes(iterator->key()), to_bytes(iterator->value()));
      ++count;
    }
    return count;
  }

 private:
  // Builds a fresh master DB at a ".building" sibling path, renamed into place only once fully
  // populated and closed, so a crash mid-build never leaves a partial master for a later call
  // to trust.
  template <typename KeyFn, typename ValueFn>
  static void ensure_master_built(const std::string &master_path,
                                  std::size_t key_count,
                                  KeyFn &&make_key,
                                  ValueFn &&make_value) {
    if (std::filesystem::exists(master_path)) {
      return;
    }
    const std::string building_path = master_path + ".building";
    rocksdb::DestroyDB(building_path, {});
    rocksdb::DB *db_handle = nullptr;
    auto status = rocksdb::DB::Open(make_benchmark_db_options(), building_path, &db_handle);
    if (!status.ok()) {
      throw std::runtime_error("RocksDB Open (master build) failed: " + status.ToString());
    }
    bulk_load_into(db_handle, key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
    delete db_handle;
    rocksdb::DestroyDB(master_path, {});
    std::error_code rename_error;
    std::filesystem::rename(building_path, master_path, rename_error);
    if (rename_error) {
      throw std::runtime_error("RocksDB master build rename failed: " + rename_error.message());
    }
  }

  // Briefly opens `source_path` read-only, checkpoints it to `dest_path` (destroying any stale
  // directory already there), then closes the read-only handle -- see the CloneFromMasterTag
  // constructor's comment above for why this is fast and safe.
  static void clone_from(const std::string &source_path, const std::string &dest_path) {
    rocksdb::DB *source_db = nullptr;
    auto status = rocksdb::DB::OpenForReadOnly(make_benchmark_db_options(), source_path, &source_db);
    if (!status.ok()) {
      throw std::runtime_error("RocksDB OpenForReadOnly (clone source) failed: " + status.ToString());
    }
    rocksdb::Checkpoint *checkpoint = nullptr;
    status = rocksdb::Checkpoint::Create(source_db, &checkpoint);
    if (!status.ok()) {
      delete source_db;
      throw std::runtime_error("RocksDB Checkpoint::Create failed: " + status.ToString());
    }
    rocksdb::DestroyDB(dest_path, {});
    status = checkpoint->CreateCheckpoint(dest_path);
    delete checkpoint;
    delete source_db;
    if (!status.ok()) {
      throw std::runtime_error("RocksDB CreateCheckpoint failed: " + status.ToString());
    }
  }

  static auto make_benchmark_db_options() -> rocksdb::Options {
    rocksdb::Options opts;
    opts.create_if_missing = true;
    // Benchmark tuning lives here so the wrapper API stays separate from the
    // current population policy.
    opts.compression = rocksdb::kNoCompression;
    opts.write_buffer_size = 256ULL * 1024ULL * 1024ULL;
    opts.max_write_buffer_number = 16;
    opts.min_write_buffer_number_to_merge = 4;
    opts.target_file_size_base = 256ULL * 1024ULL * 1024ULL;
    opts.level_compaction_dynamic_level_bytes = true;
    opts.level0_file_num_compaction_trigger = 16;
    opts.level0_slowdown_writes_trigger = 128;
    opts.level0_stop_writes_trigger = 256;
    opts.max_background_jobs = 8;
    opts.max_subcompactions = 4;
    opts.max_open_files = 256;
    return opts;
  }

  static auto make_bulk_load_write_options() -> rocksdb::WriteOptions {
    rocksdb::WriteOptions opts;
    opts.disableWAL = true;
    return opts;
  }

  // Insert/Update/Delete must fsync the WAL before returning, matching VMemKV's own per-write
  // fsync contract (wal.hpp). RocksDB's default WriteOptions{} has sync=false (page-cache only),
  // which would understate RocksDB's cost relative to the durability level this project targets
  // (eventually backing a MySQL storage engine à la MyRocks, which pays the same commit-time
  // fsync).
  static auto make_durable_write_options() -> rocksdb::WriteOptions {
    rocksdb::WriteOptions opts;
    opts.sync = true;
    return opts;
  }

  static auto to_slice(std::span<const std::byte> key_bytes) noexcept -> rocksdb::Slice {
    return {reinterpret_cast<const char *>(key_bytes.data()), key_bytes.size()};
  }

  static auto to_bytes(const rocksdb::Slice &slice) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte *>(slice.data()), slice.size()};
  }

  std::unique_ptr<rocksdb::DB> db_;
  std::string path_;
#else
  // Dummy stub implementation when RocksDB is disabled.
  explicit RocksDBStore(const std::string &unused_path) {
    (void)unused_path;
    throw std::runtime_error("RocksDB not enabled in this build");
  }

  ~RocksDBStore() = default;

  RocksDBStore(const RocksDBStore &) = delete;
  auto operator=(const RocksDBStore &) -> RocksDBStore & = delete;

  struct CloneFromMasterTag {};
  template <typename KeyFn, typename ValueFn>
  RocksDBStore(CloneFromMasterTag /*tag*/,
               const std::string &master_path,
               std::size_t key_count,
               KeyFn &&make_key,
               ValueFn &&make_value) {
    (void)master_path;
    (void)key_count;
    (void)make_key;
    (void)make_value;
    throw std::runtime_error("RocksDB not enabled in this build");
  }

  void reorganize() {}

  template <typename Callback>
  auto get_impl(std::span<const std::byte> key, Callback callback) const -> bool {
    (void)key;
    (void)callback;
    return false;
  }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static,bugprone-easily-swappable-parameters)
  auto insert_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    (void)key;
    (void)value;
    return false;
  }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static,bugprone-easily-swappable-parameters)
  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    (void)key;
    (void)value;
    return false;
  }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  auto remove_impl(std::span<const std::byte> key) -> bool {
    (void)key;
    return false;
  }
  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    (void)key_count;
    (void)make_key;
    (void)make_value;
    throw std::runtime_error("RocksDB not enabled in this build");
  }

  template <typename Cb>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  auto scan_impl(std::span<const std::byte> low, std::span<const std::byte> high, Cb callback) const -> size_t {
    (void)low;
    (void)high;
    (void)callback;
    return 0;
  }
#endif
};
