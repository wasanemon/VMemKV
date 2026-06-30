// store_adapter.hpp -- KVStore high-level facade / adapter template.
#pragma once

#include <utility>
#include <span>
#include <vector>
#include <optional>
#include <tuple>
#include <algorithm>
#include <cstring>
#include "serializer.hpp"

class RocksDBStore;

namespace vmemkv
{

template <typename KVSImpl>
class StoreAdapter
{
public:
    static constexpr bool kIsEnabled = KVSImpl::kIsEnabled;

    static std::string name()
    {
        if constexpr (std::is_same_v<KVSImpl, ::RocksDBStore>)
        {
            return "RocksDB";
        }
        else
        {
            return KVSImpl::name();
        }
    }

    template <typename... Args>
    explicit StoreAdapter(Args&&... args)
        : impl_(std::forward<Args>(args)...)
    {
    }

    ~StoreAdapter() noexcept = default;

    StoreAdapter(const StoreAdapter &) = delete;
    StoreAdapter &operator=(const StoreAdapter &) = delete;
    StoreAdapter(StoreAdapter &&) = delete;
    StoreAdapter &operator=(StoreAdapter &&) = delete;

    template <typename Key>
    uint64_t get(const Key &key) const
    {
        return kvs_detail::with_key_serialized(key, [this](std::span<const std::byte> key_bytes) -> uint64_t
            { return impl_.get_impl(key_bytes); });
    }

    template <typename Key, typename Value>
    bool insert(const Key &key, Value &&value)
    {
        return kvs_detail::with_key_serialized(
            key, [this, &value](std::span<const std::byte> key_bytes) -> bool
            {
                return kvs_detail::with_val_serialized(
                    std::forward<Value>(value),
                    [this, key_bytes](std::span<const std::byte> val_bytes) -> bool
                    {
                        if (val_bytes.size() == 8)
                        {
                            uint64_t val_u64 = 0;
                            std::memcpy(&val_u64, val_bytes.data(), 8);
                            if (val_u64 == ~0ULL)
                                return false;
                        }
                        return impl_.insert_impl(key_bytes, val_bytes);
                    });
            });
    }

    template <typename Key, typename Value>
    bool update(const Key &key, Value &&value)
    {
        return kvs_detail::with_key_serialized(
            key, [this, &value](std::span<const std::byte> key_bytes) -> bool
            {
                return kvs_detail::with_val_serialized(
                    std::forward<Value>(value),
                    [this, key_bytes](std::span<const std::byte> val_bytes) -> bool
                    {
                        if (val_bytes.size() == 8)
                        {
                            uint64_t val_u64 = 0;
                            std::memcpy(&val_u64, val_bytes.data(), 8);
                            if (val_u64 == ~0ULL)
                                return false;
                        }
                        return impl_.update_impl(key_bytes, val_bytes);
                    });
            });
    }

    template <typename Key>
    bool remove(const Key &key)
    {
        return kvs_detail::with_key_serialized(key, [this](std::span<const std::byte> key_bytes) -> bool
            { return impl_.remove_impl(key_bytes); });
    }

    template <typename LoKey, typename HiKey, typename Callback>
    size_t scan(const LoKey &lo, const HiKey &hi, Callback cb) const
    {
        return kvs_detail::with_key_serialized(
            lo, [this, &hi, &cb](std::span<const std::byte> lo_bytes) -> size_t
            {
                return kvs_detail::with_key_serialized(
                    hi,
                    [this, lo_bytes, &cb](std::span<const std::byte> hi_bytes) -> size_t
                    { return impl_.scan_impl(lo_bytes, hi_bytes, std::move(cb)); });
            });
    }

    template <typename Key>
    std::optional<std::vector<std::byte>> get_bytes(const Key &key) const
    {
        return kvs_detail::with_key_serialized(key, [this](std::span<const std::byte> key_bytes) -> std::optional<std::vector<std::byte>>
            { return impl_.get_bytes_impl(key_bytes); });
    }

    KVSImpl& impl() noexcept { return impl_; }
    const KVSImpl& impl() const noexcept { return impl_; }

    // Delegates to low-level methods if they are exposed
    void reorganize()
    {
        impl_.reorganize();
    }

    // Direct access helper for underlying flat file
    auto& t2() { return impl_.t2(); }
    const auto& t2() const { return impl_.t2(); }

private:
    KVSImpl impl_;
};

} // namespace vmemkv
