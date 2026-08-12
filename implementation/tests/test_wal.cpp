// test_wal.cpp — Isolated correctness tests for the standalone Wal type.
//
// These tests exercise vmemkv::Wal directly (no VMemKVImpl involvement):
// append/fsync semantics, LSN monotonicity across reopen, replay ordering,
// and torn/corrupt trailing record recovery on construction.

#include <doctest/doctest.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <wal/wal.hpp>

#include "test_support.hpp"

namespace {

// Runs `fn` on a background thread; returns true if it finishes within `timeout`, in which case
// the thread is join()ed. Only detaches on timeout (the test under it has already failed by
// then): unconditionally detaching would let a detached thread's TLS teardown still be in flight
// after its work becomes visible, aliasing a later TEST_CASE's reused stack address.
template <typename Fn>
auto run_with_timeout(Fn &&fn, std::chrono::milliseconds timeout) -> bool {
  auto promise = std::make_shared<std::promise<void>>();
  std::future<void> future = promise->get_future();
  std::thread worker([fn = std::forward<Fn>(fn), promise]() mutable {
    fn();
    promise->set_value();
  });
  const bool completed = future.wait_for(timeout) == std::future_status::ready;
  if (completed) {
    worker.join();
  } else {
    worker.detach();
  }
  return completed;
}

// Joins every thread in `threads` (left empty, moved-from, on return), waiting up to `timeout`
// total; returns true if all joined in time. The threads are moved into a shared_ptr the worker
// captures by value, so on timeout the worker keeps them alive and keeps joining on its own after
// this function detaches and returns -- only the worker ever touches them. Having the caller
// detach stragglers itself after a timeout would race the worker's own .join() calls on the same
// objects.
auto join_all_with_timeout(std::vector<std::thread> &threads, std::chrono::milliseconds timeout) -> bool {
  auto promise = std::make_shared<std::promise<void>>();
  std::future<void> future = promise->get_future();
  auto owned_threads = std::make_shared<std::vector<std::thread>>(std::move(threads));
  std::thread worker([owned_threads, promise]() mutable {
    for (auto &thread : *owned_threads) {
      thread.join();
    }
    promise->set_value();
  });
  const bool completed = future.wait_for(timeout) == std::future_status::ready;
  if (completed) {
    worker.join();
  } else {
    worker.detach();
  }
  return completed;
}

auto reserve_wal_path() -> std::filesystem::path { return vmemkv_test::reserve_unique_temp_path("vmemkv_wal_test"); }

using vmemkv_test::as_span;
using vmemkv_test::bytes_of;
using vmemkv_test::span_to_string;

// Thin wrappers around the split reserve_*()/await_durable() API for tests below that only care
// about a record being durably appended, not about separating reservation from the durability
// wait itself.
auto append_insert(vmemkv::Wal &wal, std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t {
  return wal.await_durable(wal.reserve_insert(key, value));
}

auto append_update(vmemkv::Wal &wal, std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t {
  return wal.await_durable(wal.reserve_update(key, value));
}

auto append_delete(vmemkv::Wal &wal, std::span<const std::byte> key) -> uint64_t {
  return wal.await_durable(wal.reserve_delete(key));
}

struct ReplayedRecord {
  vmemkv::WalRecordType type;
  std::string key;
  std::string value;
  uint64_t lsn;
};

}  // namespace

TEST_CASE("Wal: fresh file starts at LSN 1 with empty replay") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);
  CHECK(wal.next_lsn() == 1);

  const uint64_t count = wal.replay([](vmemkv::WalRecordType /*type*/,
                                       std::span<const std::byte> /*key*/,
                                       std::span<const std::byte> /*value*/,
                                       uint64_t /*lsn*/) { FAIL("replay callback invoked on empty WAL"); });
  CHECK(count == 0);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: insert/update/delete assign strictly increasing LSNs") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  const auto key = bytes_of("k1");
  const auto val = bytes_of("v1");

