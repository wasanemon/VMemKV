// rocksdb_blobdb_store.hpp — Thin RocksDB "BlobDB" wrapper exposing byte-span APIs
// for comparison.
//
// This uses RocksDB's modern integrated blob-file support (ColumnFamilyOptions::
// enable_blob_files and friends), not the legacy standalone rocksdb/utilities/
// blob_db.h stacked API, which is unavailable in the RocksDB version this project
// links against. Key-value separation (storing large values in separate blob files
// instead of inline in SST files) trades write amplification for reduced compaction
// I/O on large values, which is the interesting comparison point against plain
// RocksDBStore for the LTM(64KB) scenarios.
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
#include <rocksdb/write_batch.h>
#endif

#include "../t1_index/t1_index.hpp"  // For STORE_NOT_FOUND definition

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
    rocksdb::Options opts = make_benchmark_db_options();
    rocksdb::DB *db_handle = nullptr;
    auto status = rocksdb::DB::Open(opts, path_, &db_handle);
    if (!status.ok()) {
      throw std::runtime_error("RocksDB(BlobDB) Open failed: " + status.ToString());
    }
    db_.reset(db_handle);
  }

  // Closes and destroys the DB (cleans up temp files in bench/test usage).
  ~RocksDBBlobDBStore() {
    db_.reset();
    rocksdb::DestroyDB(path_, {});
  }

  RocksDBBlobDBStore(const RocksDBBlobDBStore &) = delete;
  auto operator=(const RocksDBBlobDBStore &) -> RocksDBBlobDBStore & = delete;

  // No-op: RocksDB self-compacts (including blob garbage collection during compaction).
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
    return db_->Put({}, to_slice(key), to_slice(value)).ok();
  }

  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    rocksdb::PinnableSlice pinned_value;
    if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pinned_value).ok()) {
      return false;  // not found
    }
    return db_->Put({}, to_slice(key), to_slice(value)).ok();
  }

  auto remove_impl(std::span<const std::byte> key) -> bool {
    rocksdb::PinnableSlice pinned_value;
    if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pinned_value).ok()) {
      return false;  // not found
    }
    return db_->Delete({}, to_slice(key)).ok();
  }

  template <typename KeyFn>
  auto bulk_load_impl(std::size_t key_count,
                      KeyFn &&make_key,
                      std::size_t target_value_size,
                      std::string *error_out = nullptr) -> bool {
    if (key_count == 0) {
      return true;
    }
    // Keep each WriteBatch within a fixed byte budget so 8B values can be
    // grouped much more aggressively than 64KB values without changing the
    // benchmark workload semantics.
    constexpr std::size_t kBatchBytes = 64ULL * 1024ULL * 1024ULL;
    rocksdb::WriteOptions write_opts = make_bulk_load_write_options();

    std::string dummy_large(target_value_size, 'a');
    std::string dummy_8b(8, 'a');
    const std::size_t batch_keys = std::max<std::size_t>(1, kBatchBytes / std::max<std::size_t>(1, target_value_size));

    std::size_t next_index = 0;
    while (next_index < key_count) {
      rocksdb::WriteBatch batch;
      const std::size_t chunk_end = std::min(key_count, next_index + batch_keys);
      for (std::size_t index = next_index; index < chunk_end; ++index) {
        const std::string key = make_key(index);
        if (target_value_size != 8 && index % 5 == 0) {
          batch.Put(key, dummy_8b);
        } else {
          batch.Put(key, dummy_large);
        }
      }

      auto status = db_->Write(write_opts, &batch);
      if (!status.ok()) {
        if (error_out != nullptr) {
          *error_out = "RocksDB(BlobDB) WriteBatch failed: " + status.ToString();
        }
        return false;
      }
      next_index = chunk_end;
    }
    return true;
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
  static auto make_benchmark_db_options() -> rocksdb::Options {
    rocksdb::Options opts;
    opts.create_if_missing = true;
    // Same base tuning as the plain RocksDBStore rival, plus blob-file (key-value
    // separation) settings so large values are stored in separate blob files
    // instead of inline in SST files during compaction.
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

    // Values below this size stay inline in SST files; only larger values are
    // redirected to blob files, matching the workload's 20% 8B / 80% large-value mix.
    opts.enable_blob_files = true;
    opts.min_blob_size = 256;
    opts.blob_file_size = 256ULL * 1024ULL * 1024ULL;
    opts.blob_compression_type = rocksdb::kNoCompression;
    opts.enable_blob_garbage_collection = true;
    return opts;
  }

  static auto make_bulk_load_write_options() -> rocksdb::WriteOptions {
    rocksdb::WriteOptions opts;
    opts.disableWAL = true;
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
  explicit RocksDBBlobDBStore(const std::string &unused_path) {
    (void)unused_path;
    throw std::runtime_error("RocksDB not enabled in this build");
  }

  ~RocksDBBlobDBStore() = default;

  RocksDBBlobDBStore(const RocksDBBlobDBStore &) = delete;
  auto operator=(const RocksDBBlobDBStore &) -> RocksDBBlobDBStore & = delete;

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
  template <typename KeyFn>
  auto bulk_load_impl(std::size_t key_count,
                      KeyFn &&make_key,
                      std::size_t target_value_size,
                      std::string *error_out = nullptr) -> bool {
    (void)key_count;
    (void)make_key;
    (void)target_value_size;
    if (error_out != nullptr) {
      *error_out = "RocksDB not enabled in this build";
    }
    return false;
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
