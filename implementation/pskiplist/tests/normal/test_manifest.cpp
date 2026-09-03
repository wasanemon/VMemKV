#include <doctest/doctest.h>

#include <pskiplist/pskiplist.hpp>

#include "support/temp_file.hpp"

using pskiplist::manifest_path;
using pskiplist::read_manifest;
using pskiplist::write_manifest;
using pskiplist_test::TempFile;

TEST_CASE("write then read round-trips epoch and high_water_mark") {
  TempFile tmp("manifest_roundtrip");
  write_manifest(tmp.path(), 42, 1000);

  const auto header = read_manifest(tmp.path());
  REQUIRE(header.has_value());
  CHECK(header->epoch == 42);
  CHECK(header->high_water_mark == 1000);
}

TEST_CASE("a later write replaces the manifest atomically") {
  TempFile tmp("manifest_overwrite");
  write_manifest(tmp.path(), 1, 10);
  write_manifest(tmp.path(), 2, 20);

  const auto header = read_manifest(tmp.path());
  REQUIRE(header.has_value());
  CHECK(header->epoch == 2);
  CHECK(header->high_water_mark == 20);
}

TEST_CASE("manifest_path appends .manifest to the data path") {
  CHECK(manifest_path("/tmp/foo.dat") == "/tmp/foo.dat.manifest");
}