  // Checks relative ordering rather than exact numbers: LSN assignment strictly increasing (and
  // therefore unique) per call is the actual contract callers depend on, not any particular
  // starting value or step size.
  const uint64_t lsn1 = append_insert(wal, as_span(key), as_span(val));
  const uint64_t lsn2 = append_update(wal, as_span(key), as_span(val));
  const uint64_t lsn3 = append_delete(wal, as_span(key));

  CHECK(lsn1 < lsn2);
  CHECK(lsn2 < lsn3);
  CHECK(wal.next_lsn() > lsn3);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: replay after reopen round-trips type/key/value/order") {
  const auto path = reserve_wal_path();
  {
    vmemkv::Wal wal(path);
    append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
    append_update(wal, as_span(bytes_of("b")), as_span(bytes_of("22")));
    append_delete(wal, as_span(bytes_of("c")));
  }

  vmemkv::Wal wal(path);
  std::vector<ReplayedRecord> records;
  const uint64_t count = wal.replay(
      [&](vmemkv::WalRecordType type, std::span<const std::byte> key, std::span<const std::byte> value, uint64_t lsn) {
        records.push_back(ReplayedRecord{type, span_to_string(key), span_to_string(value), lsn});
      });

  CHECK(count == 3);
  REQUIRE(records.size() == 3);

  CHECK(records[0].type == vmemkv::WalRecordType::Insert);
  CHECK(records[0].key == "a");
  CHECK(records[0].value == "1");

  CHECK(records[1].type == vmemkv::WalRecordType::Update);
  CHECK(records[1].key == "b");
  CHECK(records[1].value == "22");

  CHECK(records[2].type == vmemkv::WalRecordType::Delete);
  CHECK(records[2].key == "c");
  CHECK(records[2].value.empty());

  // LSNs must be assigned in the same order the records were appended, but the exact numbers
  // are an implementation detail -- only the relative ordering is part of the contract.
  CHECK(records[0].lsn < records[1].lsn);
  CHECK(records[1].lsn < records[2].lsn);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: delete record replays with empty value span") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);
  append_delete(wal, as_span(bytes_of("gone")));

  bool saw_record = false;
  wal.replay([&](vmemkv::WalRecordType type,
                 std::span<const std::byte> key,
                 std::span<const std::byte> value,
                 uint64_t /*lsn*/) {
    saw_record = true;
    CHECK(type == vmemkv::WalRecordType::Delete);
    CHECK(span_to_string(key) == "gone");
    CHECK(value.empty());
  });
  CHECK(saw_record);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: LSN numbering continues across reopen, does not reset") {
  const auto path = reserve_wal_path();
  uint64_t last_lsn_before_reopen = 0;
  {
    vmemkv::Wal wal(path);
    append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
    last_lsn_before_reopen = append_insert(wal, as_span(bytes_of("b")), as_span(bytes_of("2")));
  }

  vmemkv::Wal wal(path);
  CHECK(wal.next_lsn() > last_lsn_before_reopen);
  const uint64_t lsn = append_insert(wal, as_span(bytes_of("c")), as_span(bytes_of("3")));
  CHECK(lsn > last_lsn_before_reopen);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: repeated open/close with zero appends stays empty") {
  const auto path = reserve_wal_path();
  { vmemkv::Wal wal(path); }
  {
    vmemkv::Wal wal(path);
    CHECK(wal.next_lsn() == 1);
  }

  vmemkv::Wal wal(path);
  const uint64_t count = wal.replay([](vmemkv::WalRecordType /*type*/,
                                       std::span<const std::byte> /*key*/,
                                       std::span<const std::byte> /*value*/,
                                       uint64_t /*lsn*/) { FAIL("replay callback invoked on empty WAL"); });
  CHECK(count == 0);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: torn trailing partial header is discarded and file truncated") {
  const auto path = reserve_wal_path();
  uint64_t valid_end = 0;
  uint64_t first_lsn = 0;
  {
    vmemkv::Wal wal(path);
    first_lsn = append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
    valid_end = std::filesystem::file_size(path);
  }

