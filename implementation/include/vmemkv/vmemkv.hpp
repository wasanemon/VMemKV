// vmemkv.hpp - Core entrypoint and API specification for VMemKV.
#pragma once

#include <concepts>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "api/store_adapter.hpp"
#include "config.hpp"
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

  // get is now a callback-based API accepting a callable that processes std::span<const std::byte>
  store.get(key, [](std::span<const std::byte>) {});
};

// ─── Production Recommended Store ──────────────────────────────────────────
using VMemKVStore = StoreAdapter<VMemKVImpl<detail::System_AllOn>>;

namespace variants {

// ─── Baseline Plain Store ──────────────────────────────────────────────────
using VMemKV_Baseline = StoreAdapter<VMemKVImpl<detail::T1_AllOff>>;

using VMemKVRocksDB = StoreAdapter<::RocksDBStore>;

// ─── 1. Core Stacked Ablation Variants ───
using VMemKV_Var0_Baseline = VMemKV_Baseline;
using VMemKV_Var1_Bloom = StoreAdapter<VMemKVImpl<Config<BloomFilter>>>;
using VMemKV_Var2_Simd = StoreAdapter<VMemKVImpl<Config<BloomFilter, SimdScan>>>;
using VMemKV_Var3_Inline = StoreAdapter<VMemKVImpl<Config<BloomFilter, SimdScan, T1InlineValue>>>;
using VMemKV_Var4_Prefault = VMemKVStore;  // Fully optimized production configuration
using VMemKVStore = VMemKVStore;

using VMemKV_RocksDB = VMemKVRocksDB;

// ─── 2. Unified Benchmark Registration Tuple ───
using AllPossibleTypes = std::tuple<VMemKV_Var0_Baseline,
                                    VMemKV_Var1_Bloom,
                                    VMemKV_Var2_Simd,
                                    VMemKV_Var3_Inline,
                                    VMemKV_Var4_Prefault,
                                    VMemKV_RocksDB>;
}  // namespace variants

}  // namespace vmemkv
