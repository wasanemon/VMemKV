#include <pskiplist/pskiplist.hpp>

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

int main() {
  // PSkipList is backed by a single mmap'd file, MAP_SHARED, mutated in place.
  // capacity_bytes is fixed for the mapping's lifetime (see high_level_design.md 2.6節);
  // here it's sized for 1000 usable nodes plus the head/tail sentinels.
  const auto path = std::filesystem::temp_directory_path() / "pskiplist_basic_usage_example.dat";
  const size_t capacity_bytes = sizeof(pskiplist::DurableNode<int>) * 1002;
  pskiplist::PSkipList<int> list(path, capacity_bytes);

  // put()/get(): insert a key, then look it up.
  if (!list.put(42, 100)) return 1;
  assert(list.get(42) == 100);
  assert(!list.get(7).has_value());

  // put() on an existing key updates it in place — no duplicate is created.
  if (!list.put(42, 200)) return 1;
  assert(list.get(42) == 200);

  // remove() tombstones the key; get() no longer observes it.
  if (!list.remove(42)) return 1;
  assert(!list.get(42).has_value());
  assert(!list.remove(42));  // already gone: returns false, not an error.

  // scan() visits live keys in ascending order within [begin, end).
  for (int key = 0; key < 10; ++key) {
    if (!list.put(key, static_cast<uint64_t>(key) * 10)) return 1;
  }
  list.scan(0, 10, [](int key, uint64_t value) {
    std::printf("%d -> %lu\n", key, static_cast<unsigned long>(value));
  });

  // get()/put()/remove()/scan() are all lock-free and safe to call concurrently
  // from multiple threads without any external locking.
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&list, t] {
      for (int i = 0; i < 100; ++i) {
        const int key = 1000 + t * 100 + i;
        if (!list.put(key, static_cast<uint64_t>(key))) return;
        (void)list.get(key);
        (void)list.remove(key);
      }
    });
  }
  for (auto &worker : workers) worker.join();

  // reclaim() physically frees the space held by removed keys, making it
  // available to future put() calls again. It's not required for correctness —
  // only for reusing capacity — and it's safe to call concurrently with
  // get()/put()/remove()/scan() from other threads.
  list.reclaim();

  std::filesystem::remove(path);
  return 0;
}
