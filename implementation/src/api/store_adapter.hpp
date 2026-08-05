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
class RocksDBBlobDBStore;
class LMDBStore;

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

// Rival (non-VMemKV) backends have no T1/T2 concept: they self-manage storage
// layout, so reorganize()/get_statistics() are no-ops/empty for all of them.
template <typename T>
inline constexpr bool is_rival_store_v =
    std::is_same_v<T, ::RocksDBStore> || std::is_same_v<T, ::RocksDBBlobDBStore> || std::is_same_v<T, ::LMDBStore>;
}  // namespace detail

template <typename KVSImpl>
class StoreAdapter {
 public:
  static constexpr bool kIsEnabled = KVSImpl::kIsEnabled;
  using ConfigType = typename detail::get_config_type<KVSImpl>::type;
  // Exposes the wrapped backend type so generic code (e.g. the benchmark harness) can reach
  // backend-specific static members/tag types without needing its own KVSImpl parameter.
  using Impl = KVSImpl;

  static auto name() -> std::string {
    if constexpr (std::is_same_v<KVSImpl, ::RocksDBStore>) {
      return "RocksDB";
    } else if constexpr (std::is_same_v<KVSImpl, ::RocksDBBlobDBStore>) {
      return "RocksDB-BlobDB";
    } else if constexpr (std::is_same_v<KVSImpl, ::LMDBStore>) {
      return "LMDB";
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

  template <typename Key, typename Callback>
  auto get(const Key &key, Callback callback) const -> bool {
    return kvs_detail::with_key_serialized(key, [this, &callback](std::span<const std::byte> key_bytes) -> bool {
      return impl_.get_impl(key_bytes, std::move(callback));
    });
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

  // Bulk-loads `count` entries generated on demand by make_key(index)/make_value(index). Throws
  // on failure. Much faster than `count` individual insert() calls, but unlike insert()/update()
  // gives no durability guarantee by itself -- see each backend's bulk_load_impl() (VMemKVImpl,
  // e.g., skips its WAL entirely; callers needing crash survival must checkpoint afterward, e.g.
  // reorganize(true)). Upsert semantics (no existing-key check), and not safe for concurrent
  // access during the call.
  template <typename KeyFn, typename ValueFn>
  void bulk_load(std::size_t count, KeyFn &&make_key, ValueFn &&make_value) {
    impl_.bulk_load_impl(count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
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

  auto impl() noexcept -> KVSImpl & { return impl_; }
  [[nodiscard]] auto impl() const noexcept -> const KVSImpl & { return impl_; }

  // Delegates to low-level methods if they are exposed
  void reorganize() { impl_.reorganize(); }

  // force_t2_gc selects between a T1-only reorganize (fast, zero I/O) and a full T1+T2
  // reorganize (rebuilds the T2 file too). Rival backends have no such distinction --
  // they self-compact/self-manage storage -- so this is a no-op for them regardless
  // of the flag.
  void reorganize(bool force_t2_gc) {
    if constexpr (detail::is_rival_store_v<KVSImpl>) {
      (void)force_t2_gc;
    } else {
      impl_.reorganize(force_t2_gc);
    }
  }

  auto get_statistics() const noexcept -> ::vmemkv::VMemKVStatistics {
    if constexpr (detail::is_rival_store_v<KVSImpl>) {
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
