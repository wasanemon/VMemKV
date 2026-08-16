// test_checkpoint.cpp — Isolated correctness tests for the T1 checkpoint file and manifest
// formats (checkpoint/checkpoint.hpp). Exercises these directly, no VMemKVImpl involvement.

#include <doctest/doctest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <checkpoint/checkpoint.hpp>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace {

auto reserve_checkpoint_path() -> std::filesystem::path {
  return vmemkv_test::reserve_unique_temp_path("vmemkv_chk_test");
}

// Duck-typed stand-in for T1Index<Config>::EntrySnapshot -- exercises write_t1_checkpoint's
// template contract (a `.key` / `.hash` / `.payload_bits` triple) without depending on
// t1_index.hpp, matching the module boundary checkpoint.hpp itself keeps.
struct FakeEntrySnapshot {
  vmemkv::T1ChkKeyPrefix key{};
  uint64_t payload_bits = 0;
  uint64_t hash = 0;
};

auto make_key(std::string_view text) -> vmemkv::T1ChkKeyPrefix {
  vmemkv::T1ChkKeyPrefix key{};
  std::memcpy(key.data(), text.data(), std::min(text.size(), key.size()));
  return key;
}

}  // namespace

TEST_CASE("T1 checkpoint: write then read round-trips entries exactly") {
  const auto path = reserve_checkpoint_path();
  const std::vector<FakeEntrySnapshot> entries = {
      {make_key("aaa"), 100, 0x1111},
      {make_key("bbb"), 200, 0x2222},
      {make_key("ccc"), 300, 0x3333},
  };

  vmemkv::write_t1_checkpoint(path, std::span<const FakeEntrySnapshot>(entries));

  vmemkv::T1CheckpointFile loaded(path);
  const auto loaded_entries = loaded.entries();
  REQUIRE(loaded_entries.size() == entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    CHECK(loaded_entries[i].key_prefix == entries[i].key);
    CHECK(loaded_entries[i].hash == entries[i].hash);
    CHECK(loaded_entries[i].payload_bits == entries[i].payload_bits);
  }

  std::filesystem::remove(path);
}

TEST_CASE("T1 checkpoint: zero entries round-trips to an empty, valid file") {
  const auto path = reserve_checkpoint_path();
  const std::vector<FakeEntrySnapshot> entries;

  vmemkv::write_t1_checkpoint(path, std::span<const FakeEntrySnapshot>(entries));

  vmemkv::T1CheckpointFile loaded(path);
  CHECK(loaded.entries().empty());

  std::filesystem::remove(path);
}

TEST_CASE("T1 checkpoint: re-writing the same path replaces it atomically") {
  const auto path = reserve_checkpoint_path();
  const std::vector<FakeEntrySnapshot> first = {{make_key("old"), 1, 1}};
  const std::vector<FakeEntrySnapshot> second = {{make_key("new"), 2, 2}, {make_key("new2"), 3, 3}};

  vmemkv::write_t1_checkpoint(path, std::span<const FakeEntrySnapshot>(first));
  vmemkv::write_t1_checkpoint(path, std::span<const FakeEntrySnapshot>(second));

  vmemkv::T1CheckpointFile loaded(path);
  REQUIRE(loaded.entries().size() == 2);
  CHECK(loaded.entries()[0].payload_bits == 2);
  CHECK(loaded.entries()[1].payload_bits == 3);

  std::filesystem::remove(path);
}

TEST_CASE("T1 checkpoint: missing file throws") {
  const auto path = reserve_checkpoint_path();
  CHECK_THROWS(vmemkv::T1CheckpointFile(path));
}

TEST_CASE("T1 checkpoint: file shorter than its header throws") {
  const auto path = reserve_checkpoint_path();
  {
    std::ofstream out(path, std::ios::binary);
    constexpr std::array<char, 4> garbage{};
    out.write(garbage.data(), garbage.size());
  }
  CHECK_THROWS(vmemkv::T1CheckpointFile(path));
  std::filesystem::remove(path);
}

