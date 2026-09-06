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

// ─── Sharded T1 Checkpoint File (t1_index.chk, multi-shard layout) ─────────
//
// Used by ShardedT1Index (src/t1_index/sharded_t1_index.hpp), whose checkpoint_all_shards()
// forces every shard's reorganize() and hands back one already-sorted entry span per shard plus
// the directory's boundary keys once the whole call returns -- unlike the single-shard format
// above, the full entry set is never materialized as one array, and neither shard_count nor the
// boundary keys are known until after every shard has already been written. So the header is a
// *trailer* here, not a prefix: on-disk layout is `shard_count` sections of
// (entry_count: uint64_t, entries[entry_count]), then `boundary_count` T1ChkKeyPrefix boundary
// keys, then the header, last. This lets the writer be purely sequential (no seek-back-and-
// overwrite once final counts are known) at the cost of the reader locating the header via
// `file_size - sizeof(header)` instead of offset 0 -- trivial with mmap. Shard sections have no
// fixed stride (entry counts vary), so a reader walks them sequentially using each section's own
// entry_count. Same crash-safety discipline as the single-shard format: written once to a temp
// path, fsynced, and only reachable once the manifest commits to its generation.

inline constexpr uint32_t kShardedT1ChkMagic = 0x324B4C56;  // ASCII "VLK2" -- distinct from "VLK1"
inline constexpr uint8_t kShardedT1ChkFormatVersion = 1;

struct ShardedT1ChkFileHeader {
  uint32_t magic = kShardedT1ChkMagic;
  uint8_t format_version = kShardedT1ChkFormatVersion;
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  uint8_t reserved[3] = {};
  uint64_t shard_count = 0;
  uint64_t boundary_count = 0;     // == shard_count - 1.
  uint64_t total_entry_count = 0;  // Sum of every shard's entry_count -- lets a reader sanity
                                   // check its walk consumed exactly the expected data.
  // FNV-1a64, folded in write order: every shard section (entry_count then entries) + boundaries
  // + this header with checksum zeroed *last* -- reversed from the other formats' "header first"
  // convention, since this header's own fields aren't known until every shard has been written.
  uint64_t checksum = 0;
};
inline constexpr size_t kShardedT1ChkFileHeaderBytes = 40;
static_assert(sizeof(ShardedT1ChkFileHeader) == kShardedT1ChkFileHeaderBytes);
static_assert(std::is_standard_layout_v<ShardedT1ChkFileHeader>);

// Incremental writer: open once, call add_shard() once per shard in the same ascending order
// ShardedT1Index::checkpoint_all_shards() invokes its per_shard_writer callback, then
// finish(boundaries) once that call has returned and the boundary keys it produced are known.
// Mirrors write_t1_checkpoint_file's temp+fsync+rename discipline, just spread across multiple
// calls instead of one shot, so a shard's entries never need to be held in memory together with
// any other shard's -- only this object's own small running state (counts and a running
// checksum) persists across calls. Discards the temp file if destroyed before finish() is called
// (crash-safe: the final path is never touched until finish()'s atomic rename).
class ShardedT1CheckpointWriter {
 public:
  explicit ShardedT1CheckpointWriter(const std::filesystem::path &path);
  ~ShardedT1CheckpointWriter();

  ShardedT1CheckpointWriter(const ShardedT1CheckpointWriter &) = delete;
  auto operator=(const ShardedT1CheckpointWriter &) -> ShardedT1CheckpointWriter & = delete;
  ShardedT1CheckpointWriter(ShardedT1CheckpointWriter &&) = delete;
  auto operator=(ShardedT1CheckpointWriter &&) -> ShardedT1CheckpointWriter & = delete;

  // `entries` must already be sorted by key ascending (T1Index::reorganize()'s merge output, which
  // ShardedT1Index::checkpoint_all_shards() calls this back with per shard, already satisfies
  // this). EntrySnapshotLike is duck-typed exactly like write_t1_checkpoint()'s own template
  // parameter -- see that function's comment.
  template <typename EntrySnapshotLike>
  void add_shard(std::span<const EntrySnapshotLike> entries) {
    std::vector<T1ChkEntry> on_disk;
    on_disk.reserve(entries.size());
    for (const auto &entry : entries) {
      on_disk.push_back(T1ChkEntry{entry.key, entry.hash, entry.payload_bits});
    }
    add_shard_entries(on_disk.data(), on_disk.size());
  }

  // Call exactly once, after every add_shard() call is done. Writes the boundary keys, then the
  // header (now that shard_count/total_entry_count/checksum are all known) as a trailer, fsyncs,
  // and renames the temp file atomically onto the final path.
  void finish(std::span<const T1ChkKeyPrefix> boundaries);

 private:
  void add_shard_entries(const T1ChkEntry *entries, size_t entry_count);

  std::filesystem::path final_path_;
  std::filesystem::path temp_path_;
  int file_descriptor_ = -1;
  uint64_t shard_count_ = 0;
  uint64_t total_entry_count_ = 0;
  uint64_t checksum_ = 0;  // Running FNV-1a64 state, folded incrementally as bytes are written.
  bool finished_ = false;
};

