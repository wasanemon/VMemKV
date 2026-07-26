// test_wal.cpp — Isolated correctness tests for the standalone Wal type.
//
// These tests exercise vmemkv::Wal directly (no VMemKVImpl involvement):
// append/fsync semantics, LSN monotonicity across reopen, replay ordering,
// and torn/corrupt trailing record recovery on construction.

#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <wal/wal.hpp>

namespace {

auto reserve_wal_path() -> std::filesystem::path {
  static std::atomic<uint64_t> counter{0};
  const uint64_t sequence_number = counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp_path =
      std::filesystem::temp_directory_path() /
      ("vmemkv_wal_test_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(sequence_number));
  std::error_code ignored;
  std::filesystem::remove(temp_path, ignored);
  return temp_path;
}

auto bytes_of(std::string_view value) -> std::vector<std::byte> {
  std::vector<std::byte> out(value.size());
  std::memcpy(out.data(), value.data(), value.size());
  return out;
}

auto as_span(const std::vector<std::byte> &value) -> std::span<const std::byte> {
  return std::span<const std::byte>(value.data(), value.size());
}

auto span_to_string(std::span<const std::byte> value) -> std::string {
  return {reinterpret_cast<const char *>(value.data()), value.size()};
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

TEST_CASE("Wal: append_insert/update/delete assign strictly increasing LSNs") {
  const auto path = reserve_wal_path();
  vmemkv::Wal wal(path);

  const auto key = bytes_of("k1");
  const auto val = bytes_of("v1");

  // Checks relative ordering rather than exact numbers: LSN assignment strictly increasing (and
  // therefore unique) per call is the actual contract callers depend on, not any particular
  // starting value or step size.
  const uint64_t lsn1 = wal.append_insert(as_span(key), as_span(val));
  const uint64_t lsn2 = wal.append_update(as_span(key), as_span(val));
  const uint64_t lsn3 = wal.append_delete(as_span(key));

  CHECK(lsn1 < lsn2);
  CHECK(lsn2 < lsn3);
  CHECK(wal.next_lsn() > lsn3);

  std::filesystem::remove(path);
}

TEST_CASE("Wal: replay after reopen round-trips type/key/value/order") {
  const auto path = reserve_wal_path();
  {
    vmemkv::Wal wal(path);
    wal.append_insert(as_span(bytes_of("a")), as_span(bytes_of("1")));
    wal.append_update(as_span(bytes_of("b")), as_span(bytes_of("22")));
    wal.append_delete(as_span(bytes_of("c")));
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
  wal.append_delete(as_span(bytes_of("gone")));

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
    wal.append_insert(as_span(bytes_of("a")), as_span(bytes_of("1")));
    last_lsn_before_reopen = wal.append_insert(as_span(bytes_of("b")), as_span(bytes_of("2")));
  }

  vmemkv::Wal wal(path);
  CHECK(wal.next_lsn() > last_lsn_before_reopen);
  const uint64_t lsn = wal.append_insert(as_span(bytes_of("c")), as_span(bytes_of("3")));
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
    first_lsn = wal.append_insert(as_span(bytes_of("a")), as_span(bytes_of("1")));
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

  const uint64_t lsn = wal.append_insert(as_span(bytes_of("b")), as_span(bytes_of("2")));
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
    first_lsn = wal.append_insert(as_span(bytes_of("a")), as_span(bytes_of("1")));
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
    first_lsn = wal.append_insert(as_span(bytes_of("keep")), as_span(bytes_of("v1")));
    after_first = std::filesystem::file_size(path);
    wal.append_insert(as_span(bytes_of("corrupt")), as_span(bytes_of("v2")));
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
        lsns.push_back(wal.append_insert(as_span(key), as_span(val)));
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
    wal.append_insert(as_span(bytes_of("emptyval")), as_span(empty_value));
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
    wal.append_insert(as_span(big_key), as_span(big_value));
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
