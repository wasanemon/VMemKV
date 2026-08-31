#include "t2_flat_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <system_error>
#include <thread>
#include <vmemkv/config.hpp>

#include "../checkpoint/checkpoint.hpp"

namespace vmemkv {

namespace {

// Writes a ValueRecordHeader followed by key, value, and zero padding out to the next 8-byte
// boundary at `record_base`.
void write_record(std::byte *record_base, std::span<const std::byte> key, std::span<const std::byte> value) noexcept {
  const uint64_t raw_len = sizeof(ValueRecordHeader) + key.size() + value.size();
  const uint64_t aligned_len = vmemkv::align_up(raw_len);

  auto *header = reinterpret_cast<ValueRecordHeader *>(record_base);
  header->key_len = static_cast<uint32_t>(key.size());
  header->value_len = static_cast<uint32_t>(value.size());
  header->alloc_len = static_cast<uint32_t>(value.size());
  header->version = 0;

  auto *cursor = reinterpret_cast<std::byte *>(header + 1);
  std::memcpy(cursor, key.data(), key.size());
  cursor += key.size();

  if (!value.empty()) {
    std::memcpy(cursor, value.data(), value.size());
    cursor += value.size();
  }

  const uint64_t padding = aligned_len - raw_len;
  if (padding > 0) {
    std::memset(cursor, 0, padding);
  }
}

}  // namespace

T2FlatFile::T2FlatFile(const std::filesystem::path &path,
                       uint64_t bytes_capacity,
                       std::optional<uint64_t> initial_bytes_used)
    : path_(path) {
  const std::filesystem::path data_path = vmemkv::derive_t2_chk_path(path);
  if (!initial_bytes_used.has_value()) {
    std::error_code ignored;
    std::filesystem::remove(data_path, ignored);
    create_empty_file(data_path, bytes_capacity);
  }
  map_file(data_path, bytes_capacity, initial_bytes_used.value_or(0));
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

auto T2FlatFile::append_default(const T2Memory *mem,
                                std::span<const std::byte> key,
                                std::span<const std::byte> value) -> uint64_t {
  const uint64_t raw_required = sizeof(ValueRecordHeader) + key.size() + value.size();
  // Align all record sizes to 8 bytes. This avoids unaligned memory access penalties
  // on modern CPU architectures and allows callers to cast fields safely.
  const uint64_t required = vmemkv::align_up(raw_required);

  const uint64_t offset = mem->bytes_used.fetch_add(required, std::memory_order_relaxed);

  if (offset + required > mem->capacity) {
    throw std::runtime_error("T2 storage capacity exceeded");
  }

  std::byte *record_base = mem->base + offset;
  write_record(record_base, key, value);

  return offset;
}

void T2FlatFile::stop_writers_and_wait(const T2Memory *mem) const noexcept {
  // seq_cst: paired with acquire_write_handle()'s seq_cst writer_stop_ load and
  // ThreadReferenceTracker::acquire()'s seq_cst store -- see acquire_write_handle()'s comment.
  writer_stop_.store(true, std::memory_order_seq_cst);
  active_readers_.wait_until_retired(mem);
}

auto T2FlatFile::update_value_at(uint64_t payload, std::span<const std::byte> value) const noexcept -> bool {
  T2MemoryHandle mem = get_memory_handle();
  return update_value_at(payload, value, mem);
}

auto T2FlatFile::update_value_at(uint64_t payload,
                                 std::span<const std::byte> value,
                                 const T2Memory *mem) noexcept -> bool {
  auto *header = reinterpret_cast<ValueRecordHeader *>(const_cast<std::byte *>(resolve_record(payload, mem)));

  if (value.size() > header->alloc_len) {
    return false;
  }

  std::byte *value_begin = reinterpret_cast<std::byte *>(header + 1) + header->key_len;

  // SeqLock Write Begin: Increment version to odd to block parallel readers
  auto atomic_version = std::atomic_ref<uint64_t>(header->version);
  uint64_t ver = atomic_version.load(std::memory_order_relaxed);
  atomic_version.store(ver + 1, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);

  // This memcpy (and value_len below) plain-races a concurrent reader's plain reads of the same
  // bytes -- benign by design, see read_t2_record_seqlock()'s doc comment in vmemkv_impl.hpp.
  if (!value.empty()) {
    std::memcpy(value_begin, value.data(), value.size());
  }

  header->value_len = static_cast<uint32_t>(value.size());

  // SeqLock Write End: Increment version to even to signal complete write
  std::atomic_thread_fence(std::memory_order_release);
  atomic_version.store(ver + 2, std::memory_order_release);
  return true;
}

void T2FlatFile::retire_memory(const T2Memory *old_mem) {
  active_readers_.wait_until_retired(old_mem);
  delete old_mem;
}

void T2FlatFile::map_file(const std::filesystem::path &path, uint64_t bytes_capacity, uint64_t initial_bytes_used) {
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

  // MAP_SHARED: writes land directly in the page cache and are coherent across every mapping of
  // this file (including the base-region scan mappings below), so checkpoint_internal()'s
  // msync() durabilizes exactly what readers already see. See
  // docs/specification/why_vmemkv_does_not_need_undo_log.md for why this needs no undo log, and
  // low_level_design.md 4.3/5.1 for the full contract.
  // MAP_NORESERVE: bypasses the kernel's upfront swap-space reservation check, allowing
  // virtual address spaces much larger than physical RAM + swap without ENOMEM.
  // Physical pages are allocated on demand and can be swapped out normally.
  void *mapped = ::mmap(nullptr,
                        static_cast<size_t>(bytes_capacity),
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_NORESERVE,
                        file_descriptor,
                        0);
  const int mmap_errno = errno;
  if (mapped == MAP_FAILED) {
    ::close(file_descriptor);
    throw std::system_error(mmap_errno, std::generic_category(), "mmap");
  }
  // Unconditional (low_level_design.md 7.7): kept on because its win at low concurrency inverts
  // to a loss under sustained concurrent access, and no known deployment runs at the low, fixed
  // concurrency where turning it off would win -- see docs/benchmark/20260810_t2_no_madvise_random.md.
  if (::madvise(mapped, static_cast<size_t>(bytes_capacity), MADV_RANDOM) != 0) {
    const int err = errno;
    ::close(file_descriptor);
    throw std::system_error(err, std::generic_category(), "madvise MADV_RANDOM");
  }

  // Best-effort base-region scan mappings/read handle -- see T2Memory::base_mmap_scan's and
  // T2Memory::read_fd's doc comments for who reads these and why. A failure here is silently
  // non-fatal: the primary mapping above already provides full correctness (scan_impl()'s
  // seqlock fallback), these are purely a speed optimization. Set up unconditionally, even when
  // initial_bytes_used == 0 (a fresh store), so a later checkpoint's incremental base_boundary
  // promotion has real mappings to extend without ever remapping.
  std::byte *base_mmap_scan_ptr = nullptr;
  std::byte *base_mmap_scan_seq_ptr = nullptr;
  int read_fd_dup = -1;
  {
    void *base_mapped_scan =
        ::mmap(nullptr, static_cast<size_t>(bytes_capacity), PROT_READ, MAP_SHARED, file_descriptor, 0);
    if (base_mapped_scan != MAP_FAILED) {
      base_mmap_scan_ptr = static_cast<std::byte *>(base_mapped_scan);
    }

    void *base_mapped_scan_seq =
        ::mmap(nullptr, static_cast<size_t>(bytes_capacity), PROT_READ, MAP_SHARED, file_descriptor, 0);
    if (base_mapped_scan_seq != MAP_FAILED) {
      if (::madvise(base_mapped_scan_seq, static_cast<size_t>(bytes_capacity), MADV_SEQUENTIAL) == 0) {
        base_mmap_scan_seq_ptr = static_cast<std::byte *>(base_mapped_scan_seq);
      } else {
        ::munmap(base_mapped_scan_seq, static_cast<size_t>(bytes_capacity));
      }
    }

    // Must dup() before file_descriptor is closed below.
    read_fd_dup = ::fcntl(file_descriptor, F_DUPFD_CLOEXEC, 0);
  }

  ::close(file_descriptor);
  auto *mem = new T2Memory(static_cast<std::byte *>(mapped), bytes_capacity, initial_bytes_used);
  mem->base_mmap_scan = base_mmap_scan_ptr;
  mem->base_mmap_scan_seq = base_mmap_scan_seq_ptr;
  mem->read_fd = read_fd_dup;
  t2_mem_.store(mem, std::memory_order_release);
}

void T2FlatFile::create_empty_file(const std::filesystem::path &path, uint64_t bytes_capacity) {
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
}

}  // namespace vmemkv