// Reads and validates a sharded T1 checkpoint file by mmap'ing it (MAP_PRIVATE, read-only).
// Throws std::system_error / std::runtime_error if the file is missing or fails validation (bad
// magic, format_version, size, or checksum). boundaries()/shard_entries() stay valid for this
// object's lifetime.
class ShardedT1CheckpointFile {
 public:
  explicit ShardedT1CheckpointFile(const std::filesystem::path &path);
  ~ShardedT1CheckpointFile() noexcept;

  ShardedT1CheckpointFile(const ShardedT1CheckpointFile &) = delete;
  auto operator=(const ShardedT1CheckpointFile &) -> ShardedT1CheckpointFile & = delete;
  ShardedT1CheckpointFile(ShardedT1CheckpointFile &&) = delete;
  auto operator=(ShardedT1CheckpointFile &&) -> ShardedT1CheckpointFile & = delete;

  [[nodiscard]] auto boundaries() const noexcept -> std::span<const T1ChkKeyPrefix>;
  [[nodiscard]] auto shard_count() const noexcept -> size_t { return shard_entries_.size(); }
  [[nodiscard]] auto shard_entries(size_t index) const noexcept -> std::span<const T1ChkEntry> {
    return shard_entries_[index];
  }

 private:
  void *mapped_ = nullptr;
  size_t mapped_bytes_ = 0;
  std::vector<std::span<const T1ChkEntry>> shard_entries_;
  const T1ChkKeyPrefix *boundaries_ = nullptr;
  size_t boundary_count_ = 0;
};

// ─── Manifest (<t2_path>.manifest) ─────────────────────────────────────────
//
// The single source of truth for "how much of the T1/T2 checkpoint files is durable and safe to
// trust". `generation` is the checkpoint LSN (5.3): the WAL replay boundary, and the point up to
// which T1's checkpoint file and T2's live data file are guaranteed consistent with each other.
// Neither file is trusted by a reader beyond what the manifest promises until the manifest itself
// is committed (write to temp, fsync, rename atomically). This is what makes the pair crash-safe
// (5.2/5.3).

inline constexpr uint32_t kManifestMagic = 0x314B4D56;  // ASCII "VMK1"
// A v1/v2/v3 manifest fails the format_version check in read_manifest(), so it's treated
// identically to "no checkpoint exists yet" (same contract as any other corrupt/missing
// manifest); no on-disk migration path exists or is needed.
inline constexpr uint8_t kManifestFormatVersion = 4;

struct ManifestHeader {
  uint32_t magic = kManifestMagic;
  uint8_t format_version = kManifestFormatVersion;
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  uint8_t reserved[3] = {};
  // == checkpoint_lsn of the cycle that committed this manifest: the WAL replay boundary.
  // derive_t1_chk_path()/derive_t2_chk_path() name fixed paths independent of this value -- see
  // their own comments.
  uint64_t generation = 0;
  uint64_t t2_bytes_used = 0;  // Logical bytes durably written to the T2 checkpoint file -- not
                               // recoverable from that file's size alone (it may be truncated to
                               // a larger capacity than its live data occupies).
  uint64_t checksum = 0;       // FNV-1a64 over the header with checksum zeroed.
};
inline constexpr size_t kManifestHeaderBytes = 32;
static_assert(sizeof(ManifestHeader) == kManifestHeaderBytes);
static_assert(std::is_standard_layout_v<ManifestHeader>);

struct ManifestData {
  uint64_t generation = 0;
  uint64_t t2_bytes_used = 0;
};

// Derives the sibling manifest path for a given T2 flat-file path.
inline auto derive_manifest_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".manifest"};
}

// Derives the T1 checkpoint / T2 checkpoint file paths. Each is a single path reused across
// every checkpoint this store ever commits: the T1 file is fully rewritten each cycle via
// write_t1_checkpoint()'s temp+rename, and the T2 file is the store's one persistent data file,
// appended to in place. Neither is trusted by a reader until the manifest names the generation
// that last wrote them (5.3).
inline auto derive_t1_chk_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".t1chk"};
}
inline auto derive_t2_chk_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".t2chk"};
}

// Writes the manifest to a temp file beside `manifest_path` and renames it atomically onto
// `manifest_path`. Call only after `generation`'s T1 and T2 checkpoint files are already fully
// written and fsynced.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void write_manifest(const std::filesystem::path &manifest_path, uint64_t generation, uint64_t t2_bytes_used);

// Reads and validates the manifest at `manifest_path`. Returns the generation/t2_bytes_used on
// success; std::nullopt if the file does not exist or fails validation (bad magic,
// format_version, or checksum) -- callers must treat that identically to "no checkpoint exists
// yet".
auto read_manifest(const std::filesystem::path &manifest_path) -> std::optional<ManifestData>;

}  // namespace vmemkv
