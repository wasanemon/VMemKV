// rocksdb_store.hpp — Thin RocksDB wrapper with the same interface as T1OnlyStore.
//
// Keys  : StoreKey (16-byte big-endian array) — stored as raw bytes in RocksDB.
//         Big-endian encoding preserves lexicographic order, so RocksDB's
//         default byte-lex comparator gives the same key order as T1OnlyStore.
// Values: uint64_t — stored as 8 raw bytes (little-endian native layout).
//
// reorganize(): no-op (RocksDB manages compaction internally).
// Thread safety: RocksDB is internally thread-safe.

#pragma once

#include <cassert>
#include <cstring>
#include <memory>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>

#include "store_types.hpp"

class RocksDBStore
{
public:
    // Opens a fresh DB at `path`, destroying any existing data there.
    explicit RocksDBStore(std::string path)
        : path_(std::move(path))
    {
        rocksdb::DestroyDB(path_, {});
        rocksdb::Options opts;
        opts.create_if_missing = true;
        std::unique_ptr<rocksdb::DB> db;
        auto s = rocksdb::DB::Open(opts, path_, &db);
        assert(s.ok());
        db_ = std::move(db);
    }

    // Closes and destroys the DB (cleans up temp files in bench/test usage).
    ~RocksDBStore()
    {
        db_.reset();
        rocksdb::DestroyDB(path_, {});
    }

    RocksDBStore(const RocksDBStore &) = delete;
    RocksDBStore &operator=(const RocksDBStore &) = delete;

    // ── Point operations ──────────────────────────────────────────────────────

    uint64_t get(StoreKey key) const
    {
        rocksdb::PinnableSlice pv;
        auto s = db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv);
        if (!s.ok() || pv.size() != 8)
            return STORE_NOT_FOUND;
        uint64_t v;
        std::memcpy(&v, pv.data(), 8);
        return v;
    }

    bool insert(StoreKey key, uint64_t value)
    {
        rocksdb::PinnableSlice pv;
        if (db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv).ok())
            return false; // already exists
        return do_put(key, value);
    }

    bool update(StoreKey key, uint64_t value)
    {
        rocksdb::PinnableSlice pv;
        if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv).ok())
            return false; // not found
        return do_put(key, value);
    }

    bool remove(StoreKey key)
    {
        rocksdb::PinnableSlice pv;
        if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv).ok())
            return false; // not found
        return db_->Delete({}, to_slice(key)).ok();
    }

    // ── Range scan ────────────────────────────────────────────────────────────
    // cb(StoreKey, uint64_t) for each entry with key in [lo, hi].
    template <typename Cb>
    size_t scan(StoreKey lo, StoreKey hi, Cb cb) const
    {
        auto it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator({}));
        rocksdb::Slice lo_s = to_slice(lo), hi_s = to_slice(hi);
        size_t count = 0;
        for (it->Seek(lo_s); it->Valid() && it->key().compare(hi_s) <= 0; it->Next())
        {
            uint64_t v;
            std::memcpy(&v, it->value().data(), 8);
            cb(from_slice_key(it->key()), v);
            ++count;
        }
        return count;
    }

    // No-op: RocksDB self-compacts.
    void reorganize() {}

private:
    static rocksdb::Slice to_slice(const StoreKey &k) noexcept
    {
        return {reinterpret_cast<const char *>(k.data()), 16};
    }

    static StoreKey from_slice_key(rocksdb::Slice s) noexcept
    {
        StoreKey k{};
        std::memcpy(k.data(), s.data(), std::min(s.size(), size_t{16}));
        return k;
    }

    bool do_put(StoreKey key, uint64_t value)
    {
        char buf[8];
        std::memcpy(buf, &value, 8);
        return db_->Put({}, to_slice(key), rocksdb::Slice(buf, 8)).ok();
    }

    std::unique_ptr<rocksdb::DB> db_;
    std::string path_;
};