  // Simulate a crash mid-append: append raw bytes shorter than a full header, bypassing Wal entirely.
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    constexpr std::array<char, 10> garbage{};
    out.write(garbage.data(), garbage.size());
  }
  REQUIRE(std::filesystem::file_size(path) == valid_end + 10);

  vmemkv::Wal wal(path);
  CHECK(std::filesystem::file_size(path) == valid_end);
  CHECK(wal.next_lsn() == first_lsn + 1);

  const uint64_t lsn = append_insert(wal, as_span(bytes_of("b")), as_span(bytes_of("2")));
  CHECK(lsn == first_lsn + 1);

  std::vector<std::string> keys;
  wal.replay([&](vmemkv::WalRecordType /*type*/,
                 std::span<const std::byte> key,
                 std::span<const std::byte> /*value*/,
                 uint64_t /*lsn*/) { keys.push_back(span_to_string(key)); });
  REQUIRE(keys.size() == 2);
  CHECK(keys[0] == "a");
  CHECK(keys[1] == "b");

  std::filesystem::remove(path);
}

TEST_CASE("Wal: torn trailing record with full header but truncated payload is discarded") {
  const auto path = reserve_wal_path();
  uint64_t valid_end = 0;
  uint64_t first_lsn = 0;
  {
    vmemkv::Wal wal(path);
    first_lsn = append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
    valid_end = std::filesystem::file_size(path);
  }

  {
    // The declared lsn on this fabricated header is never read back -- the torn payload check
    // discards the record before its lsn would ever be trusted -- so any value works here.
    vmemkv::WalRecordHeader header{};
    header.lsn = first_lsn + 1;
    header.checksum = 0;
    header.magic = vmemkv::kWalRecordMagic;
    header.key_len = 5;
    header.value_len = 5;
    header.type = static_cast<uint8_t>(vmemkv::WalRecordType::Insert);

    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    // Declares a 10-byte payload but only 3 bytes actually follow -- torn payload.
    constexpr std::array<char, 3> partial_payload{'x', 'y', 'z'};
    out.write(partial_payload.data(), partial_payload.size());
  }

  vmemkv::Wal wal(path);
  CHECK(std::filesystem::file_size(path) == valid_end);
  CHECK(wal.next_lsn() == first_lsn + 1);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: corrupted checksum on trailing record is discarded, earlier records survive") {
  const auto path = reserve_wal_path();
  uint64_t after_first = 0;
  uint64_t first_lsn = 0;
  {
    vmemkv::Wal wal(path);
    first_lsn = append_insert(wal, as_span(bytes_of("keep")), as_span(bytes_of("v1")));
    after_first = std::filesystem::file_size(path);
    append_insert(wal, as_span(bytes_of("corrupt")), as_span(bytes_of("v2")));
  }

  {
    const auto offset = static_cast<std::streamoff>(after_first) +
                        static_cast<std::streamoff>(offsetof(vmemkv::WalRecordHeader, checksum));
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(offset);
    char original = 0;
    file.read(&original, 1);
    const char flipped = static_cast<char>(~original);
    file.seekp(offset);
    file.write(&flipped, 1);
  }

  vmemkv::Wal wal(path);
  CHECK(std::filesystem::file_size(path) == after_first);
  CHECK(wal.next_lsn() == first_lsn + 1);

  std::vector<std::string> keys;
  wal.replay([&](vmemkv::WalRecordType /*type*/,
                 std::span<const std::byte> key,
                 std::span<const std::byte> /*value*/,
                 uint64_t /*lsn*/) { keys.push_back(span_to_string(key)); });
  REQUIRE(keys.size() == 1);
  CHECK(keys[0] == "keep");

  std::filesystem::remove(path);
}

