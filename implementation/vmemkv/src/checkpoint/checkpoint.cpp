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
// mmap's resident pages for RAM. Best-effort for the periodic calls -- only the final fsync is a
// hard durability requirement.
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

// Plain write loop, no fsync/close -- unlike write_fsync_close, does *not* close `file_descriptor`
// on error, so a caller wrapping a whole object's worth of incremental writes (e.g.
// ShardedT1CheckpointWriter) can let a mid-stream failure propagate and rely on that object's own
// destructor to close the fd and discard its temp file, rather than every individual write call
// needing to know how to unwind the caller's state.
void write_all(int file_descriptor, const void *data, size_t size) {
  const auto *bytes = static_cast<const std::byte *>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written = ::write(file_descriptor, bytes + offset, size - offset);
    if (written <= 0) {
      throw std::system_error(errno, std::generic_category(), "write sharded t1 checkpoint");
    }
    offset += static_cast<size_t>(written);
  }
}

// Opens a fresh temp file beside `final_path`, invokes `write_body(file_descriptor)`, then
// renames the temp file atomically onto `final_path`. Used by write_manifest so the "write to
// temp, fsync, rename over" crash-safety pattern lives in exactly one place.
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

ShardedT1CheckpointWriter::ShardedT1CheckpointWriter(const std::filesystem::path &path)
    : final_path_(path), temp_path_(path.string() + ".tmp"), checksum_(kFnvOffsetBasis64) {
  file_descriptor_ = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kCheckpointFilePermissions);
  if (file_descriptor_ < 0) {
    throw std::system_error(errno, std::generic_category(), "open sharded t1 checkpoint temp file");
  }
}

ShardedT1CheckpointWriter::~ShardedT1CheckpointWriter() {
  // Not finished (finish() never called, or an earlier write threw): the temp file never became
  // reachable at final_path_, so discarding it here is just tidiness, not a correctness
  // requirement -- best-effort, matching write_via_temp_then_rename's crash-safety contract.
  if (!finished_ && file_descriptor_ >= 0) {
    ::close(file_descriptor_);
    std::error_code ec;
    std::filesystem::remove(temp_path_, ec);
  }
}

void ShardedT1CheckpointWriter::add_shard_entries(const T1ChkEntry *entries, size_t entry_count) {
  const auto count64 = static_cast<uint64_t>(entry_count);
  write_all(file_descriptor_, &count64, sizeof(count64));
  checksum_ = fnv1a64_update(checksum_, &count64, sizeof(count64));
  if (entry_count > 0) {
    write_all(file_descriptor_, entries, entry_count * sizeof(T1ChkEntry));
    checksum_ = fnv1a64_update(checksum_, entries, entry_count * sizeof(T1ChkEntry));
  }
  ++shard_count_;
  total_entry_count_ += entry_count;
}

void ShardedT1CheckpointWriter::finish(std::span<const T1ChkKeyPrefix> boundaries) {
  if (!boundaries.empty()) {
    const size_t boundary_bytes = boundaries.size() * sizeof(T1ChkKeyPrefix);
    write_all(file_descriptor_, boundaries.data(), boundary_bytes);
    checksum_ = fnv1a64_update(checksum_, boundaries.data(), boundary_bytes);
  }

  ShardedT1ChkFileHeader header;
  header.shard_count = shard_count_;
  header.boundary_count = boundaries.size();
  header.total_entry_count = total_entry_count_;
  // header.checksum is still its default (0) here -- fold the header in now, with checksum
  // zeroed, exactly like every other format's convention, just last instead of first (see this
  // struct's own comment on why).
  header.checksum = fnv1a64_update(checksum_, &header, sizeof(header));

  // write_fsync_close() always closes the fd itself, on both success and failure -- clear our
  // copy *before* calling it (not after), so a throw from it doesn't leave file_descriptor_
  // pointing at an already-closed descriptor for the destructor to close a second time.
  const int fd = file_descriptor_;
  file_descriptor_ = -1;
  write_fsync_close(fd, &header, sizeof(header), "write sharded t1 checkpoint trailer");
  finished_ = true;

  std::error_code rename_ec;
  std::filesystem::rename(temp_path_, final_path_, rename_ec);
  if (rename_ec) {
    throw std::system_error(rename_ec, "rename sharded t1 checkpoint file");
  }
}

