// background_poll.hpp - Bounded polling for background maintenance workers.
#pragma once

#include <chrono>
#include <thread>

namespace vmemkv {

inline constexpr auto kDefaultPoll10ms = std::chrono::milliseconds(10);
inline constexpr auto kDefragPoll1s = std::chrono::seconds(1);

// Sleeps `interval` until `pred` holds. Bounded by construction: every iteration
// sleeps, so the waiter always yields the CPU to the thread it waits on.
template <typename Pred>
inline void poll_until(Pred &&pred, std::chrono::nanoseconds interval = kDefaultPoll10ms) {
  while (!pred()) {
    std::this_thread::sleep_for(interval);
  }
}

}  // namespace vmemkv
