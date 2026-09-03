#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <pskiplist/pskiplist.hpp>
#include <stdexcept>

#include "support/temp_file.hpp"

using pskiplist::ManifestHeader;
using pskiplist::PSkipList;
using pskiplist_test::capacity_bytes_for_nodes;
using pskiplist_test::TempFile;

TEST_CASE("reopening with a different capacity_bytes throws") {
  TempFile tmp("recover_capacity_mismatch");
  {
    PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
    REQUIRE(skiplist.checkpoint());
  }

  CHECK_THROWS_AS((PSkipList<int>(tmp.path(), capacity_bytes_for_nodes<int>(17))), std::invalid_argument);
}

TEST_CASE("reopening with a corrupted manifest checksum starts fresh rather than trusting it") {
  TempFile tmp("recover_corrupted_manifest");
  const size_t bytes = capacity_bytes_for_nodes<int>(16);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 100));
    REQUIRE(skiplist.checkpoint());
  }

  const auto manifest_path = pskiplist::manifest_path(tmp.path());
  ManifestHeader header{};
  {
    std::fstream file(manifest_path, std::ios::in | std::ios::binary);
    REQUIRE(file.read(reinterpret_cast<char *>(&header), sizeof(header)));
  }
  header.epoch += 1000;  // corrupt without recomputing the checksum
  {
    std::fstream file(manifest_path, std::ios::out | std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(&header), sizeof(header));
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  CHECK_FALSE(reopened.get(1).has_value());
}
