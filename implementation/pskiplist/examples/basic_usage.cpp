#include <cassert>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <pskiplist/pskiplist.hpp>
#include <thread>
#include <vector>

namespace {

constexpr int kExampleKey = 42;
constexpr uint64_t kInitialValue = 100;
constexpr int kMissingKey = 7;
constexpr uint64_t kUpdatedValue = 200;

constexpr int kScanCount = 10;            // scan() demo populates and reads back keys [0, kScanCount)
constexpr uint64_t kScanValueScale = 10;  // each key's demo value is key * kScanValueScale

constexpr int kWorkerThreads = 4;
constexpr int kOpsPerWorker = 100;
constexpr int kConcurrentKeyBase = 1000;

constexpr int kCheckpointedKey = 2000;
constexpr uint64_t kCheckpointedValue = 999;
constexpr int kUncheckpointedKey = 2001;
constexpr uint64_t kUncheckpointedValue = 111;

// PSkipList's constructor can throw (std::invalid_argument for a bad capacity_bytes,
// std::system_error for a failed open/mmap) — kept in its own function so main() can
// report it cleanly instead of letting it escape uncaught.
auto run() -> int {
  // PSkipList is backed by a single mmap'd file, MAP_SHARED, mutated in place.
  // capacity_bytes is fixed for the mapping's lifetime (see high_level_design.md §2.6);
  // here it's sized for 1000 usable nodes plus the head/tail sentinels. Reopening later
  // must use this exact same capacity_bytes.
  const auto path = std::filesystem::temp_directory_path() / "pskiplist_basic_usage_example.dat";
  const size_t capacity_bytes = sizeof(pskiplist::DurableNode<int>) * 1002;

  {
    pskiplist::PSkipList<int> list(path, capacity_bytes);

    // put()/get(): insert a key, then look it up.
    if (!list.put(kExampleKey, kInitialValue)) {
      return 1;
    }
    assert(list.get(kExampleKey) == kInitialValue);
    assert(!list.get(kMissingKey).has_value());

    // put() on an existing key updates it in place — no duplicate is created.
    if (!list.put(kExampleKey, kUpdatedValue)) {
      return 1;
    }
    assert(list.get(kExampleKey) == kUpdatedValue);

    // remove() tombstones the key; get() no longer observes it.
    if (!list.remove(kExampleKey)) {
      return 1;
    }
    assert(!list.get(kExampleKey).has_value());
    assert(!list.remove(kExampleKey));  // already gone: returns false, not an error.

    // scan() visits live keys in ascending order within [begin, end).
    for (int key = 0; key < kScanCount; ++key) {
      if (!list.put(key, static_cast<uint64_t>(key) * kScanValueScale)) {
        return 1;
      }
    }
    list.scan(0, kScanCount, [](int key, uint64_t value) {
      std::printf("%d -> %lu\n", key, static_cast<unsigned long>(value));
    });

    // get()/put()/remove()/scan() are all lock-free and safe to call concurrently
    // from multiple threads without any external locking.
    std::vector<std::thread> workers;
    for (int worker_id = 0; worker_id < kWorkerThreads; ++worker_id) {
      workers.emplace_back([&list, worker_id] {
        for (int i = 0; i < kOpsPerWorker; ++i) {
          const int key = kConcurrentKeyBase + worker_id * kOpsPerWorker + i;
          if (!list.put(key, static_cast<uint64_t>(key))) {
            return;
          }
          (void)list.get(key);
          (void)list.remove(key);
        }
      });
    }
    for (auto &worker : workers) {
      worker.join();
    }

    // reclaim() physically frees the space held by removed keys, making it
    // available to future put() calls again. It's not required for correctness —
    // only for reusing capacity — and it's safe to call concurrently with
    // get()/put()/remove()/scan() from other threads.
    list.reclaim();

    // checkpoint() is synchronous: once it returns true, every put()/remove() that had
    // already returned is guaranteed durable, surviving even an unclean process exit
    // (no graceful close() is required for this guarantee — only a completed
    // checkpoint() is). kCheckpointedKey is checkpointed; kUncheckpointedKey, put() after
    // that point, is not durable unless a later checkpoint() covers it too.
    if (!list.put(kCheckpointedKey, kCheckpointedValue)) {
      return 1;
    }
    if (!list.checkpoint()) {
      return 1;
    }
    if (!list.put(kUncheckpointedKey, kUncheckpointedValue)) {  // written after the checkpoint — not durable
      return 1;
    }

    // `list` goes out of scope here without calling checkpoint() again, simulating an
    // unclean exit right after the checkpoint above.
  }

  // Reopening the same path with the same capacity_bytes runs recovery automatically:
  // it recovers exactly the state as of the last checkpoint() above, not anything
  // written after it.
  pskiplist::PSkipList<int> recovered(path, capacity_bytes);
  assert(recovered.get(kCheckpointedKey) == kCheckpointedValue);
  assert(!recovered.get(kUncheckpointedKey).has_value());

  std::filesystem::remove(path);
  return 0;
}

}  // namespace

auto main() -> int {
  try {
    return run();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "pskiplist_basic_usage: %s\n", e.what());
    return 1;
  }
}
