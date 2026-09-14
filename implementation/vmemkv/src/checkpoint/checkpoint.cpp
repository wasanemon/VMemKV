#include "checkpoint.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include "../core/io.hpp"
#include "../core/mmap.hpp"

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
    const size_t chunk = std::min(kSyncIntervalBytes, size - offset);
    try {
      write_all_exact(file_descriptor, bytes + offset, chunk, what);
    } catch (...) {
      ::close(file_descriptor);
      throw;
    }
    offset += chunk;
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

// Parsed shard sections: each shard's entry span plus the offset where the boundary
// records begin, with the running checksum/entry tallies folded in.
struct ParsedShardSections {
  std::vector<std::span<const T1ChkEntry>> entries;
  size_t end_offset = 0;
  uint64_t seen_entries = 0;
};

// Walks the per-shard entry-count/entry-span records, folding the payload checksum
// (counts then spans, matching finish()'s convention). Bounds-checked throughout: a
// corrupt entry_count claiming more data than remains fails here, not at the final
// checksum comparison. Returns nullopt on any layout violation.
auto parse_shard_sections(const std::byte *base,
                          size_t payload_limit,
                          const ShardedT1ChkFileHeader &header,
                          FoldingChecksum &checksum) -> std::optional<ParsedShardSections> {
  ParsedShardSections parsed;
  parsed.entries.reserve(header.shard_count);
  size_t offset = 0;
  for (uint64_t shard_index = 0; shard_index < header.shard_count; ++shard_index) {
    if (offset + sizeof(uint64_t) > payload_limit) {
      return std::nullopt;
    }
    uint64_t entry_count = 0;
    std::memcpy(&entry_count, base + offset, sizeof(entry_count));
    checksum.add(base + offset, sizeof(entry_count));
    offset += sizeof(entry_count);
    const size_t entry_bytes = entry_count * sizeof(T1ChkEntry);
    if (offset + entry_bytes > payload_limit) {
      return std::nullopt;
    }
    parsed.entries.emplace_back(reinterpret_cast<const T1ChkEntry *>(base + offset), static_cast<size_t>(entry_count));
    if (entry_bytes > 0) {
      checksum.add(base + offset, entry_bytes);
    }
    offset += entry_bytes;
    parsed.seen_entries += entry_count;
  }
  parsed.end_offset = offset;
  return parsed;
}

// Validates the trailing boundary records against the parsed sections (exact fit,
// entry totals, and the boundaries/shards count relation) and folds them into the
// checksum. Returns the boundary span start, or nullptr on any violation.
auto parse_shard_boundaries(const std::byte *base,
                            size_t offset,
                            size_t payload_limit,
                            const ShardedT1ChkFileHeader &header,
                            uint64_t seen_entries,
                            FoldingChecksum &checksum) -> const T1ChkKeyPrefix * {
  const size_t boundary_bytes = header.boundary_count * sizeof(T1ChkKeyPrefix);
  if (offset + boundary_bytes != payload_limit || seen_entries != header.total_entry_count ||
      header.boundary_count + 1 != header.shard_count) {
    return nullptr;
  }
  if (boundary_bytes > 0) {
    checksum.add(base + offset, boundary_bytes);
  }
  return reinterpret_cast<const T1ChkKeyPrefix *>(base + offset);
}

}  // namespace

ShardedT1CheckpointWriter::ShardedT1CheckpointWriter(const std::filesystem::path &path)
    : final_path_(path), temp_path_(path.string() + ".tmp") {
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
    remove_quiet(temp_path_);
  }
}

void ShardedT1CheckpointWriter::add_shard_entries(const T1ChkEntry *entries, size_t entry_count) {
  const auto count64 = static_cast<uint64_t>(entry_count);
  write_all_exact(file_descriptor_, &count64, sizeof(count64), "write sharded t1 checkpoint");
  checksum_.add(&count64, sizeof(count64));
  if (entry_count > 0) {
    write_all_exact(file_descriptor_, entries, entry_count * sizeof(T1ChkEntry), "write sharded t1 checkpoint");
    checksum_.add(entries, entry_count * sizeof(T1ChkEntry));
  }
  ++shard_count_;
  total_entry_count_ += entry_count;
}

