#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>

#include <pskiplist/pskiplist.hpp>
#include <unistd.h>

namespace pskiplist_test {

// A fresh, unique path under the system temp directory for a PSkipList backing file.
// Removes whatever ends up at that path (if anything) when it goes out of scope.
class TempFile {
 public:
  explicit TempFile(const std::string &label) {
    static std::atomic<uint64_t> counter{0};
    const auto id = counter.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("pskiplist_test_" + label + "_" + std::to_string(::getpid()) + "_" + std::to_string(id));
  }
  ~TempFile() {
    std::filesystem::remove(path_);
    std::filesystem::remove(pskiplist::manifest_path(path_));
  }

  TempFile(const TempFile &) = delete;
  auto operator=(const TempFile &) -> TempFile & = delete;

  [[nodiscard]] auto path() const -> const std::filesystem::path & { return path_; }

 private:
  std::filesystem::path path_;
};

// Bytes needed to hold `usable_nodes` insertable slots plus the head/tail sentinels,
// matching how capacity was expressed before the mmap-backed constructor (in node
// counts) — keeps small-capacity tests (e.g. "capacity exhausted") easy to state exactly.
template <typename Key>
[[nodiscard]] auto capacity_bytes_for_nodes(size_t usable_nodes) -> size_t {
  return (usable_nodes + 2) * sizeof(pskiplist::DurableNode<Key>);
}

}  // namespace pskiplist_test
