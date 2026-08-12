// checkpoint.hpp - T1 checkpoint file and manifest formats for VMemKV checkpoint reload.
// See docs/specification/low_level_design.md 5.2-5.5.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "../api/utils.hpp"

namespace vmemkv {

// ─── T1 Checkpoint File (t1_index.chk) ─────────────────────────────────────
//
// A flat, mmap-and-use array of fixed-size entries -- no per-record framing, no torn-tail
// handling. Unlike the WAL, this file is never appended to: it's written once to a temp path and
// only becomes reachable when the manifest (below) is committed to point at its generation. A
// crash mid-write therefore never produces a file a future reader could mistake for valid, so the
// only integrity check needed is a whole-file checksum guarding against bit rot.

inline constexpr uint32_t kT1ChkMagic = 0x314B4C56;  // ASCII "VLK1"
inline constexpr uint8_t kT1ChkFormatVersion = 1;

struct T1ChkFileHeader {
  uint32_t magic = kT1ChkMagic;
  uint8_t format_version = kT1ChkFormatVersion;
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  uint8_t reserved[3] = {};
  uint64_t entry_count = 0;
  uint64_t checksum = 0;  // FNV-1a64 over header(checksum zeroed) + entry array.
};
inline constexpr size_t kT1ChkFileHeaderBytes = 24;
static_assert(sizeof(T1ChkFileHeader) == kT1ChkFileHeaderBytes);
static_assert(std::is_standard_layout_v<T1ChkFileHeader>);

// Must match t1_index.hpp's kStoreKeyBytes. Deliberately not a dependency on StoreKey itself, to
// keep this module decoupled from T1Index's internals.
inline constexpr size_t kT1ChkKeyPrefixBytes = 16;
using T1ChkKeyPrefix = std::array<std::byte, kT1ChkKeyPrefixBytes>;

// On-disk entry layout: mirrors low_level_design.md 2.1's IndexEntry (key_prefix, hash,
// payload_bits), 32B fixed. This is intentionally a plain, mmap-able POD distinct from
// T1Index's own EntrySnapshot (whose member order differs) -- callers convert on write.
struct T1ChkEntry {
  T1ChkKeyPrefix key_prefix{};
  uint64_t hash = 0;
  uint64_t payload_bits = 0;
};
inline constexpr size_t kT1ChkEntryBytes = 32;
static_assert(sizeof(T1ChkEntry) == kT1ChkEntryBytes);
static_assert(std::is_standard_layout_v<T1ChkEntry>);

// Non-template file I/O, implemented in checkpoint.cpp: writes `header` followed by
// `entries[0..entry_count)` to a temp file beside `path`, fsyncs, and renames atomically onto
// `path`. A crash before the rename completes leaves `path` untouched.
void write_t1_checkpoint_file(const std::filesystem::path &path,
                              const T1ChkFileHeader &header,
                              const T1ChkEntry *entries,
                              size_t entry_count);

// Writes a T1 checkpoint file from `entries`, which must already be sorted by key_prefix
// ascending (T1Index::reorganize()'s merge output already satisfies this). EntrySnapshotLike
// is duck-typed (any type exposing `.key`, `.hash`, `.payload_bits`) so this template has no
// dependency on T1Index's internal layout -- e.g. T1Index<Config>::EntrySnapshot.
template <typename EntrySnapshotLike>
void write_t1_checkpoint(const std::filesystem::path &path, std::span<const EntrySnapshotLike> entries) {
  std::vector<T1ChkEntry> on_disk;
  on_disk.reserve(entries.size());
  for (const auto &entry : entries) {
    on_disk.push_back(T1ChkEntry{entry.key, entry.hash, entry.payload_bits});
  }

  T1ChkFileHeader header;
  header.entry_count = on_disk.size();
  uint64_t checksum = checksum_header(header);
  if (!on_disk.empty()) {
    checksum = fnv1a64_update(checksum, on_disk.data(), on_disk.size() * sizeof(T1ChkEntry));
  }
  header.checksum = checksum;

  write_t1_checkpoint_file(path, header, on_disk.data(), on_disk.size());
}

// Reads and validates a T1 checkpoint file by mmap'ing it (MAP_PRIVATE, read-only). Throws
// std::system_error / std::runtime_error if the file is missing or fails validation (bad
// magic, format_version, or checksum). entries() stays valid for this object's lifetime.
class T1CheckpointFile {
 public:
  explicit T1CheckpointFile(const std::filesystem::path &path);
  ~T1CheckpointFile() noexcept;

