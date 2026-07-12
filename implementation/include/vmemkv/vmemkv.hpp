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
// ─── 1. Base Store & RocksDB ───
using VMemKV_Baseline = VMemKVBaselineStore;
using VMemKV_RocksDB = VMemKVRocksDB;

// ─── 2. In-Memory Evaluation Targets (DRAM Ablation) ───
using VMemKV_InMem_Simd = StoreAdapter<VMemKVImpl<Config<BloomFilter, SimdScan>>>;
using VMemKV_InMem_Inline = StoreAdapter<VMemKVImpl<Config<BloomFilter, SimdScan, T1InlineValue>>>;
using VMemKV_InMem_Prefault = StoreAdapter<VMemKVImpl<Config<BloomFilter, SimdScan, T1InlineValue, Prefaulting>>>;

// ─── 3. Larger-than-Memory Evaluation Targets (LTM Ablation) ───
using VMemKV_LTM_Baseline = StoreAdapter<VMemKVImpl<Config<SimdScan>>>;
using VMemKV_LTM_Bloom = StoreAdapter<VMemKVImpl<Config<SimdScan, BloomFilter>>>;
using VMemKV_LTM_Hints = StoreAdapter<VMemKVImpl<Config<SimdScan, BloomFilter, MemoryHints>>>;
using VMemKV_LTM_Inline = VMemKVStore;  // Fully optimized production configuration
using VMemKV_LTM_Prefault =
    StoreAdapter<VMemKVImpl<Config<SimdScan, BloomFilter, MemoryHints, T1InlineValue, Prefaulting>>>;

// ─── 4. Unified Benchmark Registration Tuple ───
using AllPossibleTypes = std::tuple<VMemKV_Baseline,
                                    VMemKV_InMem_Simd,
                                    VMemKV_InMem_Inline,
                                    VMemKV_InMem_Prefault,
                                    VMemKV_LTM_Baseline,
                                    VMemKV_LTM_Bloom,
                                    VMemKV_LTM_Hints,
                                    VMemKV_LTM_Inline,
                                    VMemKV_LTM_Prefault,
                                    VMemKV_RocksDB>;
}  // namespace variants

}  // namespace vmemkv
