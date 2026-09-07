// test_t2_flat_file.cpp — Isolated correctness tests for the standalone T2FlatFile type.
//
// These tests exercise vmemkv::T2FlatFile directly (no VMemKVImpl/T1Index/WAL involvement):
// append_default record round-tripping and in-place update_value_at.

#include <doctest/doctest.h>

#include <api/store_adapter.hpp>  // kInlineScalarValueBytes, needed by vmemkv_impl.hpp below (get_impl()'s inline-value path)
#include <filesystem>
#include <span>
#include <string>
#include <t2_flat_file/t2_flat_file.hpp>
#include <vector>
#include <vmemkv_impl.hpp>  // for the free function vmemkv::read_t2_record_seqlock() only

#include "test_support.hpp"

namespace {

auto reserve_t2_path() -> std::filesystem::path { return vmemkv_test::reserve_unique_temp_path("vmemkv_t2_test"); }

using vmemkv_test::as_span;
using vmemkv_test::bytes_of;
using vmemkv_test::span_to_string;

constexpr uint64_t kTestCapacityBytes = 4ULL * 1024 * 1024;

struct TempT2File {
  std::filesystem::path path = reserve_t2_path();
  vmemkv::T2FlatFile file{path, kTestCapacityBytes};

  ~TempT2File() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

}  // namespace

TEST_CASE("T2FlatFile: append_default then at() round-trips key/value") {
  TempT2File t2;
  auto mem = t2.file.get_memory_handle();
  const uint64_t offset = t2.file.append_default(mem, as_span(bytes_of("hello")), as_span(bytes_of("world")));

  const auto record = t2.file.at(offset, mem);
  CHECK(span_to_string(record.key) == "hello");
  CHECK(span_to_string(record.value) == "world");
}

TEST_CASE("T2FlatFile: update_value_at overwrites in place when the new value fits alloc_len") {
  TempT2File t2;
  auto mem = t2.file.get_memory_handle();
  const uint64_t offset = t2.file.append_default(mem, as_span(bytes_of("k")), as_span(bytes_of("0123456789")));

  CHECK(vmemkv::T2FlatFile::update_value_at(offset, as_span(bytes_of("abc")), mem));

  CHECK(span_to_string(t2.file.at(offset, mem).value) == "abc");
}

TEST_CASE("T2FlatFile: update_value_at fails when the new value exceeds alloc_len") {
  TempT2File t2;
  auto mem = t2.file.get_memory_handle();
  const uint64_t offset = t2.file.append_default(mem, as_span(bytes_of("k")), as_span(bytes_of("abc")));

  CHECK_FALSE(vmemkv::T2FlatFile::update_value_at(offset, as_span(bytes_of("0123456789")), mem));
}

// read_t2_record_seqlock() takes an AtFunc supplier called fresh on every retry, so key_len/
// value_len are always re-read alongside the version recheck. This deterministically shrinks a
// record via update_value_at() then confirms read_t2_record_seqlock() always returns the
// correct, current size.
TEST_CASE(
    "T2FlatFile: read_t2_record_seqlock always observes the current value, even immediately "
    "after a shrinking update_value_at() (regression)") {
  TempT2File t2;
  auto mem = t2.file.get_memory_handle();
  const std::string original_value(64, 'A');  // alloc_len becomes 64.
  const uint64_t offset = t2.file.append_default(mem, as_span(bytes_of("k")), as_span(bytes_of(original_value)));

  const std::string shrunk_value = "new";  // Fits within alloc_len=64; update_value_at() succeeds.
  REQUIRE(vmemkv::T2FlatFile::update_value_at(offset, as_span(bytes_of(shrunk_value)), mem));

  // read_t2_record_seqlock() calls at_func() itself, fresh, so it always sees the record as it
  // is *right now*.
  std::vector<std::byte> copied;
  vmemkv::read_t2_record_seqlock([&]() -> T2RecordView { return t2.file.at(offset, mem); },
                                 [&](const T2RecordView &record) -> bool {
                                   copied.assign(record.value.begin(), record.value.end());
                                   return true;
                                 });
  CHECK(copied.size() == shrunk_value.size());
  CHECK(span_to_string(std::span<const std::byte>(copied.data(), copied.size())) == shrunk_value);
}