void ShardedT1CheckpointWriter::finish(std::span<const T1ChkKeyPrefix> boundaries) {
  if (!boundaries.empty()) {
    const size_t boundary_bytes = boundaries.size() * sizeof(T1ChkKeyPrefix);
    write_all_exact(file_descriptor_, boundaries.data(), boundary_bytes, "write sharded t1 checkpoint");
    checksum_.add(boundaries.data(), boundary_bytes);
  }

  ShardedT1ChkFileHeader header;
  header.shard_count = shard_count_;
  header.boundary_count = boundaries.size();
  header.total_entry_count = total_entry_count_;
  // header.checksum is still its default (0) here -- fold the header in now, with checksum
  // zeroed, exactly like every other format's convention, just last instead of first (see this
  // struct's own comment on why).
  checksum_.add_header(header);
  header.checksum = checksum_.value();

  // write_fsync_close() always closes the fd itself, on both success and failure -- clear our
  // copy *before* calling it (not after), so a throw from it doesn't leave file_descriptor_
  // pointing at an already-closed descriptor for the destructor to close a second time.
  const int trailer_fd = file_descriptor_;
  file_descriptor_ = -1;
  write_fsync_close(trailer_fd, &header, sizeof(header), "write sharded t1 checkpoint trailer");
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

  void *mapped = mmap_shared_or_throw(
      static_cast<size_t>(file_size), file_descriptor, PROT_READ, "mmap sharded t1 checkpoint", MAP_PRIVATE, 0);
  ::close(file_descriptor);

  const auto *base = static_cast<const std::byte *>(mapped);
  ShardedT1ChkFileHeader header;
  std::memcpy(&header, base + (file_size - kShardedT1ChkFileHeaderBytes), sizeof(header));

  const bool magic_ok = header.magic == kShardedT1ChkMagic && header.format_version == kShardedT1ChkFormatVersion;

  // Walk the shard sections and boundaries, folding both into the same checksum convention
  // finish() used (payload first, header-with-checksum-zeroed last) and recording each shard's
  // entry span as we go. Bounds-checked throughout: a corrupt entry_count claiming more data than
  // remains is exactly what this is guarding against, not just the final checksum comparison.
  bool layout_ok = true;
  FoldingChecksum checksum;
  uint64_t seen_entries = 0;
  std::vector<std::span<const T1ChkEntry>> shard_entries;
  const T1ChkKeyPrefix *boundaries = nullptr;
  const size_t payload_limit = file_size - kShardedT1ChkFileHeaderBytes;
  if (magic_ok) {
    if (auto sections = parse_shard_sections(base, payload_limit, header, checksum)) {
      shard_entries = std::move(sections->entries);
      seen_entries = sections->seen_entries;
      boundaries = parse_shard_boundaries(base, sections->end_offset, payload_limit, header, seen_entries, checksum);
      layout_ok = (boundaries != nullptr);
    } else {
      layout_ok = false;
    }
  }

  bool checksum_ok = false;
  if (magic_ok && layout_ok) {
    ShardedT1ChkFileHeader header_for_hash = header;
    header_for_hash.checksum = 0;
    const auto header_bytes =
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(&header_for_hash), sizeof(header_for_hash));
    checksum_ok = fnv1a64_range(checksum.value(), header_bytes) == header.checksum;
  }

  if (!magic_ok || !layout_ok || !checksum_ok) {
    throw std::runtime_error("sharded t1 checkpoint file failed validation (magic/version/layout/checksum)");
  }

  mapping_ = MmapGuard(mapped, static_cast<size_t>(file_size));
  shard_entries_ = std::move(shard_entries);
  boundaries_ = boundaries;
  boundary_count_ = header.boundary_count;
}

ShardedT1CheckpointFile::~ShardedT1CheckpointFile() noexcept = default;

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
