#include "t2_flat_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <system_error>

#include "../checkpoint/checkpoint.hpp"
#include "../core/mmap.hpp"

namespace vmemkv {

namespace {

// Writes a ValueRecordHeader followed by key, value, and zero padding out to the next 8-byte
// boundary at `record_base`.
void write_record(std::byte *record_base, std::span<const std::byte> key, std::span<const std::byte> value) noexcept {
  const uint64_t raw_len = sizeof(ValueRecordHeader) + key.size() + value.size();
  const uint64_t aligned_len = vmemkv::record_aligned_len(sizeof(ValueRecordHeader), key.size(), value.size());

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
    remove_quiet(data_path);
    create_empty_file(data_path, bytes_capacity);
  }
  map_file(data_path, bytes_capacity, initial_bytes_used.value_or(0));
}

T2FlatFile::~T2FlatFile() noexcept {
  const T2Memory *mem = t2_mem_.load(std::memory_order_relaxed);
  if (mem != nullptr) {
    // wait_until_retired() here only guards against a writer that started before this store began
    // shutting down still holding a handle -- readers never register in the gate (see its
    // declaration), so there's nothing else to wait for.
    gate_.wait_until_retired(mem);
    delete mem;
  }
}

auto T2FlatFile::at(uint64_t payload, const T2Memory *mem) noexcept -> T2RecordView {
  const std::byte *record_base = resolve_record(payload, mem);
  return make_record_view(reinterpret_cast<const ValueRecordHeader *>(record_base));
}

auto T2FlatFile::append_default(const T2Memory *mem,
                                std::span<const std::byte> key,
                                std::span<const std::byte> value) -> uint64_t {
  // Align all record sizes to 8 bytes. This avoids unaligned memory access penalties
  // on modern CPU architectures and allows callers to cast fields safely.
  const uint64_t required = vmemkv::record_aligned_len(sizeof(ValueRecordHeader), key.size(), value.size());

  const uint64_t offset = mem->bytes_used.fetch_add(required, std::memory_order_relaxed);

  if (offset + required > mem->capacity) {
    throw std::runtime_error("T2 storage capacity exceeded");
  }

  std::byte *record_base = mem->base + offset;
  write_record(record_base, key, value);

  return offset;
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
  void *mapped = nullptr;
  try {
    mapped = mmap_shared_or_throw(static_cast<size_t>(bytes_capacity),
                                  file_descriptor,
                                  PROT_READ | PROT_WRITE,
                                  "mmap",
                                  MAP_SHARED | MAP_NORESERVE,
                                  0);
  } catch (...) {
    ::close(file_descriptor);
    throw;
  }
  // Unconditional (low_level_design.md 7.4).
  try {
    madvise_or_throw(mapped, static_cast<size_t>(bytes_capacity), MADV_RANDOM, "madvise MADV_RANDOM");
  } catch (...) {
    ::munmap(mapped, static_cast<size_t>(bytes_capacity));
    ::close(file_descriptor);
    throw;
  }

  // Best-effort base-region scan mappings/read handle -- see BaseRegionMappings's
  // doc comments for who reads these and why. A failure here is silently
  // non-fatal: the primary mapping above already provides full correctness (scan_impl()'s
  // seqlock fallback), these are purely a speed optimization. Set up unconditionally, even when
  // initial_bytes_used == 0 (a fresh store), so a later checkpoint's incremental base_boundary
  // promotion has real mappings to extend without ever remapping.
  auto *mem = new T2Memory(static_cast<std::byte *>(mapped), bytes_capacity, initial_bytes_used);
  mem->adopt_best_effort(file_descriptor, bytes_capacity);

  ::close(file_descriptor);
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
    remove_quiet(path);
    throw std::system_error(err, std::generic_category(), "ftruncate");
  }

  ::close(file_descriptor);
}

auto T2FlatFile::punch_if_occupied(uint64_t offset, uint64_t len) const noexcept -> PunchOutcome {
  return HolePuncher::punch(vmemkv::derive_t2_chk_path(path_), get_memory(), offset, len);
}

BaseRegionMappings::~BaseRegionMappings() noexcept { dispose(); }

