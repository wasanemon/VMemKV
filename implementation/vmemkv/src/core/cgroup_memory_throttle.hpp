// cgroup_memory_throttle.hpp -- Voluntary pacing for bulk ingest under cgroup v2 memory limits.
//
// Under a cgroup v2 memory.high limit, crossing the budget provides no asynchronous reclaim:
// the overage is served entirely by the faulting thread's own synchronous direct reclaim. A
// sustained dirtying burst (e.g. bulk_load() populating a larger-than-memory corpus) can
// therefore outrun reclaim permanently and stall instead of converging. This throttle samples
// the enclosing cgroup's memory.current against memory.high every check_interval keys and
// sleeps briefly as the usage nears the limit, lowering the dirtying rate so reclaim keeps up.
// Without a cgroup v2 limit (unlimited root, cgroup v1, macOS, missing files) it is permanently
// inactive and each call costs only a counter check.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace vmemkv {

class CgroupMemoryThrottle {
 public:
  // check_interval: sample pressure once per this many keys (file reads amortize to ~ns/key).
  // cgroup_dir_override: use this directory's memory.high/memory.current instead of
  // auto-detecting from /proc/self/cgroup (tests, or operators pointing at another cgroup).
  // VMEMKV_CGROUP_ROOT serves the same override role via environment when non-null here.
  explicit CgroupMemoryThrottle(size_t check_interval = 1024, const char *cgroup_dir_override = nullptr)
      : check_interval_(check_interval == 0 ? 1 : check_interval) {
    const char *env = (cgroup_dir_override != nullptr) ? cgroup_dir_override : std::getenv("VMEMKV_CGROUP_ROOT");
    if (env != nullptr && *env != '\0') {
      init_from_dir(env);
    } else {
      init_from_self_cgroup();
    }
  }

  // Samples pressure (at most once per check_interval keys) and sleeps if usage nears the limit.
  void maybe_throttle(size_t index) {
    if (!active_ || (index % check_interval_) != 0) {
      return;
    }
    uint64_t current = 0;
    if (!read_uint_file(dir_ + "/memory.current", current)) {
      return;
    }
    // current >= high: already over budget, the reclaim race is on -- back off hard.
    // current >= 95% of high: approaching the budget -- ease off early, before the race starts.
    if (current >= high_bytes_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      throttle_events_.fetch_add(1, std::memory_order_relaxed);
    } else if (current * 100 >= high_bytes_ * 95) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      throttle_events_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] auto active() const noexcept -> bool { return active_; }

  // Lifetime count of backpressure sleeps taken -- diagnostic (e.g. confirming the throttle
  // actually engaged during a larger-than-memory bulk load).
  [[nodiscard]] auto throttle_events() const noexcept -> uint64_t {
    return throttle_events_.load(std::memory_order_relaxed);
  }

 private:
  static auto read_uint_file(const std::string &path, uint64_t &out) -> bool {
    std::ifstream file(path);
    if (!file.is_open()) {
      return false;
    }
    uint64_t value = 0;
    file >> value;
    if (file.fail()) {
      return false;  // Non-numeric content such as memory.high's "max".
    }
    out = value;
    return true;
  }

  void init_from_dir(const std::string &dir) {
    uint64_t high = 0;
    if (!read_uint_file(dir + "/memory.high", high) || high == 0) {
      return;
    }
    dir_ = dir;
    high_bytes_ = high;
    active_ = true;
  }

  // Finds the nearest enclosing cgroup (starting at this process's own, walking up to the
  // unified hierarchy root) carrying a numeric memory.high limit.
  void init_from_self_cgroup() {
    std::ifstream self_cgroup("/proc/self/cgroup");
    if (!self_cgroup.is_open()) {
      return;
    }
    std::string subpath;
    for (std::string line; std::getline(self_cgroup, line);) {
      // cgroup v2 entry: "0::<subpath>".
      if (line.rfind("0::", 0) == 0) {
        subpath = line.substr(3);
        break;
      }
    }
    if (subpath.empty()) {
      return;
    }
    std::filesystem::path dir = std::filesystem::path("/sys/fs/cgroup") / subpath.substr(1);
    const std::filesystem::path root("/sys/fs/cgroup");
    for (; dir.string().size() >= root.string().size(); dir = dir.parent_path()) {
      uint64_t high = 0;
      if (read_uint_file((dir / "memory.high").string(), high) && high != 0) {
        dir_ = dir.string();
        high_bytes_ = high;
        active_ = true;
        return;
      }
      if (dir == root) {
        break;
      }
    }
  }

  size_t check_interval_;
  bool active_ = false;
  uint64_t high_bytes_ = 0;
  std::string dir_;
  std::atomic<uint64_t> throttle_events_{0};
};

}  // namespace vmemkv
