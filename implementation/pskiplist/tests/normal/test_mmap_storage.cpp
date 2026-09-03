#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <pskiplist/pskiplist.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "support/temp_file.hpp"

using pskiplist::DurableNode;
using pskiplist::PSkipList;
using pskiplist_test::capacity_bytes_for_nodes;
using pskiplist_test::TempFile;

TEST_CASE("construction creates a backing file of the requested size on disk") {
  TempFile tmp("file_size");
  const size_t bytes = capacity_bytes_for_nodes<int>(16);
  PSkipList<int> skiplist(tmp.path(), bytes);
  CHECK(std::filesystem::file_size(tmp.path()) == bytes);
}

TEST_CASE("data written via put() is visible through an independent mapping of the same file") {
  TempFile tmp("independent_mapping");
  const size_t bytes = capacity_bytes_for_nodes<int>(16);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(7, 999));
  }

  const int fd = ::open(tmp.path().c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  void *mapped = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  REQUIRE(mapped != MAP_FAILED);

  const auto *nodes = static_cast<const DurableNode<int> *>(mapped);
  // Offset 2 is the first slot allocate() ever hands out — 0/1 are the head/tail
  // sentinels, and this list never called reclaim() before its one put().
  CHECK(nodes[2].key == 7);

  ::munmap(mapped, bytes);
  ::close(fd);
}
