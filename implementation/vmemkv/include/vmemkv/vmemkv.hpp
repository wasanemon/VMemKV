// vmemkv.hpp - Core entrypoint and API specification for VMemKV.
#pragma once

#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "api/store_adapter.hpp"
#include "config.hpp"
#include "rivals/lmdb_store.hpp"
#include "rivals/rocksdb_blobdb_store.hpp"
#include "rivals/rocksdb_store.hpp"
#include "t1_index/t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"
#include "vmemkv_impl.hpp"

namespace vmemkv {

// ─── C++20 Concept Specification ──────────────────────────────────────────
// This Concept serves as the formal interface contract of VMemKV.
// Any KVStore implementation must satisfy this constraint.
template <typename Store>
concept KVStore = requires(Store store, std::span<const std::byte> key, std::span<const std::byte> value) {
  // Core byte-span interfaces
  { store.insert(key, value) } -> std::same_as<bool>;
  { store.update(key, value) } -> std::same_as<bool>;
  { store.remove(key) } -> std::same_as<bool>;

  // get() takes a callback that processes the resulting std::span<const std::byte>
  store.get(key, [](std::span<const std::byte>) {});

  // Bulk-loads entries generated on demand by index -> std::string callbacks. See
  // StoreAdapter::bulk_load()'s own doc comment for its (weaker than insert/update)
  // durability contract.
  store.bulk_load(std::size_t{0}, [](std::size_t) { return std::string{}; }, [](std::size_t) { return std::string{}; });

  // Maintenance/introspection surface. StoreAdapter provides all three unconditionally for every
  // backend -- reorganize() because each rival defines its own no-op, checkpoint()/get_statistics()
  // via StoreAdapter's own is_rival_store_v branch -- so requiring them here just makes that
  // already-uniform guarantee explicit and catches a future rival that forgets its no-op.
  { store.reorganize() } -> std::same_as<void>;
  { store.checkpoint() } -> std::same_as<void>;
  { store.get_statistics() } -> std::same_as<VMemKVStatistics>;
};

// ─── Production Recommended Store ──────────────────────────────────────────
using VMemKVStore = StoreAdapter<VMemKVImpl<detail::System_AllOn>>;

namespace variants {

// ─── Baseline Plain Store ──────────────────────────────────────────────────
using VMemKV_Baseline = StoreAdapter<VMemKVImpl<detail::T1_AllOff>>;

using VMemKV_RocksDB = StoreAdapter<::RocksDBStore>;
using VMemKV_RocksDBBlobDB = StoreAdapter<::RocksDBBlobDBStore>;
using VMemKV_LMDB = StoreAdapter<::LMDBStore>;

// ─── 1. Core Stacked Ablation Variants ───
using VMemKV_Var0_Baseline = VMemKV_Baseline;
using VMemKV_Var1_Bloom = StoreAdapter<VMemKVImpl<Config<BloomFilter>>>;
// Fully optimized production configuration. System_AllOn equals Config<BloomFilter,
// T1InlineValue> (see config.hpp), i.e. exactly this variant's config, so this is the same type
// as VMemKVStore rather than a distinct one.
using VMemKV_Var2_Inline = VMemKVStore;

// ─── 2. Unified Benchmark Registration Tuple ───
using AllPossibleTypes = std::tuple<VMemKV_Var0_Baseline,
                                    VMemKV_Var1_Bloom,
                                    VMemKV_Var2_Inline,
                                    VMemKV_RocksDB,
                                    VMemKV_RocksDBBlobDB,
                                    VMemKV_LMDB>;
}  // namespace variants

}  // namespace vmemkv
