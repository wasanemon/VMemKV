// rocksdb_store.hpp — Thin RocksDB wrapper exposing byte-span APIs for comparison.
//
// Thread safety: RocksDB is internally thread-safe.

#pragma once

#ifdef ENABLE_ROCKSDB
#include <rocksdb/options.h>
#endif

#include "rocksdb_common.hpp"
#include "rocksdb_engine_store.hpp"

namespace vmemkv::rivals {

struct RocksDBPolicy {
  static constexpr const char *kLabel = "RocksDB";
  static constexpr const char *kCloneLabel = "RocksDB (clone)";
#ifdef ENABLE_ROCKSDB
  static auto make_options() -> rocksdb::Options { return rocksdb_common::make_base_benchmark_db_options(); }
#endif
};

}  // namespace vmemkv::rivals

// A derived class (not a type alias) so this satisfies store_adapter.hpp's `class RocksDBStore;`
// forward declaration -- a `using` alias to the template instantiation directly conflicts with
// that forward declaration ("using typedef-name after class").
class RocksDBStore : public vmemkv::rivals::RocksDBEngineStore<vmemkv::rivals::RocksDBPolicy> {
 public:
  using RocksDBEngineStore::RocksDBEngineStore;
};