TEST_CASE("T1 checkpoint: corrupted checksum is detected") {
  const auto path = reserve_checkpoint_path();
  const std::vector<FakeEntrySnapshot> entries = {{make_key("k"), 1, 1}};
  vmemkv::write_t1_checkpoint(path, std::span<const FakeEntrySnapshot>(entries));

  // Flip a byte inside the entry payload -- past the header, so magic/version/size checks
  // alone would not catch this; only the checksum can.
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(static_cast<std::streamoff>(vmemkv::kT1ChkFileHeaderBytes));
    char original = 0;
    file.read(&original, 1);
    const char flipped = static_cast<char>(~original);
    file.seekp(static_cast<std::streamoff>(vmemkv::kT1ChkFileHeaderBytes));
    file.write(&flipped, 1);
  }

  CHECK_THROWS(vmemkv::T1CheckpointFile(path));
  std::filesystem::remove(path);
}

TEST_CASE("T1 checkpoint: entry_count mismatched with actual file size is detected") {
  const auto path = reserve_checkpoint_path();
  const std::vector<FakeEntrySnapshot> entries = {{make_key("k"), 1, 1}};
  vmemkv::write_t1_checkpoint(path, std::span<const FakeEntrySnapshot>(entries));

  // Truncate off the last few bytes of the one entry -- header still claims entry_count == 1.
  std::filesystem::resize_file(path, vmemkv::kT1ChkFileHeaderBytes + vmemkv::kT1ChkEntryBytes - 4);

  CHECK_THROWS(vmemkv::T1CheckpointFile(path));
  std::filesystem::remove(path);
}

TEST_CASE("Manifest: write then read round-trips generation/t2_bytes_used") {
  const auto path = reserve_checkpoint_path();
  vmemkv::write_manifest(path, 42, 12345);

  const auto manifest = vmemkv::read_manifest(path);
  REQUIRE(manifest.has_value());
  CHECK(manifest->generation == 42);
  CHECK(manifest->t2_bytes_used == 12345);

  std::filesystem::remove(path);
}

TEST_CASE("Manifest: re-writing replaces the generation atomically") {
  const auto path = reserve_checkpoint_path();
  vmemkv::write_manifest(path, 1, 100);
  vmemkv::write_manifest(path, 2, 200);

  const auto manifest = vmemkv::read_manifest(path);
  REQUIRE(manifest.has_value());
  CHECK(manifest->generation == 2);
  CHECK(manifest->t2_bytes_used == 200);

  std::filesystem::remove(path);
}

TEST_CASE("Manifest: missing file reads as nullopt, not an error") {
  const auto path = reserve_checkpoint_path();
  const auto manifest = vmemkv::read_manifest(path);
  CHECK_FALSE(manifest.has_value());
}

TEST_CASE("Manifest: corrupted checksum reads as nullopt") {
  const auto path = reserve_checkpoint_path();
  vmemkv::write_manifest(path, 7, 70);

  {
    const auto offset = static_cast<std::streamoff>(offsetof(vmemkv::ManifestHeader, checksum));
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(offset);
    char original = 0;
    file.read(&original, 1);
    const char flipped = static_cast<char>(~original);
    file.seekp(offset);
    file.write(&flipped, 1);
  }

  CHECK_FALSE(vmemkv::read_manifest(path).has_value());
  std::filesystem::remove(path);
}

TEST_CASE("Manifest: truncated file reads as nullopt") {
  const auto path = reserve_checkpoint_path();
  {
    std::ofstream out(path, std::ios::binary);
    constexpr std::array<char, 4> garbage{};
    out.write(garbage.data(), garbage.size());
  }
  CHECK_FALSE(vmemkv::read_manifest(path).has_value());
  std::filesystem::remove(path);
}

TEST_CASE("Path derivation: manifest/t1chk/t2chk paths are distinct and generation-scoped") {
  const std::filesystem::path t2_path = "/tmp/some_store";

  const auto manifest_path = vmemkv::derive_manifest_path(t2_path);
  const auto t1chk_gen5 = vmemkv::derive_t1_chk_path(t2_path, 5);
  const auto t1chk_gen5_again = vmemkv::derive_t1_chk_path(t2_path, 5);
  const auto t1chk_gen6 = vmemkv::derive_t1_chk_path(t2_path, 6);
  const auto t2chk_gen5 = vmemkv::derive_t2_chk_path(t2_path, 5);

  CHECK(manifest_path != t1chk_gen5);
  CHECK(t1chk_gen5 == t1chk_gen5_again);  // deterministic for the same (t2_path, generation)
  CHECK(t1chk_gen5 != t1chk_gen6);        // different generations must not collide
  CHECK(t1chk_gen5 != t2chk_gen5);        // T1 chk and T2 data files for the same generation differ
}
