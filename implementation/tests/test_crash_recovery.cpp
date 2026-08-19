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
#include <checkpoint/checkpoint.hpp>
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

#include "test_support.hpp"

namespace {

auto reserve_crash_temp_path() -> std::filesystem::path {
  return vmemkv_test::reserve_unique_temp_path("vmemkv_crash", /*also_remove_wal_sibling=*/true);
}

void cleanup_store_files(const std::filesystem::path &t2_path) {
  std::error_code ignored;
  std::filesystem::remove(t2_path, ignored);
  std::filesystem::remove(vmemkv::derive_wal_path(t2_path), ignored);
  std::filesystem::remove(vmemkv::derive_manifest_path(t2_path), ignored);
  std::filesystem::remove(vmemkv::derive_t1_chk_path(t2_path), ignored);
  std::filesystem::remove(vmemkv::derive_t2_chk_path(t2_path), ignored);
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

#define CRASH_RECOVERY_STORE_TYPES \
  vmemkv::variants::VMemKV_Var0_Baseline, vmemkv::variants::VMemKV_Var1_Bloom, vmemkv::variants::VMemKV_Var2_Inline

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

    store->reorganize();  // T1-only fast path.
    store->defragment();  // T1-only placeholder (5.2) -- exercised here for survival, not GC.

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
  // Shrinks append-region capacity to force frequent reorganizes. Both fields must be
  // redeclared: Entries is computed from Log2 inside Config<>'s own scope.
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

// Regression test: insert_impl()/update_impl() used to log to the WAL before the T1/T2 mutation
// was known to succeed. A failed mutation (e.g. T2 capacity exceeded) left a durably-fsynced
// "phantom" WAL record whose replay hit the identical throw on the next restart, permanently
// bricking the store. Fixed by logging only after the mutation actually applies.
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

// ─── Checkpoint / Reload integration tests ──────────────────────────────────────────────────
// See docs/specification/low_level_design.md 5.2-5.5. These exercise the checkpoint mechanism
// entirely through the public Store interface plus filesystem inspection of the manifest and the
// checkpoint files it produces -- never through VMemKVImpl internals directly.

TEST_CASE_TEMPLATE("checkpoint: explicit checkpoint() persists a manifest and survives restart",
                   Store,
                   CRASH_RECOVERY_STORE_TYPES) {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 50;
  {
    auto store = std::make_unique<Store>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
    store->impl().checkpoint();
    CHECK(std::filesystem::exists(vmemkv::derive_manifest_path(path)));
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

TEST_CASE("checkpoint: rotates the WAL down to just the post-checkpoint tail") {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 200;
  auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
  for (int i = 0; i < kKeyCount; ++i) {
    REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
  }
  const uint64_t wal_size_before = std::filesystem::file_size(vmemkv::derive_wal_path(path));

  store->impl().checkpoint();

  const uint64_t wal_size_after = std::filesystem::file_size(vmemkv::derive_wal_path(path));
  CHECK(wal_size_after < wal_size_before);  // Rotation dropped everything up to checkpoint_lsn.

  cleanup_store_files(path);
}

TEST_CASE("checkpoint: a second checkpoint reuses the same T1/T2 checkpoint files") {
  const auto path = reserve_crash_temp_path();
  auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
  for (int i = 0; i < 20; ++i) {
    REQUIRE(store->insert("a" + std::to_string(i), make_value(i)));
  }
  store->impl().checkpoint();
  const auto manifest1 = vmemkv::read_manifest(vmemkv::derive_manifest_path(path));
  REQUIRE(manifest1.has_value());
  const auto t1_chk_path = vmemkv::derive_t1_chk_path(path);
  const auto t2_chk_path = vmemkv::derive_t2_chk_path(path);
  REQUIRE(std::filesystem::exists(t1_chk_path));
  REQUIRE(std::filesystem::exists(t2_chk_path));

  for (int i = 0; i < 20; ++i) {
    REQUIRE(store->insert("b" + std::to_string(i), make_value(1000 + i)));
  }
  store->impl().checkpoint();
  const auto manifest2 = vmemkv::read_manifest(vmemkv::derive_manifest_path(path));
  REQUIRE(manifest2.has_value());
  CHECK(manifest2->generation != manifest1->generation);  // checkpoint_lsn still advances...

  // ...but the checkpoint files themselves are the same paths every cycle -- checkpoint_internal()
  // durabilizes in place rather than building a new file per generation.
  CHECK(std::filesystem::exists(t1_chk_path));
  CHECK(std::filesystem::exists(t2_chk_path));

  cleanup_store_files(path);
}

TEST_CASE("checkpoint: deletes and updates after a checkpoint are correctly reflected after restart") {
  const auto path = reserve_crash_temp_path();
  constexpr int kKeyCount = 30;
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount; ++i) {
      REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
    }
    store->impl().checkpoint();  // Checkpoint captures all 30 keys.

    // Tail activity after the checkpoint: delete half, update the rest.
    for (int i = 0; i < kKeyCount / 2; ++i) {
      REQUIRE(store->remove("k" + std::to_string(i)));
    }
    for (int i = kKeyCount / 2; i < kKeyCount; ++i) {
      REQUIRE(store->update("k" + std::to_string(i), make_value(1000 + i)));
    }
  }
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
    for (int i = 0; i < kKeyCount / 2; ++i) {
      CHECK_FALSE(get_bytes(store, "k" + std::to_string(i)).has_value());
    }
    for (int i = kKeyCount / 2; i < kKeyCount; ++i) {
      const auto val = get_bytes(store, "k" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(1000 + i));
    }
  }
  cleanup_store_files(path);
}

TEST_CASE("checkpoint: insert/checkpoint/insert-more/restart preserves both pre- and post-checkpoint keys") {
  const auto path = reserve_crash_temp_path();
  constexpr int kFirstBatch = 20;
  constexpr int kSecondBatch = 20;
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
    for (int i = 0; i < kFirstBatch; ++i) {
      REQUIRE(store->insert("a" + std::to_string(i), make_value(i)));
    }
    store->impl().checkpoint();
    for (int i = 0; i < kSecondBatch; ++i) {
      REQUIRE(store->insert("b" + std::to_string(i), make_value(1000 + i)));
    }
  }
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
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

TEST_CASE("checkpoint: a valid manifest pointing at a missing T1 checkpoint file fails construction loudly") {
  const auto path = reserve_crash_temp_path();
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
    REQUIRE(store->insert("k", make_value(0)));
    store->impl().checkpoint();
  }
  const auto manifest = vmemkv::read_manifest(vmemkv::derive_manifest_path(path));
  REQUIRE(manifest.has_value());
  // Simulate loss/corruption of the T1 checkpoint file. Since the WAL has already been rotated
  // down to the tail, falling back to full replay would silently lose the pre-checkpoint key --
  // construction must fail loudly instead (see load_checkpoint_if_present).
  std::filesystem::remove(vmemkv::derive_t1_chk_path(path));

