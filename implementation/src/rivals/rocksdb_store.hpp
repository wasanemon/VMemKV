// rocksdb_store.hpp — Thin RocksDB wrapper exposing byte-span APIs for comparison.
//
// Thread safety: RocksDB is internally thread-safe.

#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <optional>
#include <stdexcept>

#ifdef ENABLE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#endif

#include "../t1_index/t1_index.hpp" // For STORE_NOT_FOUND definition

class RocksDBStore
{
public:
    static constexpr bool kIsEnabled =
#ifdef ENABLE_ROCKSDB
        true;
#else
        false;
#endif

#ifdef ENABLE_ROCKSDB
    // Opens a fresh DB at `path`, destroying any existing data there.
    explicit RocksDBStore(std::string path)
        : path_(std::move(path))
    {
        rocksdb::DestroyDB(path_, {});
        rocksdb::Options opts;
        opts.create_if_missing = true;
        rocksdb::DB *db = nullptr;
        auto s = rocksdb::DB::Open(opts, path_, &db);
        assert(s.ok());
        db_.reset(db);
    }

    // Closes and destroys the DB (cleans up temp files in bench/test usage).
    ~RocksDBStore()
    {
        db_.reset();
        rocksdb::DestroyDB(path_, {});
    }

    RocksDBStore(const RocksDBStore &) = delete;
    RocksDBStore &operator=(const RocksDBStore &) = delete;

    // No-op: RocksDB self-compacts.
    void reorganize() {}

    // ─── Low-level byte-span APIs (called by StoreAdapter) ───────────────────────

    uint64_t get_impl(std::span<const std::byte> key) const
    {
        rocksdb::PinnableSlice pv;
        auto s = db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv);
        if (!s.ok() || pv.size() != 8)
            return vmemkv::STORE_NOT_FOUND;
        uint64_t v;
        std::memcpy(&v, pv.data(), 8);
        return v;
    }

    bool insert_impl(std::span<const std::byte> key, std::span<const std::byte> value)
    {
        rocksdb::PinnableSlice pv;
        if (db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv).ok())
            return false; // already exists
        return db_->Put({}, to_slice(key), to_slice(value)).ok();
    }

    bool update_impl(std::span<const std::byte> key, std::span<const std::byte> value)
    {
        rocksdb::PinnableSlice pv;
        if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv).ok())
            return false; // not found
        return db_->Put({}, to_slice(key), to_slice(value)).ok();
    }

    bool remove_impl(std::span<const std::byte> key)
    {
        rocksdb::PinnableSlice pv;
        if (!db_->Get({}, db_->DefaultColumnFamily(), to_slice(key), &pv).ok())
            return false; // not found
        return db_->Delete({}, to_slice(key)).ok();
    }

    std::optional<std::vector<std::byte>> get_bytes_impl(std::span<const std::byte> key) const
    {
        std::string val;
        auto s = db_->Get({}, to_slice(key), &val);
        if (!s.ok())
            return std::nullopt;
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte *>(val.data()),
            reinterpret_cast<const std::byte *>(val.data()) + val.size());
    }

    template <typename Cb>
    size_t scan_impl(std::span<const std::byte> lo,
                      std::span<const std::byte> hi,
                      Cb cb) const
    {
        auto it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator({}));
        rocksdb::Slice lo_s = to_slice(lo), hi_s = to_slice(hi);
        size_t count = 0;
        for (it->Seek(lo_s); it->Valid() && it->key().compare(hi_s) <= 0; it->Next())
        {
            uint64_t v = 0;
            const auto val_s = it->value();
            const size_t n = std::min<size_t>(val_s.size(), 8);
            for (size_t i = 0; i < n; ++i)
                v |= static_cast<uint64_t>(static_cast<uint8_t>(val_s[i])) << (i * 8);

            cb(to_bytes(it->key()), v);
            ++count;
        }
        return count;
    }

private:
    static rocksdb::Slice to_slice(std::span<const std::byte> k) noexcept
    {
        return {reinterpret_cast<const char *>(k.data()), k.size()};
    }

    static std::span<const std::byte> to_bytes(const rocksdb::Slice &s) noexcept
    {
        return {reinterpret_cast<const std::byte *>(s.data()), s.size()};
    }

    std::unique_ptr<rocksdb::DB> db_;
    std::string path_;
#else
    // Dummy stub implementation when RocksDB is disabled.
    explicit RocksDBStore(std::string)
    {
        throw std::runtime_error("RocksDB not enabled in this build");
    }

    ~RocksDBStore() = default;

    RocksDBStore(const RocksDBStore &) = delete;
    RocksDBStore &operator=(const RocksDBStore &) = delete;

    void reorganize() {}

    uint64_t get_impl(std::span<const std::byte>) const { return vmemkv::STORE_NOT_FOUND; }
    bool insert_impl(std::span<const std::byte>, std::span<const std::byte>) { return false; }
    bool update_impl(std::span<const std::byte>, std::span<const std::byte>) { return false; }
    bool remove_impl(std::span<const std::byte>) { return false; }
    std::optional<std::vector<std::byte>> get_bytes_impl(std::span<const std::byte>) const { return std::nullopt; }

    template <typename Cb>
    size_t scan_impl(std::span<const std::byte>, std::span<const std::byte>, Cb) const { return 0; }
#endif
};
