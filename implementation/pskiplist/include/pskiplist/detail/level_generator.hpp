#pragma once

#include <cstdint>
#include <functional>
#include <random>
#include <thread>

namespace pskiplist {

inline constexpr double kDefaultLevelPromotionProbability = 0.25;
inline constexpr int kDefaultMaxLevel = 32;

// Assigns each new chunk's height via a geometric distribution (high_level_design.md 2.5):
// promotes from level 1 with probability `p` until it stops or reaches `max_level`. RNG state
// is thread-local (seeded from `seed` + thread id), so a fixed `seed` reproduces single-threaded
// sequences without claiming bit-exact reproducibility across concurrent interleavings.
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
