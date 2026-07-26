// test_crash_recovery.cpp — Integration tests for VMemKVImpl WAL-backed crash recovery.
//
// Each scenario simulates a "restart" by constructing two (or more) store
// instances over the same on-disk path -- destroying the first before
// constructing the second, exactly like a process crash-and-restart. Torn
// and corrupted WAL bytes are injected directly via vmemkv::derive_wal_path(),
// bypassing vmemkv::Wal entirely, to simulate a crash mid-append.

#include <doctest/doctest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <vmemkv/vmemkv.hpp>
#include <wal/wal.hpp>

namespace {

auto reserve_crash_temp_path() -> std::filesystem::path {
  static std::atomic<uint64_t> counter{0};
  const uint64_t sequence_number = counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp_path =
      std::filesystem::temp_directory_path() /
      ("vmemkv_crash_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(sequence_number));
  std::error_code ignored;
  std::filesystem::remove(temp_path, ignored);
  std::filesystem::remove(vmemkv::derive_wal_path(temp_path), ignored);
  return temp_path;
}

void cleanup_store_files(const std::filesystem::path &t2_path) {
  std::error_code ignored;
  std::filesystem::remove(t2_path, ignored);
  std::filesystem::remove(vmemkv::derive_wal_path(t2_path), ignored);
}

// Values are always >= 9 bytes so T1InlineValue's <=8B inlining never applies,
// forcing every write through T2 across all variants (including the fully-optimized one).
auto make_value(int index) -> std::string { return "value_" + std::to_string(index) + "_pad"; }

template <typename StorePtr>
auto get_bytes(StorePtr &store, const std::string &key) -> std::optional<std::string> {
  std::optional<std::string> result;
  const bool found = store->get(key, [&](std::span<const std::byte> val) {
    result = std::string(reinterpret_cast<const char *>(val.data()), val.size());
  });
  if (found) {
    return result;
  }
  return std::nullopt;
}

// Simulates a crash mid-append: raw bytes shorter than a full WAL record header, appended
// directly to the WAL file, bypassing vmemkv::Wal.
void append_torn_header_bytes(const std::filesystem::path &t2_path) {
  std::ofstream out(vmemkv::derive_wal_path(t2_path), std::ios::binary | std::ios::app);
  constexpr std::array<char, 10> garbage{};
  out.write(garbage.data(), garbage.size());
}

// Simulates a crash after a well-formed header reached disk but its payload was cut short:
// a full header (correct magic) declaring a longer payload than what actually follows.
void append_torn_payload_bytes(const std::filesystem::path &t2_path) {
  vmemkv::WalRecordHeader header{};
  header.lsn = 0;  // Irrelevant: this record is always discarded as the torn tail.
  header.checksum = 0;
  header.magic = vmemkv::kWalRecordMagic;
  header.key_len = 5;
  header.value_len = 5;
  header.type = static_cast<uint8_t>(vmemkv::WalRecordType::Insert);

  std::ofstream out(vmemkv::derive_wal_path(t2_path), std::ios::binary | std::ios::app);
  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  constexpr std::array<char, 3> partial_payload{'x', 'y', 'z'};
  out.write(partial_payload.data(), partial_payload.size());
}

// Flips the checksum field of the WAL record starting at `record_start_offset`, corrupting it
// while leaving header/payload lengths intact -- simulates bit rot on an already-fsynced record
// rather than a literal crash.
void corrupt_record_checksum(const std::filesystem::path &t2_path, uint64_t record_start_offset) {
  const auto wal_path = vmemkv::derive_wal_path(t2_path);
  const auto offset = static_cast<std::streamoff>(record_start_offset) +
                      static_cast<std::streamoff>(offsetof(vmemkv::WalRecordHeader, checksum));
  std::fstream file(wal_path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(offset);
  char original = 0;
  file.read(&original, 1);
  const char flipped = static_cast<char>(~original);
  file.seekp(offset);
  file.write(&flipped, 1);
}

constexpr uint64_t kStoreCapacityBytes = 64ULL * 1024 * 1024;

}  // namespace

#define CRASH_RECOVERY_STORE_TYPES                                                                                 \
  vmemkv::variants::VMemKV_Var0_Baseline, vmemkv::variants::VMemKV_Var1_Bloom, vmemkv::variants::VMemKV_Var2_Simd, \
      vmemkv::variants::VMemKV_Var3_Inline, vmemkv::variants::VMemKV_Var4_Prefault

TEST_CASE_TEMPLATE("crash recovery: clean restart, empty store stays empty", Store, CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  { auto store = std::make_unique<Store>(path, kStoreCapacityBytes); }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    CHECK_FALSE(get_bytes(store, "anything").has_value());
    CHECK(store->insert("k", make_value(0)));
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: clean restart recovers 100 inserted keys", Store, CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 100;
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
  }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: update then restart persists only the latest value",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    REQUIRE(store->insert("k", make_value(0)));
    REQUIRE(store->update("k", make_value(1)));
    REQUIRE(store->update("k", make_value(2)));
  }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    const auto val = get_bytes(store, "k");
    REQUIRE(val.has_value());
    CHECK(*val == make_value(2));
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: delete then restart tombstone is durable, re-insert works",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    REQUIRE(store->insert("k", make_value(0)));
    REQUIRE(store->remove("k"));
  }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    CHECK_FALSE(get_bytes(store, "k").has_value());
    REQUIRE(store->insert("k", make_value(1)));
    const auto val = get_bytes(store, "k");
    REQUIRE(val.has_value());
    CHECK(*val == make_value(1));
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: insert-delete-reinsert-update replays to final state only",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    REQUIRE(store->insert("k", make_value(0)));
    REQUIRE(store->remove("k"));
    REQUIRE(store->insert("k", make_value(1)));
    REQUIRE(store->update("k", make_value(2)));
  }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    const auto val = get_bytes(store, "k");
    REQUIRE(val.has_value());
    CHECK(*val == make_value(2));
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: torn trailing partial header discarded, prior writes recovered, store usable",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 20;
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
  }
  append_torn_header_bytes(path);
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
    REQUIRE(store->insert("new_key", make_value(999)));
    const auto val = get_bytes(store, "new_key");
    REQUIRE(val.has_value());
    CHECK(*val == make_value(999));
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE(
    "crash recovery: torn trailing full-header-truncated-payload discarded, prior writes recovered, store usable",
    Store,
    CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 20;
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
  }
  append_torn_payload_bytes(path);
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
    REQUIRE(store->insert("new_key", make_value(999)));
    const auto val = get_bytes(store, "new_key");
    REQUIRE(val.has_value());
    CHECK(*val == make_value(999));
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: corrupted checksum on last record discarded, earlier keys intact",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 10;
  uint64_t offset_before_last = 0;
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
    offset_before_last = std::filesystem::file_size(vmemkv::derive_wal_path(path));
    REQUIRE(store->insert("doomed", make_value(999)));
  }
  corrupt_record_checksum(path, offset_before_last);
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
    CHECK_FALSE(get_bytes(store, "doomed").has_value());
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: writes after recovery remain durable across a second restart",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kFirstBatch = 20;
  constexpr int kSecondBatch = 20;
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kFirstBatch; ++i) {
      REQUIRE(store->insert("a" + std::to_string(i), make_value(i)));
    }
  }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kFirstBatch; ++i) {
      REQUIRE(get_bytes(store, "a" + std::to_string(i)).has_value());
    }
    for (int i = 0; i < kSecondBatch; ++i) {
      REQUIRE(store->insert("b" + std::to_string(i), make_value(1000 + i)));
    }
  }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kFirstBatch; ++i) {
      const auto val = get_bytes(store, "a" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
    for (int i = 0; i < kSecondBatch; ++i) {
      const auto val = get_bytes(store, "b" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(1000 + i));
    }
  }
  cleanup_store_files(path);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("crash recovery: concurrent inserts before crash all recover", Store, CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kThreadCount = 8;
  constexpr int kPerThread = 200;
  std::atomic<bool> all_inserts_ok{true};
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      threads.emplace_back([&store, &all_inserts_ok, thread_index]() {
        for (int i = 0; i < kPerThread; ++i) {
          const std::string key = "t" + std::to_string(thread_index) + "_" + std::to_string(i);
          if (!store->insert(key, make_value(thread_index * kPerThread + i))) {
            all_inserts_ok.store(false, std::memory_order_relaxed);
          }
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
  }
  CHECK(all_inserts_ok.load());

  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string key = "t" + std::to_string(thread_index) + "_" + std::to_string(i);
        const auto val = get_bytes(store, key);
        REQUIRE(val.has_value());
        CHECK(*val == make_value(thread_index * kPerThread + i));
      }
    }
  }
  cleanup_store_files(path);
}

