#include "checkpoint.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <system_error>

namespace vmemkv {

namespace {

constexpr mode_t kCheckpointFilePermissions = 0600;

// Writes `size` bytes fully, fsyncs, and closes `file_descriptor` -- shared tail for the T1
// checkpoint file and the manifest.
//
// Writes in kSyncIntervalBytes chunks, periodically fdatasync()-ing and
// posix_fadvise(DONTNEED)-ing what's been written, rather than one write()+fsync() at the end.
// This bounds how much of the T1 checkpoint accumulates as page cache, competing with the live T2
// mmap's resident pages for RAM (see maybe_sync_and_drop_checkpoint_cache() in vmemkv_impl.hpp for
// the equivalent on T2). Best-effort for the periodic calls -- only the final fsync is a hard
// durability requirement.
void write_fsync_close(int file_descriptor, const void *data, size_t size, const char *what) {
  constexpr size_t kSyncIntervalBytes = 512ULL * 1024 * 1024;  // 512MiB
  const auto *bytes = static_cast<const std::byte *>(data);
  size_t offset = 0;
  size_t synced = 0;
  while (offset < size) {
    const ssize_t written = ::write(file_descriptor, bytes + offset, size - offset);
    if (written <= 0) {
      const int err = errno;
      ::close(file_descriptor);
      throw std::system_error(err, std::generic_category(), what);
    }
    offset += static_cast<size_t>(written);
    if (offset - synced >= kSyncIntervalBytes && ::fdatasync(file_descriptor) == 0) {
      ::posix_fadvise(
          file_descriptor, static_cast<off_t>(synced), static_cast<off_t>(offset - synced), POSIX_FADV_DONTNEED);
      synced = offset;
    }
  }
  if (::fsync(file_descriptor) != 0) {
    const int err = errno;
    ::close(file_descriptor);
    throw std::system_error(err, std::generic_category(), what);
  }
  ::close(file_descriptor);
}

// Opens a fresh temp file beside `final_path`, invokes `write_body(file_descriptor)`, then
// renames the temp file atomically onto `final_path`. Shared by write_t1_checkpoint_file and
// write_manifest so the "write to temp, fsync, rename over" crash-safety pattern lives in
// exactly one place.
template <typename WriteBody>
void write_via_temp_then_rename(const std::filesystem::path &final_path, WriteBody write_body) {
  const std::filesystem::path temp_path = final_path.string() + ".tmp";
  const int file_descriptor = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kCheckpointFilePermissions);
  if (file_descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open checkpoint temp file");
  }
  write_body(file_descriptor);  // Consumes and closes the fd (see write_fsync_close).

  std::error_code rename_ec;
  std::filesystem::rename(temp_path, final_path, rename_ec);
  if (rename_ec) {
    throw std::system_error(rename_ec, "rename checkpoint file");
  }
}

}  // namespace

void write_t1_checkpoint_file(const std::filesystem::path &path,
                              const T1ChkFileHeader &header,
                              const T1ChkEntry *entries,
                              size_t entry_count) {
  write_via_temp_then_rename(path, [&](int file_descriptor) {
    const ssize_t header_written = ::write(file_descriptor, &header, sizeof(header));
    if (header_written != static_cast<ssize_t>(sizeof(header))) {
      const int err = errno;
      ::close(file_descriptor);
      throw std::system_error(err, std::generic_category(), "write t1 checkpoint header");
    }
    // No special-casing needed for entry_count == 0 -- write_fsync_close's write() is already a
    // no-op at size 0.
    write_fsync_close(file_descriptor, entries, entry_count * sizeof(T1ChkEntry), "write t1 checkpoint entries");
  });
}

T1CheckpointFile::T1CheckpointFile(const std::filesystem::path &path) {
  const int file_descriptor = ::open(path.c_str(), O_RDONLY);
  if (file_descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open t1 checkpoint");
  }

  struct stat file_stat {};
  if (::fstat(file_descriptor, &file_stat) != 0) {
    const int err = errno;
    ::close(file_descriptor);
    throw std::system_error(err, std::generic_category(), "fstat t1 checkpoint");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);
  if (file_size < kT1ChkFileHeaderBytes) {
    ::close(file_descriptor);
    throw std::runtime_error("t1 checkpoint file smaller than its header");
  }

  void *mapped = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
  const int mmap_errno = errno;
  ::close(file_descriptor);
  if (mapped == MAP_FAILED) {
    throw std::system_error(mmap_errno, std::generic_category(), "mmap t1 checkpoint");
  }

  T1ChkFileHeader header;
  std::memcpy(&header, mapped, sizeof(header));

  const auto *entry_bytes = static_cast<const std::byte *>(mapped) + kT1ChkFileHeaderBytes;
  const uint64_t payload_bytes = file_size - kT1ChkFileHeaderBytes;

  const bool magic_ok = header.magic == kT1ChkMagic && header.format_version == kT1ChkFormatVersion;
  const bool size_ok = payload_bytes == header.entry_count * kT1ChkEntryBytes;

  bool checksum_ok = false;
  if (magic_ok && size_ok) {
    uint64_t checksum = checksum_header(header);
    if (payload_bytes > 0) {
      checksum = fnv1a64_update(checksum, entry_bytes, payload_bytes);
    }
    checksum_ok = checksum == header.checksum;
  }

  if (!magic_ok || !size_ok || !checksum_ok) {
    ::munmap(mapped, file_size);
    throw std::runtime_error("t1 checkpoint file failed validation (magic/version/size/checksum)");
  }

  mapped_ = mapped;
  mapped_bytes_ = file_size;
  entries_ = reinterpret_cast<const T1ChkEntry *>(entry_bytes);
  entry_count_ = header.entry_count;
}

T1CheckpointFile::~T1CheckpointFile() noexcept {
  if (mapped_ != nullptr) {
    ::munmap(mapped_, mapped_bytes_);
  }
}

auto T1CheckpointFile::entries() const noexcept -> std::span<const T1ChkEntry> { return {entries_, entry_count_}; }

void write_manifest(const std::filesystem::path &manifest_path, uint64_t generation, uint64_t t2_bytes_used) {
  ManifestHeader header;
  header.generation = generation;
  header.t2_bytes_used = t2_bytes_used;
  header.checksum = checksum_header(header);

  write_via_temp_then_rename(manifest_path, [&](int file_descriptor) {
    write_fsync_close(file_descriptor, &header, sizeof(header), "write manifest");
  });
}

auto read_manifest(const std::filesystem::path &manifest_path) -> std::optional<ManifestData> {
  const int file_descriptor = ::open(manifest_path.c_str(), O_RDONLY);
  if (file_descriptor < 0) {
    return std::nullopt;  // Missing manifest == "no checkpoint exists yet", not an error.
  }

  ManifestHeader header;
  const ssize_t bytes_read = ::read(file_descriptor, &header, sizeof(header));
  ::close(file_descriptor);
  if (bytes_read != static_cast<ssize_t>(sizeof(header))) {
    return std::nullopt;  // Torn/truncated manifest: treat as absent, fall back safely.
  }

  if (header.magic != kManifestMagic || header.format_version != kManifestFormatVersion) {
    return std::nullopt;
  }

  if (checksum_header(header) != header.checksum) {
    return std::nullopt;
  }

  return ManifestData{header.generation, header.t2_bytes_used};
}

}  // namespace vmemkv
