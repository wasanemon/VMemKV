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

#include "rival_store_disabled_stub.hpp"

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

    // MDB_WRITEMAP without MDB_MAPASYNC means every commit synchronously msyncs, matching
    // VMemKV's own per-write fsync contract (wal.hpp) and RocksDB's WriteOptions.sync=true (see
    // RocksDBStore::make_durable_write_options()). bulk_load_impl() below toggles MDB_MAPASYNC
    // on for its own duration instead, like the other two engines' bulk loaders.
    constexpr unsigned int kEnvFlags = MDB_NOSUBDIR | MDB_WRITEMAP;
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

  // Tag type selecting the clone-from-master constructor below. Public so StoreAdapter's
  // variadic forwarding constructor can name it directly -- see
  // for_each_store_variant()'s make_fresh_corpus()/make_fresh_corpus_checkpoint() in bench_kv.cpp.
  struct CloneFromMasterTag {};

  // Builds `master_path` once via bulk_load, then clones it into this instance's own path with
  // mdb_env_copy2(..., MDB_CP_COMPACT) -- much cheaper than re-running bulk_load_impl against an
  // empty environment each time. Must be a real copy, not a hardlink: LMDB mmaps and writes its
  // B+Tree pages in place, so a hardlinked clone and its master would corrupt each other on the
  // first write (unlike RocksDBStore::clone_from(), which can hardlink immutable SST files).
  template <typename KeyFn, typename ValueFn>
  LMDBStore(CloneFromMasterTag /*tag*/,
            const std::string &master_path,
            std::size_t key_count,
            KeyFn &&make_key,
            ValueFn &&make_value) {
    ensure_master_built(master_path, key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
    static std::atomic<uint64_t> clone_counter{0};
    path_ = master_path + "_clone_" + std::to_string(clone_counter.fetch_add(1, std::memory_order_relaxed)) + ".lmdb";
    clone_from(master_path, path_);

    if (mdb_env_create(&env_) != 0) {
      throw std::runtime_error("LMDB env_create failed (clone)");
    }
    mdb_env_set_mapsize(env_, kMapSizeBytes);
    mdb_env_set_maxreaders(env_, kMaxReaders);
    constexpr unsigned int kEnvFlags = MDB_NOSUBDIR | MDB_WRITEMAP;
    int rc = mdb_env_open(env_, path_.c_str(), kEnvFlags, 0664);
    if (rc != 0) {
      mdb_env_close(env_);
      env_ = nullptr;
      throw std::runtime_error("LMDB env_open failed (clone): " + std::string(mdb_strerror(rc)));
    }
    MDB_txn *txn = nullptr;
    rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != 0) {
      mdb_env_close(env_);
      env_ = nullptr;
      throw std::runtime_error("LMDB txn_begin failed (clone): " + std::string(mdb_strerror(rc)));
    }
    rc = mdb_dbi_open(txn, nullptr, 0, &dbi_);
    if (rc != 0) {
      mdb_txn_abort(txn);
      mdb_env_close(env_);
      env_ = nullptr;
      throw std::runtime_error("LMDB dbi_open failed (clone): " + std::string(mdb_strerror(rc)));
    }
    mdb_txn_commit(txn);
  }

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

  template <typename KeyFn, typename ValueFn>
  void bulk_load_impl(std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    bulk_load_into(env_, dbi_, key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
  }

  template <typename KeyFn, typename ValueFn>
  static void bulk_load_into(MDB_env *env, MDB_dbi dbi, std::size_t key_count, KeyFn &&make_key, ValueFn &&make_value) {
    if (key_count == 0) {
      return;
    }
    // Non-durable during bulk load, like VMemKV's own bulk_load_impl (bypasses its WAL) and
    // RocksDB's (disableWAL=true): toggle MDB_MAPASYNC on for the duration so batch commits skip
    // the synchronous msync, then restore the durable default.
    mdb_env_set_flags(env, MDB_MAPASYNC, 1);
    struct RestoreDurability {
      MDB_env *env;
      ~RestoreDurability() { mdb_env_set_flags(env, MDB_MAPASYNC, 0); }
    } restore_durability{env};

    // Batch many puts per write transaction: committing once per key would pay the
    // (async, but still non-trivial) commit overhead key_count times.
    constexpr std::size_t kBatchKeys = 100'000;

    std::size_t next_index = 0;
    while (next_index < key_count) {
      MDB_txn *txn = nullptr;
      if (mdb_txn_begin(env, nullptr, 0, &txn) != 0) {
        throw std::runtime_error("LMDB txn_begin failed during bulk load");
      }
      const std::size_t chunk_end = std::min(key_count, next_index + kBatchKeys);
      for (std::size_t index = next_index; index < chunk_end; ++index) {
        const std::string key = make_key(index);
        const std::string value = make_value(index);
        MDB_val mkey{key.size(), const_cast<char *>(key.data())};
        MDB_val mval{value.size(), const_cast<char *>(value.data())};
        if (mdb_put(txn, dbi, &mkey, &mval, 0) != 0) {
          mdb_txn_abort(txn);
          throw std::runtime_error("LMDB put failed during bulk load");
        }
      }
      if (mdb_txn_commit(txn) != 0) {
        throw std::runtime_error("LMDB txn_commit failed during bulk load");
      }
      next_index = chunk_end;
    }
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

  // Builds a fresh master at a ".building" sibling path, renamed into place only once fully
  // populated and closed, so a crash mid-build never leaves a partial master for a later call
  // to trust. The build env is closed before the rename: LMDB forbids the same file being open
  // twice at once in one process, and clone_from() below opens this path again afterward.
  template <typename KeyFn, typename ValueFn>
  static void ensure_master_built(const std::string &master_path,
                                  std::size_t key_count,
                                  KeyFn &&make_key,
                                  ValueFn &&make_value) {
    if (std::filesystem::exists(master_path)) {
      return;
    }
    const std::string building_path = master_path + ".building";
    std::error_code ignored;
    std::filesystem::remove(building_path, ignored);
    std::filesystem::remove(building_path + "-lock", ignored);

    MDB_env *build_env = nullptr;
    if (mdb_env_create(&build_env) != 0) {
      throw std::runtime_error("LMDB env_create failed (master build)");
    }
    mdb_env_set_mapsize(build_env, kMapSizeBytes);
    mdb_env_set_maxreaders(build_env, kMaxReaders);
    int rc = mdb_env_open(build_env, building_path.c_str(), MDB_NOSUBDIR | MDB_WRITEMAP, 0664);
    if (rc != 0) {
      mdb_env_close(build_env);
      throw std::runtime_error("LMDB env_open failed (master build): " + std::string(mdb_strerror(rc)));
    }
    MDB_dbi build_dbi = 0;
    MDB_txn *txn = nullptr;
    rc = mdb_txn_begin(build_env, nullptr, 0, &txn);
    if (rc != 0) {
      mdb_env_close(build_env);
      throw std::runtime_error("LMDB txn_begin failed (master build): " + std::string(mdb_strerror(rc)));
    }
    rc = mdb_dbi_open(txn, nullptr, 0, &build_dbi);
    if (rc != 0) {
      mdb_txn_abort(txn);
      mdb_env_close(build_env);
      throw std::runtime_error("LMDB dbi_open failed (master build): " + std::string(mdb_strerror(rc)));
    }
    mdb_txn_commit(txn);

    bulk_load_into(build_env, build_dbi, key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
    mdb_env_close(build_env);

    std::filesystem::remove(master_path, ignored);
    std::filesystem::remove(master_path + "-lock", ignored);
    std::error_code rename_error;
    std::filesystem::rename(building_path, master_path, rename_error);
    if (rename_error) {
      throw std::runtime_error("LMDB master build rename failed: " + rename_error.message());
    }
  }

  // Copies `source_path` to `dest_path` via mdb_env_copy2(..., MDB_CP_COMPACT), which also
  // drops free/unused pages so the clone is no larger than its live data. See the
  // CloneFromMasterTag constructor above for why this must be a real copy, not a hardlink.
  static void clone_from(const std::string &source_path, const std::string &dest_path) {
    std::error_code ignored;
    std::filesystem::remove(dest_path, ignored);
    std::filesystem::remove(dest_path + "-lock", ignored);

    MDB_env *source_env = nullptr;
    if (mdb_env_create(&source_env) != 0) {
      throw std::runtime_error("LMDB env_create failed (clone source)");
    }
    mdb_env_set_mapsize(source_env, kMapSizeBytes);
    mdb_env_set_maxreaders(source_env, kMaxReaders);
    int rc = mdb_env_open(source_env, source_path.c_str(), MDB_NOSUBDIR | MDB_RDONLY, 0664);
    if (rc != 0) {
      mdb_env_close(source_env);
      throw std::runtime_error("LMDB env_open failed (clone source): " + std::string(mdb_strerror(rc)));
    }
    rc = mdb_env_copy2(source_env, dest_path.c_str(), MDB_CP_COMPACT);
    mdb_env_close(source_env);
    if (rc != 0) {
      throw std::runtime_error("LMDB env_copy2 failed: " + std::string(mdb_strerror(rc)));
    }
  }

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
  VMEMKV_RIVAL_DISABLED_STUB(LMDBStore, "LMDB")
#endif
};