TEST_CASE("Wal: concurrent appends from multiple threads yield unique LSNs that all survive replay") {
  const auto path = reserve_wal_path();
  constexpr int kThreadCount = 8;
  constexpr int kPerThread = 200;
  constexpr int kTotal = kThreadCount * kPerThread;

  // Each thread records the LSNs *returned to it*, not the file's raw byte layout: the actual
  // contract under concurrency is "every acknowledged append gets a distinct LSN and is durably
  // recovered by replay", not "LSNs come out as a gapless 1..N run" -- a real gap could occur
  // if some future append legitimately failed partway (its reserved LSN would never be written),
  // so asserting strict contiguity here would test more than the class actually promises.
  vmemkv::Wal wal(path);
  std::vector<std::vector<uint64_t>> per_thread_lsns(kThreadCount);
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads.emplace_back([&wal, thread_index, &per_thread_lsns]() {
      auto &lsns = per_thread_lsns[static_cast<size_t>(thread_index)];
      lsns.reserve(kPerThread);
      for (int i = 0; i < kPerThread; ++i) {
        const auto key = bytes_of("t" + std::to_string(thread_index) + "_" + std::to_string(i));
        const auto val = bytes_of("v");
        lsns.push_back(append_insert(wal, as_span(key), as_span(val)));
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  std::vector<uint64_t> acknowledged_lsns;
  acknowledged_lsns.reserve(kTotal);
  for (const auto &lsns : per_thread_lsns) {
    acknowledged_lsns.insert(acknowledged_lsns.end(), lsns.begin(), lsns.end());
  }
  REQUIRE(acknowledged_lsns.size() == static_cast<size_t>(kTotal));
  std::sort(acknowledged_lsns.begin(), acknowledged_lsns.end());
  CHECK(std::adjacent_find(acknowledged_lsns.begin(), acknowledged_lsns.end()) ==
        acknowledged_lsns.end());  // No two appends were handed the same LSN.
  CHECK(wal.next_lsn() > acknowledged_lsns.back());

  std::vector<uint64_t> replayed_lsns;
  const uint64_t count = wal.replay([&](vmemkv::WalRecordType /*type*/,
                                        std::span<const std::byte> /*key*/,
                                        std::span<const std::byte> /*value*/,
                                        uint64_t lsn) { replayed_lsns.push_back(lsn); });
  CHECK(count == static_cast<uint64_t>(kTotal));
  std::sort(replayed_lsns.begin(), replayed_lsns.end());
  CHECK(replayed_lsns == acknowledged_lsns);  // Every acknowledged append survives replay, and no others do.

  std::filesystem::remove(path);
}

TEST_CASE("Wal: zero-length value on Insert round-trips distinctly from Delete") {
  const auto path = reserve_wal_path();
  {
    vmemkv::Wal wal(path);
    const std::vector<std::byte> empty_value;
    append_insert(wal, as_span(bytes_of("emptyval")), as_span(empty_value));
  }

  vmemkv::Wal wal(path);
  bool saw_record = false;
  wal.replay([&](vmemkv::WalRecordType type,
                 std::span<const std::byte> key,
                 std::span<const std::byte> value,
                 uint64_t /*lsn*/) {
    saw_record = true;
    CHECK(type == vmemkv::WalRecordType::Insert);
    CHECK(span_to_string(key) == "emptyval");
    CHECK(value.empty());
  });
  CHECK(saw_record);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: large (>64KB) key/value payloads round-trip") {
  const auto path = reserve_wal_path();
  constexpr size_t kLargeSize = 70 * 1024;
  const std::vector<std::byte> big_key(kLargeSize, std::byte{0xAB});
  const std::vector<std::byte> big_value(kLargeSize, std::byte{0xCD});
  {
    vmemkv::Wal wal(path);
    append_insert(wal, as_span(big_key), as_span(big_value));
  }

  vmemkv::Wal wal(path);
  bool saw_record = false;
  wal.replay([&](vmemkv::WalRecordType type,
                 std::span<const std::byte> key,
                 std::span<const std::byte> value,
                 uint64_t /*lsn*/) {
    saw_record = true;
    CHECK(type == vmemkv::WalRecordType::Insert);
    REQUIRE(key.size() == big_key.size());
    REQUIRE(value.size() == big_value.size());
    CHECK(std::equal(key.begin(), key.end(), big_key.begin()));
    CHECK(std::equal(value.begin(), value.end(), big_value.begin()));
  });
  CHECK(saw_record);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: rotate() drops records at or before checkpoint_lsn, keeps the tail") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  const uint64_t lsn_a = append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
  const uint64_t lsn_b = append_insert(wal, as_span(bytes_of("b")), as_span(bytes_of("2")));
  const uint64_t lsn_c = append_insert(wal, as_span(bytes_of("c")), as_span(bytes_of("3")));

  wal.rotate(lsn_b);  // Keep only records with lsn > lsn_b, i.e. just "c".

  std::vector<ReplayedRecord> records;
  const uint64_t count = wal.replay(
      [&](vmemkv::WalRecordType type, std::span<const std::byte> key, std::span<const std::byte> value, uint64_t lsn) {
        records.push_back(ReplayedRecord{type, span_to_string(key), span_to_string(value), lsn});
      });

  CHECK(count == 1);
  REQUIRE(records.size() == 1);
  CHECK(records[0].key == "c");
  CHECK(records[0].lsn == lsn_c);
  CHECK(lsn_a < lsn_b);  // Sanity on the fixture, not the code under test.

  std::filesystem::remove(path);
}

TEST_CASE("Wal: rotate() keeps LSN numbering continuous, does not reset") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
  const uint64_t lsn_b = append_insert(wal, as_span(bytes_of("b")), as_span(bytes_of("2")));

  wal.rotate(lsn_b);
  CHECK(wal.next_lsn() > lsn_b);

  const uint64_t lsn_after = append_insert(wal, as_span(bytes_of("c")), as_span(bytes_of("3")));
  CHECK(lsn_after > lsn_b);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: rotate() with an empty tail (checkpoint_lsn == latest) still preserves LSN continuity") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  const uint64_t lsn_a = append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
  wal.rotate(lsn_a);  // Nothing has a higher lsn -- the rotated file is empty.

  CHECK(wal.next_lsn() > lsn_a);

  const uint64_t count = wal.replay([](vmemkv::WalRecordType /*type*/,
                                       std::span<const std::byte> /*key*/,
                                       std::span<const std::byte> /*value*/,
                                       uint64_t /*lsn*/) { FAIL("replay callback invoked on empty rotated WAL"); });
  CHECK(count == 0);

  const uint64_t lsn_after = append_insert(wal, as_span(bytes_of("b")), as_span(bytes_of("2")));
  CHECK(lsn_after > lsn_a);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: rotate() shrinks the on-disk file size, not just what replay() reports") {
  const auto path = reserve_wal_path();
  uint64_t size_after_one_record = 0;
  uint64_t lsn_first = 0;
  {
    vmemkv::Wal wal(path);
    lsn_first = append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
    size_after_one_record = std::filesystem::file_size(path);
    for (int i = 0; i < 50; ++i) {
      append_insert(wal, as_span(bytes_of("k" + std::to_string(i))), as_span(bytes_of("v")));
    }
    REQUIRE(std::filesystem::file_size(path) > size_after_one_record);

    wal.rotate(lsn_first);  // Drop only the very first record.
  }

  // A fresh Wal reopening the rotated file must also see the correct (shrunk) state -- this
  // is the actual crash-recovery-relevant property, not just the live object's own view.
  vmemkv::Wal reopened(path);
  CHECK(reopened.next_lsn() > lsn_first);
  uint64_t replayed_count = reopened.replay([](vmemkv::WalRecordType /*type*/,
                                               std::span<const std::byte> /*key*/,
                                               std::span<const std::byte> /*value*/,
                                               uint64_t lsn) { CHECK(lsn > 0); });
  CHECK(replayed_count == 50);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: rotate() survives reopen -- rotated file replays correctly from a new Wal instance") {
  const auto path = reserve_wal_path();
  uint64_t lsn_b = 0;
  uint64_t lsn_c = 0;
  {
    vmemkv::Wal wal(path);
    append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
    lsn_b = append_insert(wal, as_span(bytes_of("b")), as_span(bytes_of("2")));
    lsn_c = append_insert(wal, as_span(bytes_of("c")), as_span(bytes_of("3")));
    wal.rotate(lsn_b);
  }

  vmemkv::Wal reopened(path);
  CHECK(reopened.next_lsn() == lsn_c + 1);

  std::vector<std::string> keys;
  reopened.replay([&](vmemkv::WalRecordType /*type*/,
                      std::span<const std::byte> key,
                      std::span<const std::byte> /*value*/,
                      uint64_t /*lsn*/) { keys.push_back(span_to_string(key)); });
  REQUIRE(keys.size() == 1);
  CHECK(keys[0] == "c");

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------------------------
// Group-commit (lock-free ring buffer) regression tests.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Wal: append after reopening a non-empty WAL does not hang") {
  // Regression test: next_to_flush_ must be initialized from next_lsn_ after recovery, not left
  // at its default of 1, or the first append as leader on a reopened non-empty WAL spins forever
  // waiting for ring slots never published in this process.
  const auto path = reserve_wal_path();
  {
    vmemkv::Wal wal(path);
    append_insert(wal, as_span(bytes_of("a")), as_span(bytes_of("1")));
    append_insert(wal, as_span(bytes_of("b")), as_span(bytes_of("2")));
  }

  vmemkv::Wal wal(path);
  const bool completed = run_with_timeout(
      [&wal]() { append_insert(wal, as_span(bytes_of("c")), as_span(bytes_of("3"))); }, std::chrono::seconds(5));
  REQUIRE(completed);

  std::vector<std::string> keys;
  wal.replay([&](vmemkv::WalRecordType /*type*/,
                 std::span<const std::byte> key,
                 std::span<const std::byte> /*value*/,
                 uint64_t /*lsn*/) { keys.push_back(span_to_string(key)); });
  REQUIRE(keys.size() == 3);
  CHECK(keys[2] == "c");

  std::filesystem::remove(path);
}

TEST_CASE("Wal: concurrent appends replay with exactly the content each thread wrote, keyed by LSN") {
  // Strengthens the "concurrent appends yield unique LSNs" test above by also verifying
  // per-record content -- catches a group-commit batching bug (e.g. two records' bytes swapped
  // between slots) that a set-of-LSNs check alone would miss.
  const auto path = reserve_wal_path();
  constexpr int kThreadCount = 32;
  constexpr int kPerThread = 50;

  vmemkv::Wal wal(path);
  std::mutex recorded_mutex;
  std::unordered_map<uint64_t, ReplayedRecord> recorded_by_lsn;

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads.emplace_back([&, thread_index]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string key_str = "t" + std::to_string(thread_index) + "_" + std::to_string(i);
        const std::string value_str = "v" + std::to_string(thread_index) + "_" + std::to_string(i);
        const auto key = bytes_of(key_str);
        const auto val = bytes_of(value_str);
        const uint64_t lsn = append_insert(wal, as_span(key), as_span(val));
        std::lock_guard<std::mutex> lock(recorded_mutex);
        recorded_by_lsn[lsn] = ReplayedRecord{vmemkv::WalRecordType::Insert, key_str, value_str, lsn};
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  REQUIRE(recorded_by_lsn.size() == static_cast<size_t>(kThreadCount * kPerThread));

  std::unordered_map<uint64_t, ReplayedRecord> replayed_by_lsn;
  wal.replay(
      [&](vmemkv::WalRecordType type, std::span<const std::byte> key, std::span<const std::byte> value, uint64_t lsn) {
        replayed_by_lsn[lsn] = ReplayedRecord{type, span_to_string(key), span_to_string(value), lsn};
      });

  REQUIRE(replayed_by_lsn.size() == recorded_by_lsn.size());
  for (const auto &[lsn, expected] : recorded_by_lsn) {
    const auto it = replayed_by_lsn.find(lsn);
    REQUIRE(it != replayed_by_lsn.end());
    CHECK(it->second.type == expected.type);
    CHECK(it->second.key == expected.key);
    CHECK(it->second.value == expected.value);
  }

  std::filesystem::remove(path);
}

TEST_CASE("Wal: append storm exceeding ring capacity does not corrupt or duplicate records") {
  // Regression test: without bounding each leader collection pass to at most kWalRingCapacity
  // records, a burst of appends large enough to wrap the ring before any flush would let the
  // leader misinterpret a stale slot as the LSN it's seeking -- writing the wrong record's bytes
  // under the wrong LSN and stranding the record that actually owned it.
  //
  // Threads are joined via join_all_with_timeout() rather than detached (see its comment) to
  // avoid a TSan-caught false data race from a still-tearing-down detached thread.
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  // Must exceed the ring's capacity (currently 4096 -- see wal.hpp's kWalRingCapacity) by a
  // comfortable margin.
  constexpr int kAppendCount = 4096 + 512;

  std::mutex recorded_mutex;
  std::unordered_map<uint64_t, std::string> recorded_by_lsn;

  std::vector<std::thread> threads;
  threads.reserve(kAppendCount);
  for (int i = 0; i < kAppendCount; ++i) {
    threads.emplace_back([&, i]() {
      const std::string key_str = "k" + std::to_string(i);
      const auto key = bytes_of(key_str);
      const auto val = bytes_of("v");
      const uint64_t lsn = append_insert(wal, as_span(key), as_span(val));
      std::lock_guard<std::mutex> lock(recorded_mutex);
      recorded_by_lsn[lsn] = key_str;
    });
  }

  // Generous timeout: this test's high thread count is exactly what TSan's per-thread bookkeeping
  // overhead hits hardest -- comfortably under a second normally, over a minute under TSan.
  REQUIRE(join_all_with_timeout(threads, std::chrono::minutes(10)));
  REQUIRE(recorded_by_lsn.size() == static_cast<size_t>(kAppendCount));  // every LSN was unique

  std::unordered_map<uint64_t, std::string> replayed_by_lsn;
  const uint64_t count = wal.replay([&](vmemkv::WalRecordType /*type*/,
                                        std::span<const std::byte> key,
                                        std::span<const std::byte> /*value*/,
                                        uint64_t lsn) { replayed_by_lsn[lsn] = span_to_string(key); });

  CHECK(count == static_cast<uint64_t>(kAppendCount));
  REQUIRE(replayed_by_lsn.size() == recorded_by_lsn.size());
  for (const auto &[lsn, expected_key] : recorded_by_lsn) {
    const auto it = replayed_by_lsn.find(lsn);
    REQUIRE(it != replayed_by_lsn.end());
    // Exactly the key this LSN's caller wrote -- not some other, stale record aliased in from a
    // wrapped-around ring slot.
    CHECK(it->second == expected_key);
  }

  std::filesystem::remove(path);
}

TEST_CASE("Wal: rotate() under concurrent appends returns promptly and preserves the tail") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> appended_count{0};
  std::mutex recorded_mutex;
  std::vector<uint64_t> all_lsns;

  // Kept joinable (never detached) so this test can never race a later TEST_CASE's stack -- see
  // run_with_timeout()'s comment for the class of bug this avoids.
  std::thread writer([&]() {
    int i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const auto key = bytes_of("k" + std::to_string(i));
      const auto val = bytes_of("v");
      const uint64_t lsn = append_insert(wal, as_span(key), as_span(val));
      {
        std::lock_guard<std::mutex> lock(recorded_mutex);
        all_lsns.push_back(lsn);
      }
      appended_count.fetch_add(1, std::memory_order_relaxed);
      ++i;
    }
  });

  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (appended_count.load(std::memory_order_relaxed) < 50 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(appended_count.load(std::memory_order_relaxed) >= 50);
  }

  uint64_t checkpoint_lsn = 0;
  {
    std::lock_guard<std::mutex> lock(recorded_mutex);
    checkpoint_lsn = all_lsns[all_lsns.size() / 2];
  }

  const bool rotate_completed = run_with_timeout([&]() { wal.rotate(checkpoint_lsn); }, std::chrono::seconds(10));
  REQUIRE(rotate_completed);

  stop.store(true, std::memory_order_relaxed);
  const bool writer_stopped = run_with_timeout([&]() { writer.join(); }, std::chrono::seconds(10));
  REQUIRE(writer_stopped);

  std::vector<uint64_t> replayed_lsns;
  wal.replay([&](vmemkv::WalRecordType /*type*/,
                 std::span<const std::byte> /*key*/,
                 std::span<const std::byte> /*value*/,
                 uint64_t lsn) { replayed_lsns.push_back(lsn); });

  std::sort(replayed_lsns.begin(), replayed_lsns.end());
  CHECK(std::adjacent_find(replayed_lsns.begin(), replayed_lsns.end()) == replayed_lsns.end());  // no duplicates
  for (uint64_t lsn : replayed_lsns) {
    CHECK(lsn > checkpoint_lsn);  // rotate() must have dropped everything at or before checkpoint_lsn
  }

  std::filesystem::remove(path);
}

TEST_CASE("Wal: write/fsync failure poisons the Wal and fails outstanding callers without hanging") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  // Establish a small baseline so later writes past this size trigger EFBIG.
  append_insert(wal, as_span(bytes_of("seed")), as_span(bytes_of("1")));
  const auto size_after_seed = static_cast<rlim_t>(std::filesystem::file_size(path));

  struct rlimit original_limit {};
  REQUIRE(getrlimit(RLIMIT_FSIZE, &original_limit) == 0);
  struct sigaction original_sigxfsz {};
  REQUIRE(sigaction(SIGXFSZ, nullptr, &original_sigxfsz) == 0);

  struct RestoreGuard {
    struct rlimit limit;
    struct sigaction sigxfsz;
    ~RestoreGuard() {
      setrlimit(RLIMIT_FSIZE, &limit);
      sigaction(SIGXFSZ, &sigxfsz, nullptr);
    }
  } restore_guard{original_limit, original_sigxfsz};

  // Writing past RLIMIT_FSIZE raises SIGXFSZ by default, which would kill this test process
  // outright -- ignore it so the write() syscall instead just fails with EFBIG.
  struct sigaction ignore_action {};
  ignore_action.sa_handler = SIG_IGN;
  REQUIRE(sigaction(SIGXFSZ, &ignore_action, nullptr) == 0);

  struct rlimit tight_limit = original_limit;
  tight_limit.rlim_cur = size_after_seed + 4;  // just barely past the seed record
  REQUIRE(setrlimit(RLIMIT_FSIZE, &tight_limit) == 0);

  // Several concurrent appends, all of which should now fail (write() past the limit returns
  // EFBIG) rather than hang. Kept joinable and joined via join_all_with_timeout() -- see the
  // ring-capacity test's comment and run_with_timeout()'s for why (never detach-and-poll).
  constexpr int kThreadCount = 8;
  std::atomic<int> threw{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&, i]() {
      try {
        append_insert(wal, as_span(bytes_of("x" + std::to_string(i))), as_span(bytes_of("y")));
      } catch (...) {
        threw.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  REQUIRE(join_all_with_timeout(threads, std::chrono::seconds(10)));
  CHECK(threw.load(std::memory_order_relaxed) == kThreadCount);  // every one of them must have failed

  // Restore the real limit before any further syscalls in this test.
  REQUIRE(setrlimit(RLIMIT_FSIZE, &original_limit) == 0);

  // The Wal must now be permanently poisoned: any further append fails immediately, without
  // attempting another syscall.
  CHECK_THROWS(append_insert(wal, as_span(bytes_of("after")), as_span(bytes_of("z"))));

  std::filesystem::remove(path);
}