TEST_CASE_TEMPLATE("crash recovery: delete-heavy workload restarts to correct split, reorganize still works",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kInsertCount = 100;
  constexpr int kRemoveCount = 50;
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kInsertCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
    for (int i = 0; i < kRemoveCount; ++i) {
      REQUIRE(store->remove("k" + std::to_string(i)));
    }
  }
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    int live_count = 0;
    for (int i = 0; i < kInsertCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      if (i < kRemoveCount) {
        CHECK_FALSE(val.has_value());
      } else {
        REQUIRE(val.has_value());
        CHECK(*val == make_value(i));
        ++live_count;
      }
    }
    CHECK(live_count == kInsertCount - kRemoveCount);

    store->reorganize(false);  // T1-only fast path.
    store->reorganize(true);   // Forced T2 GC.

    for (int i = kRemoveCount; i < kInsertCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
  }
  cleanup_store_files(path);
}

namespace {
struct TinyAppendConfig : vmemkv::Config<> {
  // Partial override style, same as ReorgEveryWriteConfig in test_kv_store.cpp: inherit all
  // defaults and only shrink the append-region capacity to force frequent reorganizes.
  // Both fields must be redeclared: T1AppendCapacityEntries is computed from
  // T1AppendCapacityLog2 inside Config<>'s own scope, so overriding the log2 alone (without
  // also shadowing Entries here) would silently leave APPEND_CAP at the base 2^22 default.
  static constexpr size_t T1AppendCapacityLog2 = 8;  // 256-entry append region.
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << T1AppendCapacityLog2;
};
static_assert(TinyAppendConfig::T1AppendCapacityEntries == (size_t{1} << TinyAppendConfig::T1AppendCapacityLog2));

using VMemKV_TinyAppend = vmemkv::StoreAdapter<vmemkv::VMemKVImpl<TinyAppendConfig>>;
}  // namespace

