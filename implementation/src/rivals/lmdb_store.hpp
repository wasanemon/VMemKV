// lmdb_store.hpp — Thin LMDB (Lightning Memory-Mapped Database) wrapper exposing
// byte-span APIs for comparison.
//
// Thread safety: LMDB allows unlimited concurrent readers (MVCC snapshots that never
// block or are blocked) but exactly one writer transaction at a time; concurrent
// Insert/Update/Delete calls serialize on LMDB's internal writer lock. This is
// reported as-is rather than worked around, since it is an inherent characteristic
// of LMDB's B+Tree/copy-on-write design being compared against the other engines.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef ENABLE_LMDB
#include <lmdb.h>
#endif

#include "../t1_index/t1_index.hpp"  // For STORE_NOT_FOUND definition

class LMDBStore {
 public:
  static constexpr bool kIsEnabled =
#ifdef ENABLE_LMDB
      true;
#else
      false;
#endif

#ifdef ENABLE_LMDB
  // Opens a fresh environment at a unique subpath (single-file DB via MDB_NOSUBDIR),
  // mirroring RocksDBStore's per-instance path uniquing.
  explicit LMDBStore(std::string path) {
    static std::atomic<uint64_t> instance_counter{0};
    path_ = std::move(path) + "_" + std::to_string(instance_counter.fetch_add(1, std::memory_order_relaxed)) + ".lmdb";
    std::filesystem::remove(path_);
    std::filesystem::remove(path_ + "-lock");

    if (mdb_env_create(&env_) != 0) {
      throw std::runtime_error("LMDB env_create failed");
    }
    mdb_env_set_mapsize(env_, kMapSizeBytes);
    mdb_env_set_maxreaders(env_, kMaxReaders);

    // Benchmark tuning lives here so the wrapper API stays separate from the
    // current population policy. MDB_WRITEMAP + MDB_MAPASYNC skips the per-commit
    // fsync (async background flush instead), matching RocksDB's benchmark
    // WriteOptions{} default of sync=false -- otherwise LMDB's default full fsync
    // per transaction would make the comparison unfair (measuring disk latency
    // rather than the engine itself).
    constexpr unsigned int kEnvFlags = MDB_NOSUBDIR | MDB_WRITEMAP | MDB_MAPASYNC;
    int rc = mdb_env_open(env_, path_.c_str(), kEnvFlags, 0664);
    if (rc != 0) {
      mdb_env_close(env_);
      env_ = nullptr;
      throw std::runtime_error("LMDB env_open failed: " + std::string(mdb_strerror(rc)));
    }

    MDB_txn *txn = nullptr;
    rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != 0) {
      mdb_env_close(env_);
      env_ = nullptr;
      throw std::runtime_error("LMDB txn_begin failed: " + std::string(mdb_strerror(rc)));
    }
    rc = mdb_dbi_open(txn, nullptr, 0, &dbi_);
    if (rc != 0) {
      mdb_txn_abort(txn);
      mdb_env_close(env_);
      env_ = nullptr;
      throw std::runtime_error("LMDB dbi_open failed: " + std::string(mdb_strerror(rc)));
    }
    mdb_txn_commit(txn);
  }

  // Closes the environment (cleans up temp files in bench/test usage).
  ~LMDBStore() {
    if (env_ != nullptr) {
      mdb_env_close(env_);
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(path_ + "-lock", ignored);
  }

  LMDBStore(const LMDBStore &) = delete;
  auto operator=(const LMDBStore &) -> LMDBStore & = delete;

  // No-op: LMDB reclaims freed B+Tree pages automatically via its freelist; there is
  // no manual reorganize/compaction concept during normal operation.
  void reorganize() {}

  // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

  template <typename Callback>
  auto get_impl(std::span<const std::byte> key, Callback callback) const -> bool {
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != 0) {
      return false;
    }
    MDB_val mkey = to_val(key);
    MDB_val mval{};
    const bool found = (mdb_get(txn, dbi_, &mkey, &mval) == 0);
    if (found) {
      callback(std::span<const std::byte>(static_cast<const std::byte *>(mval.mv_data), mval.mv_size));
    }
    mdb_txn_abort(txn);
    return found;
  }

  auto insert_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != 0) {
      return false;
    }
    MDB_val mkey = to_val(key);
    MDB_val existing{};
    if (mdb_get(txn, dbi_, &mkey, &existing) == 0) {
      mdb_txn_abort(txn);
      return false;  // already exists
    }
    MDB_val mval = to_val(value);
    if (mdb_put(txn, dbi_, &mkey, &mval, 0) != 0) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == 0;
  }

  auto update_impl(std::span<const std::byte> key, std::span<const std::byte> value) -> bool {
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != 0) {
      return false;
    }
    MDB_val mkey = to_val(key);
    MDB_val existing{};
    if (mdb_get(txn, dbi_, &mkey, &existing) != 0) {
      mdb_txn_abort(txn);
      return false;  // not found
    }
    MDB_val mval = to_val(value);
    if (mdb_put(txn, dbi_, &mkey, &mval, 0) != 0) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == 0;
  }

  auto remove_impl(std::span<const std::byte> key) -> bool {
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != 0) {
      return false;
    }
    MDB_val mkey = to_val(key);
    MDB_val existing{};
    if (mdb_get(txn, dbi_, &mkey, &existing) != 0) {
      mdb_txn_abort(txn);
      return false;  // not found
    }
    if (mdb_del(txn, dbi_, &mkey, nullptr) != 0) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == 0;
  }

  template <typename KeyFn>
  auto bulk_load_impl(std::size_t key_count,
                      KeyFn &&make_key,
                      std::size_t target_value_size,
                      std::string *error_out = nullptr) -> bool {
    if (key_count == 0) {
      return true;
    }
    // Batch many puts per write transaction: committing once per key would pay the
    // (async, but still non-trivial) commit overhead key_count times.
    constexpr std::size_t kBatchKeys = 100'000;
    std::string dummy_large(target_value_size, 'a');
    std::string dummy_8b(8, 'a');

    std::size_t next_index = 0;
    while (next_index < key_count) {
      MDB_txn *txn = nullptr;
      if (mdb_txn_begin(env_, nullptr, 0, &txn) != 0) {
        if (error_out != nullptr) {
          *error_out = "LMDB txn_begin failed during bulk load";
        }
        return false;
      }
      const std::size_t chunk_end = std::min(key_count, next_index + kBatchKeys);
      for (std::size_t index = next_index; index < chunk_end; ++index) {
        const std::string key = make_key(index);
        const std::string &value = (target_value_size != 8 && index % 5 == 0) ? dummy_8b : dummy_large;
        MDB_val mkey{key.size(), const_cast<char *>(key.data())};
        MDB_val mval{value.size(), const_cast<char *>(value.data())};
        if (mdb_put(txn, dbi_, &mkey, &mval, 0) != 0) {
          mdb_txn_abort(txn);
          if (error_out != nullptr) {
            *error_out = "LMDB put failed during bulk load";
          }
          return false;
        }
      }
      if (mdb_txn_commit(txn) != 0) {
        if (error_out != nullptr) {
          *error_out = "LMDB txn_commit failed during bulk load";
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
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != 0) {
      return 0;
    }
    MDB_cursor *cursor = nullptr;
    if (mdb_cursor_open(txn, dbi_, &cursor) != 0) {
      mdb_txn_abort(txn);
      return 0;
    }

    size_t count = 0;
    MDB_val mkey = to_val(lower_bound);
    MDB_val mval{};
    int rc = mdb_cursor_get(cursor, &mkey, &mval, MDB_SET_RANGE);
    while (rc == 0) {
      if (compare_to_bound(mkey, upper_bound) > 0) {
        break;
      }
      callback(std::span<const std::byte>(static_cast<const std::byte *>(mkey.mv_data), mkey.mv_size),
               std::span<const std::byte>(static_cast<const std::byte *>(mval.mv_data), mval.mv_size));
      ++count;
      rc = mdb_cursor_get(cursor, &mkey, &mval, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return count;
  }

 private:
  // 1TB virtual address space reservation (sparse; LMDB only maps what's actually
  // used). Matches vmemkv::Config<>::DefaultT2CapacityBytes so both engines are
  // given comparable headroom for LTM-scale corpora.
  static constexpr size_t kMapSizeBytes = 1ULL << 40;
  static constexpr unsigned int kMaxReaders = 512;

  static auto to_val(std::span<const std::byte> bytes) noexcept -> MDB_val {
    return MDB_val{bytes.size(), const_cast<std::byte *>(bytes.data())};
  }

  // LMDB's default (unnamed) database uses plain memcmp byte-string ordering, so
  // this mirrors that exactly to decide whether `key` is past `upper_bound`.
  static auto compare_to_bound(const MDB_val &key, std::span<const std::byte> upper_bound) noexcept -> int {
    const auto *key_bytes = static_cast<const std::byte *>(key.mv_data);
    const size_t min_len = std::min(key.mv_size, upper_bound.size());
    const int cmp = min_len == 0 ? 0 : std::memcmp(key_bytes, upper_bound.data(), min_len);
    if (cmp != 0) {
      return cmp;
    }
    if (key.mv_size == upper_bound.size()) {
      return 0;
    }
    return key.mv_size < upper_bound.size() ? -1 : 1;
  }

  MDB_env *env_ = nullptr;
  MDB_dbi dbi_ = 0;
  std::string path_;
#else
  // Dummy stub implementation when LMDB is disabled.
  explicit LMDBStore(const std::string &unused_path) {
    (void)unused_path;
    throw std::runtime_error("LMDB not enabled in this build");
  }

  ~LMDBStore() = default;

  LMDBStore(const LMDBStore &) = delete;
  auto operator=(const LMDBStore &) -> LMDBStore & = delete;

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
      *error_out = "LMDB not enabled in this build";
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
