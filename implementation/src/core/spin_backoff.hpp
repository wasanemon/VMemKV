// spin_backoff.hpp -- shared bounded spin-then-sleep backoff for busy-wait retry loops.
#pragma once

#include <chrono>
#include <thread>

namespace vmemkv {

// Bounded spin-then-sleep backoff for a thread waiting on another thread's progress (e.g. "T2
// hasn't caught up to what T1 already reflects yet", or ThreadReferenceTracker::wait_until_retired()
// waiting for a slot to clear): a bare `continue` (pure busy-spin, no yield) converges quickly on
// an under-subscribed machine but is not safe to assume converges *at all* once real thread
// contention is involved -- reproduced on CI (4 vCPUs, plus leftover RocksDB threadpool threads
// from earlier test cases still competing for them) as a genuine, sustained starvation of the
// waiting thread by whichever thread is actually making progress, even though the same test
// passed extensively under much lighter local contention. yield() alone is only a scheduler hint
// (some schedulers may treat it as a no-op); sleep_for() forces an actual, real-time
// descheduling, which is what actually guarantees the other thread gets to run.
struct SpinBackoff {
  int spin_count = 0;
  static constexpr int kYieldSpinsBeforeSleep = 32;
  static constexpr auto kBackoffSleep = std::chrono::milliseconds(1);

  void wait() {
    ++spin_count;
    if (spin_count <= kYieldSpinsBeforeSleep) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(kBackoffSleep);
    }
  }

  void reset() { spin_count = 0; }
};

}  // namespace vmemkv