ShardedT1CheckpointFile::ShardedT1CheckpointFile(const std::filesystem::path &path) {
  const int file_descriptor = ::open(path.c_str(), O_RDONLY);
  if (file_descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open sharded t1 checkpoint");
  }

  struct stat file_stat {};
  if (::fstat(file_descriptor, &file_stat) != 0) {
    const int err = errno;
    ::close(file_descriptor);
    throw std::system_error(err, std::generic_category(), "fstat sharded t1 checkpoint");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);
  if (file_size < kShardedT1ChkFileHeaderBytes) {
    ::close(file_descriptor);
    throw std::runtime_error("sharded t1 checkpoint file smaller than its trailer");
  }

  void *mapped = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
  const int mmap_errno = errno;
  ::close(file_descriptor);
  if (mapped == MAP_FAILED) {
    throw std::system_error(mmap_errno, std::generic_category(), "mmap sharded t1 checkpoint");
  }

  const auto *base = static_cast<const std::byte *>(mapped);
  ShardedT1ChkFileHeader header;
  std::memcpy(&header, base + (file_size - kShardedT1ChkFileHeaderBytes), sizeof(header));

  const bool magic_ok = header.magic == kShardedT1ChkMagic && header.format_version == kShardedT1ChkFormatVersion;

  // Walk the shard sections and boundaries, folding both into the same checksum convention
  // finish() used (payload first, header-with-checksum-zeroed last) and recording each shard's
  // entry span as we go. Bounds-checked throughout: a corrupt entry_count claiming more data than
  // remains is exactly what this is guarding against, not just the final checksum comparison.
  bool layout_ok = true;
  uint64_t checksum = kFnvOffsetBasis64;
  uint64_t seen_entries = 0;
  std::vector<std::span<const T1ChkEntry>> shard_entries;
  size_t offset = 0;
  const size_t payload_limit = file_size - kShardedT1ChkFileHeaderBytes;
  if (magic_ok) {
    shard_entries.reserve(header.shard_count);
    for (uint64_t shard_index = 0; shard_index < header.shard_count && layout_ok; ++shard_index) {
      if (offset + sizeof(uint64_t) > payload_limit) {
        layout_ok = false;
        break;
      }
      uint64_t entry_count = 0;
      std::memcpy(&entry_count, base + offset, sizeof(entry_count));
      checksum = fnv1a64_update(checksum, base + offset, sizeof(entry_count));
      offset += sizeof(entry_count);

      const size_t entry_bytes = entry_count * sizeof(T1ChkEntry);
      if (offset + entry_bytes > payload_limit) {
        layout_ok = false;
        break;
      }
      shard_entries.push_back({reinterpret_cast<const T1ChkEntry *>(base + offset), static_cast<size_t>(entry_count)});
      if (entry_bytes > 0) {
        checksum = fnv1a64_update(checksum, base + offset, entry_bytes);
      }
      offset += entry_bytes;
      seen_entries += entry_count;
    }
  }

  const T1ChkKeyPrefix *boundaries = nullptr;
  if (magic_ok && layout_ok) {
    const size_t boundary_bytes = header.boundary_count * sizeof(T1ChkKeyPrefix);
    if (offset + boundary_bytes != payload_limit || seen_entries != header.total_entry_count ||
        header.boundary_count + 1 != header.shard_count) {
      layout_ok = false;
    } else {
      boundaries = reinterpret_cast<const T1ChkKeyPrefix *>(base + offset);
      if (boundary_bytes > 0) {
        checksum = fnv1a64_update(checksum, base + offset, boundary_bytes);
      }
    }
  }

  bool checksum_ok = false;
  if (magic_ok && layout_ok) {
    ShardedT1ChkFileHeader header_for_hash = header;
    header_for_hash.checksum = 0;
    checksum_ok = fnv1a64_update(checksum, &header_for_hash, sizeof(header_for_hash)) == header.checksum;
  }

  if (!magic_ok || !layout_ok || !checksum_ok) {
    ::munmap(mapped, file_size);
    throw std::runtime_error("sharded t1 checkpoint file failed validation (magic/version/layout/checksum)");
  }

  mapped_ = mapped;
  mapped_bytes_ = file_size;
  shard_entries_ = std::move(shard_entries);
  boundaries_ = boundaries;
  boundary_count_ = header.boundary_count;
}

ShardedT1CheckpointFile::~ShardedT1CheckpointFile() noexcept {
  if (mapped_ != nullptr) {
    ::munmap(mapped_, mapped_bytes_);
  }
}

auto ShardedT1CheckpointFile::boundaries() const noexcept -> std::span<const T1ChkKeyPrefix> {
  return {boundaries_, boundary_count_};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
