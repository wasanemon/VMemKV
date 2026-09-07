// test_swap_check.cpp -- Unit tests for the startup swap validation (swap_check.hpp) against
// fake meminfo files.

#include <doctest/doctest.h>
#include <unistd.h>

#include <atomic>
#include <core/swap_check.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

auto reserve_tmp_file(const std::string &name) -> std::filesystem::path {
  static std::atomic<uint64_t> counter{0};
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("swapcheck_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                                      std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + "_" + name);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return path;
}

void write_meminfo(const std::filesystem::path &path, const std::string &swap_line) {
  std::ofstream file(path, std::ios::trunc);
  file << "MemTotal:        4024548 kB\n"
          "MemFree:         1234567 kB\n"
       << swap_line << "\n";
}

struct EnvGuard {
  std::string name;
  bool had_value = false;
  std::string old_value;
  explicit EnvGuard(const char *n) : name(n) {
    if (const char *v = std::getenv(n); v != nullptr) {
      had_value = true;
      old_value = v;
    }
  }
  ~EnvGuard() {
    if (had_value) {
      ::setenv(name.c_str(), old_value.c_str(), 1);
    } else {
      ::unsetenv(name.c_str());
    }
  }
};

}  // namespace

TEST_CASE("SwapCheck: parses SwapTotal from meminfo") {
  const auto path = reserve_tmp_file("parse");
  write_meminfo(path, "SwapTotal:       8388608 kB");
  const auto total = vmemkv::system_swap_total_bytes(path.string());
  REQUIRE(total.has_value());
  CHECK(*total == 8388608ULL * 1024ULL);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST_CASE("SwapCheck: missing file yields nullopt and validation passes") {
  CHECK(!vmemkv::system_swap_total_bytes("/nonexistent-meminfo-xyz").has_value());
  CHECK_NOTHROW(vmemkv::validate_swap_for_ltm("/nonexistent-meminfo-xyz"));
}

TEST_CASE("SwapCheck: zero swap warns but passes without a required floor") {
  EnvGuard guard("VMEMKV_REQUIRE_SWAP_BYTES");
  ::unsetenv("VMEMKV_REQUIRE_SWAP_BYTES");
  const auto path = reserve_tmp_file("zero");
  write_meminfo(path, "SwapTotal:             0 kB");
  CHECK_NOTHROW(vmemkv::validate_swap_for_ltm(path.string()));
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST_CASE("SwapCheck: below the required floor throws") {
  EnvGuard guard("VMEMKV_REQUIRE_SWAP_BYTES");
  const auto path = reserve_tmp_file("floor");
  write_meminfo(path, "SwapTotal:       8388608 kB");  // 8 GiB.
  ::setenv("VMEMKV_REQUIRE_SWAP_BYTES", std::to_string(16ULL * 1024ULL * 1024ULL * 1024ULL).c_str(), 1);
  CHECK_THROWS_AS(vmemkv::validate_swap_for_ltm(path.string()), std::runtime_error);
  ::setenv("VMEMKV_REQUIRE_SWAP_BYTES", std::to_string(1ULL * 1024ULL * 1024ULL * 1024ULL).c_str(), 1);
  CHECK_NOTHROW(vmemkv::validate_swap_for_ltm(path.string()));
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