  T1CheckpointFile(const T1CheckpointFile &) = delete;
  auto operator=(const T1CheckpointFile &) -> T1CheckpointFile & = delete;
  T1CheckpointFile(T1CheckpointFile &&) = delete;
  auto operator=(T1CheckpointFile &&) -> T1CheckpointFile & = delete;

  [[nodiscard]] auto entries() const noexcept -> std::span<const T1ChkEntry>;

 private:
  void *mapped_ = nullptr;
  size_t mapped_bytes_ = 0;
  const T1ChkEntry *entries_ = nullptr;
  size_t entry_count_ = 0;
};

// ─── Manifest (<t2_path>.manifest) ─────────────────────────────────────────
//
// The single source of truth for "which checkpoint generation is current". `generation` doubles
// as the checkpoint LSN (5.3): the T1 and T2 checkpoint files for a generation live at paths
// derived from it, and neither is trusted by a reader until the manifest naming that generation
// is committed (write to temp, fsync, rename atomically). This is what makes the pair crash-safe
// (5.2/5.3).

inline constexpr uint32_t kManifestMagic = 0x314B4D56;  // ASCII "VMK1"
// v2: split the single `generation` field into `t1_generation`/`t2_generation` -- see their
// declarations below. A v1 manifest fails both the format_version check and the file-size check
// in read_manifest(), so it's treated identically to "no checkpoint exists yet" (same contract
// as any other corrupt/missing manifest); no on-disk migration path exists or is needed.
inline constexpr uint8_t kManifestFormatVersion = 2;

struct ManifestHeader {
  uint32_t magic = kManifestMagic;
  uint8_t format_version = kManifestFormatVersion;
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  uint8_t reserved[3] = {};
  // == checkpoint_lsn of the cycle that committed this manifest. Always advances on every
  // committed checkpoint (T2-rebuild or T1-only) and names the T1 checkpoint file
  // (derive_t1_chk_path). Distinct from `t2_generation` below since a T1-only checkpoint (see
  // VMemKVImpl::reorganize_internal()'s T1-only-with-checkpoint branch) advances this without
  // touching T2 at all.
  uint64_t t1_generation = 0;
  // == checkpoint_lsn of whichever cycle most recently actually rebuilt T2. Names the T2
  // checkpoint file (derive_t2_chk_path); stays unchanged across any number of subsequent
  // T1-only checkpoints, since those reference the same, still-valid T2 file. Equal to
  // t1_generation whenever this checkpoint itself rebuilt T2.
  uint64_t t2_generation = 0;
  uint64_t t2_bytes_used = 0;  // Logical bytes used in t2_generation's T2 checkpoint file --
                               // not recoverable from that file's size alone (it may be
                               // truncated to a larger capacity than its live data occupies).
  uint64_t checksum = 0;       // FNV-1a64 over the header with checksum zeroed.
};
inline constexpr size_t kManifestHeaderBytes = 40;
static_assert(sizeof(ManifestHeader) == kManifestHeaderBytes);
static_assert(std::is_standard_layout_v<ManifestHeader>);

struct ManifestData {
  uint64_t t1_generation = 0;
  uint64_t t2_generation = 0;
  uint64_t t2_bytes_used = 0;
};

// Derives the sibling manifest path for a given T2 flat-file path.
inline auto derive_manifest_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".manifest"};
}

// Derives the generation-specific T1 checkpoint / T2 checkpoint file paths. Never reused
// across generations, so an orphaned file from an incomplete checkpoint (crash before the
// manifest rename) is simply never referenced by anything and is safe to leave or delete.
inline auto derive_t1_chk_path(const std::filesystem::path &t2_path, uint64_t generation) -> std::filesystem::path {
  return {t2_path.string() + ".chk" + std::to_string(generation) + ".t1"};
}
inline auto derive_t2_chk_path(const std::filesystem::path &t2_path, uint64_t generation) -> std::filesystem::path {
  return {t2_path.string() + ".chk" + std::to_string(generation) + ".t2"};
}

// Writes the manifest to a temp file beside `manifest_path` and renames it atomically onto
// `manifest_path`. Call only after t1_generation's T1 checkpoint file, and (if this checkpoint
// actually rebuilt T2) t2_generation's T2 checkpoint file, are already fully written and fsynced.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void write_manifest(const std::filesystem::path &manifest_path,
                    uint64_t t1_generation,
                    uint64_t t2_generation,
                    uint64_t t2_bytes_used);

// Reads and validates the manifest at `manifest_path`. Returns the generation/t2_bytes_used on
// success; std::nullopt if the file does not exist or fails validation (bad magic,
// format_version, or checksum) -- callers must treat that identically to "no checkpoint exists
// yet".
auto read_manifest(const std::filesystem::path &manifest_path) -> std::optional<ManifestData>;

}  // namespace vmemkv
