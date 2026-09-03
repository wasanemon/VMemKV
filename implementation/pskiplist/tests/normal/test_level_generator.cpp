#include <doctest/doctest.h>

#include <pskiplist/pskiplist.hpp>

using pskiplist::LevelGenerator;

TEST_CASE("next_level always returns 1 when the promotion probability is zero") {
  const LevelGenerator generator(42, 32, 0.0);
  for (int i = 0; i < 100; ++i) {
    CHECK(generator.next_level() == 1);
  }
}

TEST_CASE("next_level always saturates at max_level when the promotion probability is one") {
  const LevelGenerator generator(42, 8, 1.0);
  for (int i = 0; i < 100; ++i) {
    CHECK(generator.next_level() == 8);
  }
}

TEST_CASE("next_level stays within [1, max_level] for a normal probability") {
  const LevelGenerator generator(42, 16, 0.25);
  for (int i = 0; i < 1000; ++i) {
    const int level = generator.next_level();
    CHECK(level >= 1);
    CHECK(level <= 16);
  }
}
