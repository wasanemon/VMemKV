// test_t2_flat_file.cpp — Isolated correctness tests for the standalone T2FlatFile type.
//
// These tests exercise vmemkv::T2FlatFile directly (no VMemKVImpl/T1Index/WAL involvement):
// append_default record round-tripping, in-place update_value_at, swap_memory, and
// T2Memory::generation uniqueness.

#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <api/store_adapter.hpp>  // pulled in transitively by vmemkv_impl.hpp; kInlineScalarValueBytes lives here
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
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
  vmemkv::T2FlatFile file{path, kTestCapacityBytes, vmemkv::T2Memory::allocate_generation()};

  ~TempT2File() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

// Anonymous mmap wrapped in a T2Memory, mirroring how reorganize_internal()/swap_memory()
// build a fresh mapping -- without needing a second on-disk file per call site.
auto make_anon_t2_memory(uint64_t capacity) -> std::unique_ptr<vmemkv::T2Memory> {
  void *mapped = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(mapped != MAP_FAILED);
  return std::make_unique<vmemkv::T2Memory>(static_cast<std::byte *>(mapped), capacity);
}

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

  CHECK(t2.file.update_value_at(offset, as_span(bytes_of("abc"))));

  CHECK(span_to_string(t2.file.at(offset, mem).value) == "abc");
}

TEST_CASE("T2FlatFile: update_value_at fails when the new value exceeds alloc_len") {
  TempT2File t2;
  auto mem = t2.file.get_memory_handle();
  const uint64_t offset = t2.file.append_default(mem, as_span(bytes_of("k")), as_span(bytes_of("abc")));

  CHECK_FALSE(t2.file.update_value_at(offset, as_span(bytes_of("0123456789"))));
}

TEST_CASE("T2FlatFile: swap_memory publishes the new capacity/bytes_used/base") {
  TempT2File t2;
  CHECK(t2.file.bytes_capacity() == kTestCapacityBytes);

  constexpr uint64_t new_capacity = kTestCapacityBytes * 2;
  auto new_mem = make_anon_t2_memory(new_capacity);
  const std::byte *new_base = new_mem->base;
  new_mem->bytes_used.store(123, std::memory_order_relaxed);

  t2.file.swap_memory(std::move(new_mem));

  CHECK(t2.file.bytes_capacity() == new_capacity);
  CHECK(t2.file.bytes_used() == 123U);
  CHECK(t2.file.get_memory()->base == new_base);
}

TEST_CASE("T2FlatFile: T2Memory::generation is unique and never repeats, even across a freed address") {
  // generation is the pairing tag between a T2Memory and the T1Index::SortedSnapshot built
  // against it (see T2Memory::generation's declaration), so it must never repeat, independent of
  // whether a later T2Memory's heap allocation happens to reuse an earlier, already-freed one's
  // address.
  auto mem_a = make_anon_t2_memory(4096);
  auto mem_b = make_anon_t2_memory(4096);
  CHECK(mem_a->generation != mem_b->generation);

  const uint64_t generation_a = mem_a->generation;
  mem_a.reset();  // Frees mem_a; a later T2Memory's heap allocation may reuse its address.
  auto mem_c = make_anon_t2_memory(4096);

  CHECK(mem_c->generation != generation_a);
  CHECK(mem_c->generation != mem_b->generation);
}

// Regression test: read_t2_record_seqlock() used to take an already-built T2RecordView and only
// re-validate the version counter on retry, never re-reading key_len/value_len -- a view built
// once outside the function could carry a stale size that no version recheck would catch. Fixed
// by taking an AtFunc supplier called fresh on every retry instead. This deterministically shrinks
// a record via update_value_at() then confirms read_t2_record_seqlock() always returns the
// correct, current size -- no threading needed since the flaw was structural, not just a race
// window (a concurrent version of this bug hung the reader spinning on a version field past the
// record's own bytes).
TEST_CASE(
    "T2FlatFile: read_t2_record_seqlock always observes the current value, even immediately "
    "after a shrinking update_value_at() (regression)") {
  TempT2File t2;
  auto mem = t2.file.get_memory_handle();
  const std::string original_value(64, 'A');  // alloc_len becomes 64.
  const uint64_t offset = t2.file.append_default(mem, as_span(bytes_of("k")), as_span(bytes_of(original_value)));

  const std::string shrunk_value = "new";  // Fits within alloc_len=64; update_value_at() succeeds.
  REQUIRE(t2.file.update_value_at(offset, as_span(bytes_of(shrunk_value))));

  // read_t2_record_seqlock() calls at_func() itself, fresh, so it always sees the record as it
  // is *right now* -- there is no way for a caller to hand it a stale, pre-built view anymore.
  std::vector<std::byte> copied;
  vmemkv::read_t2_record_seqlock([&]() -> T2RecordView { return t2.file.at(offset, mem); },
                                 [&](const T2RecordView &record) -> bool {
                                   copied.assign(record.value.begin(), record.value.end());
                                   return true;
                                 });
  CHECK(copied.size() == shrunk_value.size());
  CHECK(span_to_string(std::span<const std::byte>(copied.data(), copied.size())) == shrunk_value);
}
