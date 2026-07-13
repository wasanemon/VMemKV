#include "t2_flat_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <system_error>
#include <vmemkv/config.hpp>

namespace vmemkv {

T2FlatFile::T2FlatFile(const std::filesystem::path &path, uint64_t bytes_capacity)
    : path_(path), t2_bytes_capacity_(bytes_capacity) {
  t2_bytes_used_.store(0, std::memory_order_release);
  map_file(path, create_empty_file(path, bytes_capacity));
}

T2FlatFile::~T2FlatFile() noexcept {
  const T2Memory *mem = t2_mem_.load(std::memory_order_relaxed);
  if (mem != nullptr) {
    retire_memory(mem);
  }
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto T2FlatFile::at(uint64_t payload, const T2Memory *mem) const noexcept -> T2RecordView {
  const std::byte *record_base = resolve_record(payload, mem);
  const auto *header = reinterpret_cast<const ValueRecordHeader *>(record_base);
  const auto *key_begin = reinterpret_cast<const std::byte *>(header + 1);
  const std::byte *value_begin = key_begin + header->key_len;

  return T2RecordView{
      header,
      std::span<const std::byte>(key_begin, header->key_len),
      std::span<const std::byte>(value_begin, header->value_len),
  };
}

auto T2FlatFile::append_default(std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t {
  const uint64_t raw_required = sizeof(ValueRecordHeader) + key.size() + value.size();
  // Align all record sizes to 8 bytes. This avoids unaligned memory access penalties
  // on modern CPU architectures and allows callers to cast fields safely.
  const uint64_t required = vmemkv::align_up(raw_required);

  const uint64_t offset = t2_bytes_used_.fetch_add(required, std::memory_order_relaxed);

  if (offset + required > t2_bytes_capacity_) {
    throw std::runtime_error("T2 storage capacity exceeded");
  }

  T2MemoryHandle mem = get_memory_handle();
  std::byte *record_base = mem->base + offset;

  auto *header = reinterpret_cast<ValueRecordHeader *>(record_base);
  header->key_len = static_cast<uint32_t>(key.size());
  header->value_len = static_cast<uint32_t>(value.size());
  header->alloc_len = static_cast<uint32_t>(value.size());
  header->flags = 0;
  header->version = 0;

  auto *cursor = reinterpret_cast<std::byte *>(header + 1);
  std::memcpy(cursor, key.data(), key.size());
  cursor += key.size();

  if (!value.empty()) {
    std::memcpy(cursor, value.data(), value.size());
    cursor += value.size();
  }

  const uint64_t padding = required - raw_required;
  if (padding > 0) {
    std::memset(cursor, 0, padding);
  }

  return offset;
}

struct ThreadChunk {
  std::byte *base = nullptr;
  uint64_t offset = 0;
  uint64_t capacity = 0;
  const T2Memory *mem = nullptr;
};
static thread_local ThreadChunk tl_chunk;
static constexpr uint64_t OSPageSize = 4096;

auto T2FlatFile::append_prefault(std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t {
  const uint64_t raw_required = sizeof(ValueRecordHeader) + key.size() + value.size();
  const uint64_t required = vmemkv::align_up(raw_required);

  T2MemoryHandle mem = get_memory_handle();

  if (tl_chunk.capacity < required || tl_chunk.mem != mem) {
    const uint64_t chunk_alloc_size = 2ULL * 1024 * 1024;  // 2MB
    const uint64_t global_offset = t2_bytes_used_.fetch_add(chunk_alloc_size, std::memory_order_relaxed);

    if (global_offset + chunk_alloc_size > t2_bytes_capacity_) {
      throw std::runtime_error("T2 storage capacity exceeded during chunk allocation");
    }

    tl_chunk.base = const_cast<std::byte *>(mem->base) + global_offset;
    tl_chunk.offset = 0;
    tl_chunk.capacity = chunk_alloc_size;
    tl_chunk.mem = mem;

    // Pre-fault the allocated 2MB chunk by writing to each page (4KB steps)
    for (uint64_t page = 0; page < chunk_alloc_size; page += OSPageSize) {
      volatile std::byte *ptr = tl_chunk.base + page;
      *ptr = std::byte{0};
    }
  }

  std::byte *record_base = tl_chunk.base + tl_chunk.offset;
  uint64_t offset = record_base - mem->base;

  tl_chunk.offset += required;
  tl_chunk.capacity -= required;

  auto *header = reinterpret_cast<ValueRecordHeader *>(record_base);
  header->key_len = static_cast<uint32_t>(key.size());
  header->value_len = static_cast<uint32_t>(value.size());
  header->alloc_len = static_cast<uint32_t>(value.size());
  header->flags = 0;
  header->version = 0;

  auto *cursor = reinterpret_cast<std::byte *>(header + 1);
  std::memcpy(cursor, key.data(), key.size());
  cursor += key.size();

  if (!value.empty()) {
    std::memcpy(cursor, value.data(), value.size());
    cursor += value.size();
  }

  const uint64_t padding = required - raw_required;
  if (padding > 0) {
    std::memset(cursor, 0, padding);
  }

  return offset;
}

auto T2FlatFile::update_value_at(uint64_t payload, std::span<const std::byte> value) noexcept -> bool {
  T2MemoryHandle mem = get_memory_handle();
  auto *header = reinterpret_cast<ValueRecordHeader *>(const_cast<std::byte *>(resolve_record(payload, mem)));

  if (value.size() > header->alloc_len) {
    return false;
  }

  std::byte *value_begin = reinterpret_cast<std::byte *>(header + 1) + header->key_len;

  // SeqLock Write Begin: Increment version to odd to block parallel readers
  auto atomic_version = std::atomic_ref<uint64_t>(header->version);
  uint64_t v = atomic_version.load(std::memory_order_relaxed);
  atomic_version.store(v + 1, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);

  if (!value.empty()) {
    std::memcpy(value_begin, value.data(), value.size());
  }

  header->value_len = static_cast<uint32_t>(value.size());

  // SeqLock Write End: Increment version to even to signal complete write
  std::atomic_thread_fence(std::memory_order_release);
  atomic_version.store(v + 2, std::memory_order_release);
  return true;
}

void T2FlatFile::swap_memory(std::unique_ptr<T2Memory> new_mem, uint64_t bytes_used) {
  const T2Memory *raw_new = new_mem.release();
  const T2Memory *old_mem = t2_mem_.exchange(raw_new, std::memory_order_acq_rel);
  t2_bytes_used_.store(bytes_used, std::memory_order_release);
  t2_bytes_capacity_ = raw_new->capacity;
  if (old_mem != nullptr) {
    retire_memory(old_mem);
  }
}

void T2FlatFile::retire_memory(const T2Memory *old_mem) {
  active_readers_.wait_until_retired(old_mem);
  delete old_mem;
}

void T2FlatFile::map_file(const std::filesystem::path &path, uint64_t bytes_capacity) {
  const int file_descriptor = ::open(path.c_str(), O_RDWR);
  if (file_descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open");
  }

  struct stat file_stat;
  if (::fstat(file_descriptor, &file_stat) != 0) {
    const int err = errno;
    ::close(file_descriptor);
    throw std::system_error(err, std::generic_category(), "fstat");
  }

  if (file_stat.st_size < 0 || static_cast<uint64_t>(file_stat.st_size) < bytes_capacity) {
    ::close(file_descriptor);
    throw std::invalid_argument("T2 file is smaller than capacity");
  }

  // MAP_PRIVATE: changes are not written back to the underlying file (volatile on restart).
  // MAP_NORESERVE: bypasses the kernel's upfront swap-space reservation check, allowing
  // virtual address spaces much larger than physical RAM + swap without ENOMEM.
  // Physical pages are allocated on demand and can be swapped out normally.
  void *mapped = ::mmap(nullptr,
                        static_cast<size_t>(bytes_capacity),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_NORESERVE,
                        file_descriptor,
                        0);
  const int mmap_errno = errno;
  ::close(file_descriptor);
  if (mapped == MAP_FAILED) {
    throw std::system_error(mmap_errno, std::generic_category(), "mmap");
  }

  t2_mem_.store(new T2Memory(static_cast<std::byte *>(mapped), bytes_capacity), std::memory_order_release);
  t2_bytes_capacity_ = bytes_capacity;
}

auto T2FlatFile::create_empty_file(const std::filesystem::path &path, uint64_t bytes_capacity) -> uint64_t {
  if (bytes_capacity == 0) {
    throw std::invalid_argument("T2Store capacity must be greater than zero");
  }
  if (bytes_capacity > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    throw std::invalid_argument("T2Store capacity is too large for mmap");
  }
  if (bytes_capacity > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
    throw std::invalid_argument("T2Store capacity is too large for ftruncate");
  }

  const int file_descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (file_descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open");
  }

  if (::ftruncate(file_descriptor, static_cast<off_t>(bytes_capacity)) != 0) {
    const int err = errno;
    ::close(file_descriptor);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    throw std::system_error(err, std::generic_category(), "ftruncate");
  }

  ::close(file_descriptor);
  return bytes_capacity;
}

}  // namespace vmemkv
