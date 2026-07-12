// store_adapter.hpp -- KVStore high-level facade / adapter template.
#pragma once

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>
#include <vmemkv/config.hpp>

#include "serializer.hpp"

class RocksDBStore;

namespace vmemkv {

inline constexpr std::size_t kInlineScalarValueBytes = 8;

namespace detail {
template <typename T, typename = void>
struct get_config_type {
  using type = vmemkv::Config<>;
};

template <typename T>
struct get_config_type<T, std::void_t<typename T::ConfigType>> {
  using type = typename T::ConfigType;
};
}  // namespace detail

template <typename KVSImpl>
class StoreAdapter {
 public:
  static constexpr bool kIsEnabled = KVSImpl::kIsEnabled;
  using ConfigType = typename detail::get_config_type<KVSImpl>::type;

  static auto name() -> std::string {
    if constexpr (std::is_same_v<KVSImpl, ::RocksDBStore>) {
      return "RocksDB";
    } else {
      return KVSImpl::name();
    }
  }

  template <typename... Args>
  explicit StoreAdapter(Args &&...args) : impl_(std::forward<Args>(args)...) {}

  ~StoreAdapter() noexcept = default;

  StoreAdapter(const StoreAdapter &) = delete;
  auto operator=(const StoreAdapter &) -> StoreAdapter & = delete;
  StoreAdapter(StoreAdapter &&) = delete;
  auto operator=(StoreAdapter &&) -> StoreAdapter & = delete;

  template <typename Key>
  [[nodiscard]] auto get(const Key &key) const -> uint64_t {
    return kvs_detail::with_key_serialized(
        key, [this](std::span<const std::byte> key_bytes) -> uint64_t { return impl_.get_impl(key_bytes); });
  }

  template <typename Key, typename Value>
  auto insert(const Key &key, Value &&value) -> bool {
    return kvs_detail::with_key_serialized(key, [this, &value](std::span<const std::byte> key_bytes) -> bool {
      return kvs_detail::with_val_serialized(std::forward<Value>(value),
                                             [this, key_bytes](std::span<const std::byte> val_bytes) -> bool {
                                               if (val_bytes.size() == kInlineScalarValueBytes) {
                                                 uint64_t val_u64 = 0;
                                                 std::memcpy(&val_u64, val_bytes.data(), kInlineScalarValueBytes);
                                                 if (val_u64 == ~0ULL) {
                                                   return false;
                                                 }
                                               }
                                               return impl_.insert_impl(key_bytes, val_bytes);
                                             });
    });
  }

  template <typename Key, typename Value>
  auto update(const Key &key, Value &&value) -> bool {
    return kvs_detail::with_key_serialized(key, [this, &value](std::span<const std::byte> key_bytes) -> bool {
      return kvs_detail::with_val_serialized(std::forward<Value>(value),
                                             [this, key_bytes](std::span<const std::byte> val_bytes) -> bool {
                                               if (val_bytes.size() == kInlineScalarValueBytes) {
                                                 uint64_t val_u64 = 0;
                                                 std::memcpy(&val_u64, val_bytes.data(), kInlineScalarValueBytes);
                                                 if (val_u64 == ~0ULL) {
                                                   return false;
                                                 }
                                               }
                                               return impl_.update_impl(key_bytes, val_bytes);
                                             });
    });
  }

  template <typename Key>
  auto remove(const Key &key) -> bool {
    return kvs_detail::with_key_serialized(
        key, [this](std::span<const std::byte> key_bytes) -> bool { return impl_.remove_impl(key_bytes); });
  }

  template <typename LoKey, typename HiKey, typename Callback>
  [[nodiscard]] auto scan(const LoKey &lower_bound, const HiKey &upper_bound, Callback callback) const -> size_t {
    return kvs_detail::with_key_serialized(
        lower_bound, [this, &upper_bound, &callback](std::span<const std::byte> lower_bound_bytes) -> size_t {
          return kvs_detail::with_key_serialized(
              upper_bound,
              [this, lower_bound_bytes, &callback](std::span<const std::byte> upper_bound_bytes) -> size_t {
                return impl_.scan_impl(lower_bound_bytes, upper_bound_bytes, std::move(callback));
              });
        });
  }

  template <typename Key>
  [[nodiscard]] auto get_bytes(const Key &key) const -> std::optional<std::vector<std::byte>> {
    return kvs_detail::with_key_serialized(
        key, [this](std::span<const std::byte> key_bytes) -> std::optional<std::vector<std::byte>> {
          return impl_.get_bytes_impl(key_bytes);
        });
  }

  auto impl() noexcept -> KVSImpl & { return impl_; }
  [[nodiscard]] auto impl() const noexcept -> const KVSImpl & { return impl_; }

  // Delegates to low-level methods if they are exposed
  void reorganize() { impl_.reorganize(); }

  auto get_statistics() const noexcept -> ::vmemkv::VMemKVStatistics {
    if constexpr (std::is_same_v<KVSImpl, ::RocksDBStore>) {
      return ::vmemkv::VMemKVStatistics{};
    } else {
      return impl_.get_statistics();
    }
  }

  // Direct access helper for underlying flat file
  auto t2() -> auto & { return impl_.t2(); }
  [[nodiscard]] auto t2() const -> const auto & { return impl_.t2(); }

 private:
  KVSImpl impl_;
};

}  // namespace vmemkv
