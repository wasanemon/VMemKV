// swap_check.hpp -- Startup swap validation for larger-than-memory operation.
//
// VMemKV spills beyond RAM through the OS virtual memory system; without swap backing, a
// larger-than-memory workload meets the OOM killer instead of graceful paging. This check runs
// once per VMemKVImpl construction: it warns (once per process) when the host has no swap at
// all, and fails construction when the operator set an explicit floor via VMEMKV_REQUIRE_SWAP_BYTES
// that the host does not meet. Without /proc/meminfo (non-Linux) it silently does nothing.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace vmemkv {

inline auto system_swap_total_bytes(const std::string &meminfo_path = "/proc/meminfo") -> std::optional<uint64_t> {
  std::ifstream meminfo(meminfo_path);
  if (!meminfo.is_open()) {
    return std::nullopt;
  }
  std::string key;
  std::size_t value_kb = 0;
  std::string unit;
  while (meminfo >> key >> value_kb >> unit) {
    if (key == "SwapTotal:") {
      return value_kb * 1024ULL;
    }
    meminfo.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return std::nullopt;
}

inline void validate_swap_for_ltm(const std::string &meminfo_path = "/proc/meminfo") {
  const std::optional<uint64_t> swap_total = system_swap_total_bytes(meminfo_path);
  if (!swap_total.has_value()) {
    return;
  }
  if (const char *env = std::getenv("VMEMKV_REQUIRE_SWAP_BYTES"); env != nullptr && *env != '\0') {
    const uint64_t required = std::strtoull(env, nullptr, 10);
    if (*swap_total < required) {
      throw std::runtime_error("VMemKV requires " + std::to_string(required) +
                               " swap bytes (VMEMKV_REQUIRE_SWAP_BYTES) but the host provides only " +
                               std::to_string(*swap_total));
    }
    return;
  }
  if (*swap_total == 0) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
      std::cerr << "[vmemkv] warning: host has no swap; larger-than-memory workloads risk the OOM "
                   "killer instead of paging. Set VMEMKV_REQUIRE_SWAP_BYTES to enforce a minimum."
                << std::endl;
    }
  }
}

}  // namespace vmemkv