BaseRegionMappings::BaseRegionMappings(BaseRegionMappings &&other) noexcept
    : base_mmap_scan(other.base_mmap_scan),
      base_mmap_scan_seq(other.base_mmap_scan_seq),
      read_fd(other.read_fd),
      capacity_(other.capacity_) {
  other.base_mmap_scan = nullptr;
  other.base_mmap_scan_seq = nullptr;
  other.read_fd = -1;
  other.capacity_ = 0;
}

auto BaseRegionMappings::operator=(BaseRegionMappings &&other) noexcept -> BaseRegionMappings & {
  if (this != &other) {
    dispose();
    base_mmap_scan = other.base_mmap_scan;
    base_mmap_scan_seq = other.base_mmap_scan_seq;
    read_fd = other.read_fd;
    capacity_ = other.capacity_;
    other.base_mmap_scan = nullptr;
    other.base_mmap_scan_seq = nullptr;
    other.read_fd = -1;
    other.capacity_ = 0;
  }
  return *this;
}

void BaseRegionMappings::adopt_best_effort(int file_descriptor, uint64_t capacity) noexcept {
  capacity_ = capacity;
  void *scan = best_effort_mmap_shared(static_cast<size_t>(capacity), file_descriptor, PROT_READ, 0);
  if (scan != nullptr) {
    base_mmap_scan = static_cast<std::byte *>(scan);
  }

  void *scan_seq = best_effort_mmap_shared(static_cast<size_t>(capacity), file_descriptor, PROT_READ, 0);
  if (scan_seq != nullptr) {
    if (::madvise(scan_seq, static_cast<size_t>(capacity), MADV_SEQUENTIAL) == 0) {
      base_mmap_scan_seq = static_cast<std::byte *>(scan_seq);
    } else {
      ::munmap(scan_seq, static_cast<size_t>(capacity));
    }
  }

  // Must dup() before the caller closes file_descriptor.
  read_fd = ::fcntl(file_descriptor, F_DUPFD_CLOEXEC, 0);
}

void BaseRegionMappings::dispose() noexcept {
  if (base_mmap_scan != nullptr) {
    // Mapped to construction-time capacity, not base_boundary.
    ::munmap(base_mmap_scan, static_cast<size_t>(capacity_));
    base_mmap_scan = nullptr;
  }
  if (base_mmap_scan_seq != nullptr) {
    ::munmap(base_mmap_scan_seq, static_cast<size_t>(capacity_));
    base_mmap_scan_seq = nullptr;
  }
  if (read_fd >= 0) {
    ::close(read_fd);
    read_fd = -1;
  }
  capacity_ = 0;
}

auto HolePuncher::punch(const std::filesystem::path &t2_chk_path,
                        const T2Memory *mem,
                        uint64_t offset,
                        uint64_t len) noexcept -> T2FlatFile::PunchOutcome {
  if (len == 0) {
    return T2FlatFile::PunchOutcome::AlreadyHollow;
  }
  if (offset + len > mem->capacity) {
    return T2FlatFile::PunchOutcome::Failed;
  }
  const int file_descriptor = ::open(t2_chk_path.c_str(), O_RDWR);
  if (file_descriptor < 0) {
    return T2FlatFile::PunchOutcome::Failed;
  }
  const off_t data_at = ::lseek(file_descriptor, static_cast<off_t>(offset), SEEK_DATA);
  const int seek_errno = errno;
  if (data_at < 0) {
    ::close(file_descriptor);
    // ENXIO: no data at or after offset -- hollow (within this file's size, which always
    // covers the range). Anything else (filesystems without SEEK_DATA support): report
    // not-hollow so the punch below runs and reports support itself.
    if (seek_errno == ENXIO) {
      return T2FlatFile::PunchOutcome::AlreadyHollow;
    }
  } else if (static_cast<uint64_t>(data_at) >= offset + len) {
    ::close(file_descriptor);
    return T2FlatFile::PunchOutcome::AlreadyHollow;
  }
  const int punch_rc = ::fallocate(
      file_descriptor, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset), static_cast<off_t>(len));
  ::close(file_descriptor);
  if (punch_rc != 0) {
    return T2FlatFile::PunchOutcome::Failed;
  }
  // Drop the range from the page cache as well: punch zeroes the file blocks, but already
  // resident pages would keep serving stale bytes until reclaimed.
  ::madvise(mem->base + offset, static_cast<size_t>(len), MADV_DONTNEED);
  return T2FlatFile::PunchOutcome::Punched;
}

}  // namespace vmemkv