// Regression test for the append-region-full livelock risk: during recovery no reorg_worker_
// thread exists yet, so replaying into write_entry_lockfree naively (relying on the background
// worker to clear reorg_running_) would hang forever once the WAL holds more live distinct keys
// than one (tiny, 256-entry) append region can hold.
TEST_CASE("crash recovery: recovery under tiny T1 append-region capacity does not hang") {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 5000;
  {
    auto store = std::make_unique<VMemKV_TinyAppend>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
  }
  {
    auto store = std::make_unique<VMemKV_TinyAppend>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
  }
  cleanup_store_files(path);
}

namespace {
// >16 bytes unconditionally bypasses T1's inline-value optimization, forcing every write
// through T2 so a tiny T2 capacity is what actually gets exhausted below.
auto capacity_test_key(int index) -> std::string { return "key_over16bytes_" + std::to_string(index); }
}  // namespace

// Regression test for a WAL-durability-ordering bug: insert_impl()/update_impl() used to call
// wal_.append_insert()/append_update() *before* the T1/T2 mutation was known to succeed. When
// the mutation then threw (T2 physical capacity exceeded, via write_entry_lockfree), the WAL
// already held a durably fsynced "phantom" record for a write that never actually happened.
// Replaying that record on the next restart hit the identical throw, this time from inside the
// constructor -- permanently bricking the store. The fix logs to the WAL only after the mutation
// has actually applied, so a failed insert()/update() leaves no trace in the WAL at all.
TEST_CASE("crash recovery: insert failing on T2 capacity exceeded leaves no phantom WAL record") {
  const auto path = reserve_crash_temp_path();
  constexpr uint64_t kTinyT2Capacity = 200;  // Room for 3 of these ~56B records, not 4.

  bool insert_threw = false;
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kTinyT2Capacity);
    for (int i = 0; i < 3; ++i) {
      REQUIRE(store->insert(capacity_test_key(i), std::string("01234567")));
    }
    try {
      store->insert(capacity_test_key(99), std::string("01234567"));
    } catch (const std::exception &) {
      insert_threw = true;
    }
  }
  REQUIRE(insert_threw);  // Sanity: T2 capacity is genuinely exhausted by the 4th insert.

  // Restarting with the same capacity must succeed outright (no phantom WAL record to replay
  // for the key whose insert() threw), and that key must be absent from the recovered store.
  auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kTinyT2Capacity);
  for (int i = 0; i < 3; ++i) {
    const auto val = get_bytes(store, capacity_test_key(i));
    REQUIRE(val.has_value());
    CHECK(*val == "01234567");
  }
  CHECK_FALSE(get_bytes(store, capacity_test_key(99)).has_value());

  cleanup_store_files(path);
}

// Same bug, but through update_impl()'s write_entry_lockfree() fallback path (value grew too
// large for the in-place t2_.update_value_at() branch, forcing a fresh T2 append instead).
TEST_CASE("crash recovery: update failing on T2 capacity exceeded leaves no phantom WAL record") {
  const auto path = reserve_crash_temp_path();
  constexpr uint64_t kTinyT2Capacity = 200;  // 2 inserts (112B) leaves 88B -- not enough for a 96B growth append.

  bool update_threw = false;
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kTinyT2Capacity);
    REQUIRE(store->insert(capacity_test_key(0), std::string("01234567")));
    REQUIRE(store->insert(capacity_test_key(1), std::string("01234567")));
    try {
      store->update(capacity_test_key(0), std::string(50, 'x'));
    } catch (const std::exception &) {
      update_threw = true;
    }
  }
  REQUIRE(update_threw);  // Sanity: the growth-triggered re-append genuinely exceeds T2 capacity.

  // Restarting must succeed, and key 0 must still hold its pre-update value (the failed update
  // left no phantom WAL record to replay).
  auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kTinyT2Capacity);
  const auto val0 = get_bytes(store, capacity_test_key(0));
  REQUIRE(val0.has_value());
  CHECK(*val0 == "01234567");
  const auto val1 = get_bytes(store, capacity_test_key(1));
  REQUIRE(val1.has_value());
  CHECK(*val1 == "01234567");

  cleanup_store_files(path);
}
