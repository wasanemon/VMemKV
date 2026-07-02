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
concept KVStore = requires(Store store,
                           std::span<const std::byte> key,
                           std::span<const std::byte> value,
                           std::string_view key_str,
                           uint64_t value_u64) {
  // Core byte-span interfaces
  { store.insert(key, value) } -> std::same_as<bool>;
  { store.get(key) } -> std::same_as<uint64_t>;
  { store.update(key, value) } -> std::same_as<bool>;
  { store.remove(key) } -> std::same_as<bool>;
  { store.get_bytes(key) } -> std::same_as<std::optional<std::vector<std::byte>>>;

  // Serialization helper interfaces (syntactic sugar)
  { store.insert(key_str, value_u64) } -> std::same_as<bool>;
  { store.get(key_str) } -> std::same_as<uint64_t>;
  { store.update(key_str, value_u64) } -> std::same_as<bool>;
};

// ─── Production Recommended Store ──────────────────────────────────────────
using VMemKVStore = StoreAdapter<VMemKVImpl<detail::System_AllOn>>;

// ─── Baseline Plain Store ──────────────────────────────────────────────────
using VMemKVBaselineStore = StoreAdapter<VMemKVImpl<detail::T1_AllOff>>;

using VMemKVRocksDB = StoreAdapter<::RocksDBStore>;

namespace variants {
// ─── Base Store Definitions ──────────────────────────────────────────────
using VMemKV_Baseline = VMemKVBaselineStore;
using VMemKV_RocksDB = VMemKVRocksDB;
using VMemKV_T2_InlineAll = VMemKVStore;  // Fully optimized production configuration

// ─── A. T1 Index Optimizations (Cumulative Addition Study) ───────────────
// Measures performance by enabling T1 optimizations incrementally
// (AppendMap -> BloomFilter -> SimdScan -> MemoryHints).
using VMemKV_Cumulative_Step1 = StoreAdapter<VMemKVImpl<Config<AppendMap>>>;
using VMemKV_Cumulative_Step2 = StoreAdapter<VMemKVImpl<Config<AppendMap, BloomFilter>>>;
using VMemKV_Cumulative_Step3 = StoreAdapter<VMemKVImpl<Config<AppendMap, BloomFilter, SimdScan>>>;
using VMemKV_Cumulative_Step4 = StoreAdapter<VMemKVImpl<Config<AppendMap, BloomFilter, SimdScan, MemoryHints>>>;

// ─── B. T1 Index Optimizations (Ablation/Subtractive Study) ──────────────
// Measures the individual performance contribution of each T1 feature
// by disabling it from the fully optimized configuration.
using VMemKV_Ablation_No_AppendMap =
    StoreAdapter<VMemKVImpl<Config<BloomFilter, SimdScan, MemoryHints, T1InlineValue>>>;
using VMemKV_Ablation_No_BloomFilter =
    StoreAdapter<VMemKVImpl<Config<AppendMap, SimdScan, MemoryHints, T1InlineValue>>>;
using VMemKV_Ablation_No_SimdScan =
    StoreAdapter<VMemKVImpl<Config<AppendMap, BloomFilter, MemoryHints, T1InlineValue>>>;
using VMemKV_Ablation_No_MemoryHints =
    StoreAdapter<VMemKVImpl<Config<AppendMap, BloomFilter, SimdScan, T1InlineValue>>>;

// ─── C. T2 Storage Value Inlining (Ablation/Subtractive Study) ───────────
// Measures the performance impact of value-inlining policies.
using VMemKV_T2_NoInline = VMemKV_Cumulative_Step4;  // No inlining (equivalent to Step 4)

// ─── D. Master List of All Unique Configurations ─────────────────────────
// The unified collection of all unique configuration variants, ensuring no
// duplicate evaluations during tests or benchmarks.
using AllPossibleTypes = std::tuple<VMemKV_Baseline,
                                    VMemKV_Cumulative_Step1,
                                    VMemKV_Cumulative_Step2,
                                    VMemKV_Cumulative_Step3,
                                    VMemKV_Cumulative_Step4,
                                    VMemKV_Ablation_No_AppendMap,
                                    VMemKV_Ablation_No_BloomFilter,
                                    VMemKV_Ablation_No_SimdScan,
                                    VMemKV_Ablation_No_MemoryHints,
                                    VMemKV_T2_InlineAll,
                                    VMemKV_RocksDB>;
}  // namespace variants

}  // namespace vmemkv