  CHECK_THROWS(std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes));

  cleanup_store_files(path);
}

namespace {
struct TinyWalCheckpointConfig : vmemkv::Config<> {
  static constexpr size_t WalMaxBytesSinceCheckpoint = 2048;  // Small enough to trip quickly.
};
using VMemKV_TinyWalCheckpoint = vmemkv::StoreAdapter<vmemkv::VMemKVImpl<TinyWalCheckpointConfig>>;
}  // namespace

// Regression test for checkpoint()'s first-ever call: this store has never committed a checkpoint
// before, so checkpoint_internal() must create the T2 checkpoint file (O_CREAT) rather than
// assume one already exists, even for an insert-only workload that never generates fragmentation
// on its own.
TEST_CASE("checkpoint(): first call on a fresh store creates the T2 checkpoint file") {
  const auto path = reserve_crash_temp_path();
  auto store = std::make_unique<VMemKV_TinyWalCheckpoint>(path, kStoreCapacityBytes);
  for (int i = 0; i < 200; ++i) {
    REQUIRE(store->insert("k" + std::to_string(i), make_value(i)));
  }
  const auto stats_before = store->impl().get_statistics();
  store->impl().checkpoint();
  const auto stats_after = store->impl().get_statistics();

  CHECK(std::filesystem::exists(vmemkv::derive_manifest_path(path)));
  CHECK(stats_after.t2_reorg_count == stats_before.t2_reorg_count + 1);
  CHECK(std::filesystem::exists(vmemkv::derive_t2_chk_path(path)));

  for (int i = 0; i < 200; ++i) {
    const auto val = get_bytes(store, "k" + std::to_string(i));
    REQUIRE(val.has_value());
    CHECK(*val == make_value(i));
  }

  cleanup_store_files(path);
}

