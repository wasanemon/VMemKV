// rocksdb_store.hpp — Thin RocksDB wrapper exposing byte-span APIs for comparison.
//
// Thread safety: RocksDB is internally thread-safe.

#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
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
#endif

#include "../t1_index/t1_index.hpp"  // For STORE_NOT_FOUND definition

namespace {
constexpr std::size_t kEncodedScalarValueBytes = 8;
}

class RocksDBStore {
 public:
  static constexpr bool kIsEnabled =
#ifdef ENABLE_ROCKSDB
      true;
#else
      false;
#endif

#ifdef ENABLE_ROCKSDB
  // Opens a fresh DB at `path`, destroying any existing data there.
  explicit RocksDBStore(std::string path) : path_(std::move(path)) {
    rocksdb::DestroyDB(path_, {});
    rocksdb::Options opts;
    opts.create_if_missing = true;
    rocksdb::DB *db_handle = nullptr;
    auto status = rocksdb::DB::Open(opts, path_, &db_handle);
    assert(status.ok());
    db_.reset(db_handle);
  }

  // Closes and destroys the DB (cleans up temp files in bench/test usage).
  ~RocksDBStore() {
    db_.reset();
    rocksdb::DestroyDB(path_, {});
  }

  RocksDBStore(const RocksDBStore &) = delete;
  auto operator=(const RocksDBStore &) -> RocksDBStore & = delete;

  // No-op: RocksDB self-compacts.
  void reorganize() {}

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  [[nodiscard]] auto get_impl(std::span<const std::byte> key) const -> uint64_t {
    rocksdb::PinnableSlice pinned_value;
    auto status = db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pinned_value);
    if (!status.ok() || pinned_value.size() != kEncodedScalarValueBytes) {
      return vmemkv::STORE_NOT_FOUND;
    }
    uint64_t value;
    std::memcpy(&value, pinned_value.data(), kEncodedScalarValueBytes);
    return value;
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

  [[nodiscard]] auto get_bytes_impl(std::span<const std::byte> key) const -> std::optional<std::vector<std::byte>> {
    std::string val;
    auto status = db_->Get({}, to_slice(key), &val);
    if (!status.ok()) {
      return std::nullopt;
    }
    return std::vector<std::byte>(reinterpret_cast<const std::byte *>(val.data()),
                                  reinterpret_cast<const std::byte *>(val.data()) + val.size());
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
      uint64_t value = 0;
      const auto value_slice = iterator->value();
      const size_t value_bytes = std::min<size_t>(value_slice.size(), kEncodedScalarValueBytes);
      for (size_t index = 0; index < value_bytes; ++index) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(value_slice[index])) << (index * kEncodedScalarValueBytes);
      }

      callback(to_bytes(iterator->key()), value);
      ++count;
    }
    return count;
  }

 private:
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
  explicit RocksDBStore(std::string) { throw std::runtime_error("RocksDB not enabled in this build"); }

  ~RocksDBStore() = default;

  RocksDBStore(const RocksDBStore &) = delete;
  RocksDBStore &operator=(const RocksDBStore &) = delete;

  void reorganize() {}

  [[nodiscard]] auto get_impl(std::span<const std::byte>) const -> uint64_t { return vmemkv::STORE_NOT_FOUND; }
  auto insert_impl(std::span<const std::byte>, std::span<const std::byte>) -> bool { return false; }
  auto update_impl(std::span<const std::byte>, std::span<const std::byte>) -> bool { return false; }
  auto remove_impl(std::span<const std::byte>) -> bool { return false; }
  [[nodiscard]] auto get_bytes_impl(std::span<const std::byte>) const -> std::optional<std::vector<std::byte>> {
    return std::nullopt;
  }

  template <typename Cb>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  auto scan_impl(std::span<const std::byte>, std::span<const std::byte>, Cb) const -> size_t {
    return 0;
  }
#endif
};
