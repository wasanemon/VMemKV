#pragma once

#include <cstdint>
#include <functional>
#include <random>
#include <thread>

namespace pskiplist {

inline constexpr double kDefaultLevelPromotionProbability = 0.25;
inline constexpr int kDefaultMaxLevel = 32;

// Assigns each new node's participation height via a geometric distribution (2.5節):
// starting at level 1, promotes to the next level with probability `p` until it stops or
// reaches `max_level`. RNG state is thread-local, seeded once per thread from `seed`
// combined with the calling thread's id — concurrent callers never synchronize on a
// shared generator, and a fixed `seed` gives a reproducible sequence for single-threaded
// use (6章's fault-injection tests), without claiming exact reproducibility across
// concurrent interleavings.
class LevelGenerator {
 public:
  explicit LevelGenerator(uint64_t seed, int max_level = kDefaultMaxLevel, double p = kDefaultLevelPromotionProbability)
      : seed_(seed), max_level_(max_level), p_(p) {}

  [[nodiscard]] auto next_level() const -> int {
    thread_local std::mt19937_64 rng(seed_ ^ std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int level = 1;
    while (level < max_level_ && unit(rng) < p_) {
      ++level;
    }
    return level;
  }

 private:
  uint64_t seed_;
  int max_level_;
  double p_;
};

}  // namespace pskiplist