// Regression test: repeated checkpoint() calls each durabilize the tail written since the last
// cycle in place (same T1/T2 checkpoint file paths throughout, checkpoint_lsn still advancing
// every cycle), and a restart after several cycles correctly adopts the final state.
TEST_CASE("checkpoint: repeated checkpoint() calls durabilize in place and survive restart") {
  const auto path = reserve_crash_temp_path();
  auto store = std::make_unique<VMemKV_TinyWalCheckpoint>(path, kStoreCapacityBytes);

  REQUIRE(store->insert("seed", make_value(-1)));
  store->impl().checkpoint();
  const auto manifest1 = vmemkv::read_manifest(vmemkv::derive_manifest_path(path));
  REQUIRE(manifest1.has_value());
  const auto t2_chk_path = vmemkv::derive_t2_chk_path(path);
  REQUIRE(std::filesystem::exists(t2_chk_path));

  const auto stats_after_seed = store->impl().get_statistics();

  // Cycles 2-4: insert-only, then an explicit checkpoint() call each round.
  constexpr int kCycles = 3;
  constexpr int kKeysPerCycle = 200;
  uint64_t last_generation = manifest1->generation;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    for (int i = 0; i < kKeysPerCycle; ++i) {
      REQUIRE(store->insert("c" + std::to_string(cycle) + "_" + std::to_string(i), make_value(i)));
    }
    store->impl().checkpoint();

    const auto manifest_now = vmemkv::read_manifest(vmemkv::derive_manifest_path(path));
    REQUIRE(manifest_now.has_value());
    CHECK(manifest_now->generation != last_generation);  // checkpoint_lsn advances every cycle.
    last_generation = manifest_now->generation;
    REQUIRE(std::filesystem::exists(t2_chk_path));  // Same file, every cycle.
  }

  const auto stats_after_cycles = store->impl().get_statistics();
  CHECK(stats_after_cycles.t2_reorg_count ==
        stats_after_seed.t2_reorg_count + kCycles);  // Every checkpoint() ran a cycle.

  // A genuine restart adopts the final checkpoint correctly.
  store.reset();
  auto restarted = std::make_unique<VMemKV_TinyWalCheckpoint>(path, kStoreCapacityBytes);
  CHECK(get_bytes(restarted, "seed").has_value());
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    for (int i = 0; i < kKeysPerCycle; ++i) {
      const auto val = get_bytes(restarted, "c" + std::to_string(cycle) + "_" + std::to_string(i));
      REQUIRE(val.has_value());
      CHECK(*val == make_value(i));
    }
  }

  cleanup_store_files(path);
}

// Regression test for a real race, confirmed via direct reproduction (not just reasoned about):
// checkpoint_internal()'s pre-stop pass (copy_live_entries(/*destructive=*/false)) used to capture
// a tail-resident record's current bytes and mark it durabilized with no barrier stopping an
// in-place update from landing on that exact offset immediately afterward; the post-stop pass
// then skipped it (already durabilized), so the checkpoint file kept the stale, pre-update bytes.
// Once the cycle published a fresh T2Memory generation with base_boundary advanced past this
// offset, the record became base-resident -- read through the seqlock-free base-region mmaps
// (7.9), which reflect the *file's* stale bytes, not the in-place update's private COW page. Live
// reads reverted to the pre-update value immediately, with no crash or restart involved; only a
// subsequent restart (WAL replay of the update, whose LSN is always > checkpoint_lsn) corrected
// it.
//
// Fixed on the write side (capture_watermark_, see its own declaration and
// try_in_place_update()'s allow_in_place check): checkpoint_internal() claims a record's offset
// -- advancing capture_watermark_ past it -- *before* reading it, so any in-place update racing
// that exact offset is guaranteed to see allow_in_place==false and redirect out-of-place instead
// of mutating a record the checkpoint has already (or is about to) durabilize. This makes the
// race impossible rather than detecting and correcting it after the fact.
TEST_CASE(
    "checkpoint: an in-place update racing the pre-stop/writer-stop window stays visible live and survives a restart") {
  const auto path = reserve_crash_temp_path();
  const std::string v1(200, 'a');
  const std::string v2(200, 'b');
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
    REQUIRE(store->insert("racer", v1));  // Tail-resident: no checkpoint has run yet.

    std::thread racer;
    store->impl().reorganize_internal(
        /*do_checkpoint=*/true,
        /*pre_stop_hook=*/[&] {
          // Fires after the pre-stop copy_live_entries() pass has already captured "racer"=v1
          // and before stop_writers_and_wait() blocks new writers -- the exact window under
          // suspicion. update() itself waits for WAL durability, so joining here is sufficient
          // to guarantee the racing write is fully applied (T2 and WAL) before this hook returns.
          racer = std::thread([&] { REQUIRE(store->update("racer", v2)); });
          racer.join();
        });

    // Live reads must already reflect v2 regardless of what the checkpoint file captured -- T2's
    // live mapping was updated in place independently of checkpoint_internal()'s file I/O.
    const auto live = get_bytes(store, "racer");
    REQUIRE(live.has_value());
    CHECK(*live == v2);
  }
  // Restart without another checkpoint: if the checkpoint file's "racer" slot captured a stale
  // v1 and the store trusted it without correction, this would read back v1.
  {
    auto store = std::make_unique<vmemkv::variants::VMemKV_Var0_Baseline>(path, kStoreCapacityBytes);
    const auto val = get_bytes(store, "racer");
    REQUIRE(val.has_value());
    CHECK(*val == v2);
  }
  cleanup_store_files(path);
}
