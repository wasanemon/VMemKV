#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace pskiplist {

inline constexpr uint32_t kManifestMagic = 0x504b4c31;  // "PKL1"
inline constexpr uint8_t kManifestFormatVersion = 1;

// 32B fixed manifest header (3章). A mismatched format_version is treated identically to
// a missing manifest — no migration is implemented.
struct ManifestHeader {
  uint32_t magic = kManifestMagic;
  uint8_t format_version = kManifestFormatVersion;
  uint8_t reserved[3] = {};
  uint64_t epoch = 0;            // node epoch stamps <= this are trusted as durable
  uint64_t high_water_mark = 0;  // bump allocation's last reached offset
  uint64_t checksum = 0;         // FNV-1a64 of this header with checksum itself zeroed
};
static_assert(sizeof(ManifestHeader) == 32);

[[nodiscard]] inline auto manifest_checksum(const ManifestHeader &header) -> uint64_t {
  ManifestHeader zeroed = header;
  zeroed.checksum = 0;
  const auto *bytes = reinterpret_cast<const unsigned char *>(&zeroed);
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < sizeof(zeroed); ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

[[nodiscard]] inline auto manifest_path(const std::filesystem::path &data_path) -> std::filesystem::path {
  auto path = data_path;
  path += ".manifest";
  return path;
}

// Writes the manifest for `data_path` via temp file + fsync + atomic rename (3章), so a
// reader never observes a partially-written manifest.
inline void write_manifest(const std::filesystem::path &data_path, uint64_t epoch, uint64_t high_water_mark) {
  ManifestHeader header;
  header.epoch = epoch;
  header.high_water_mark = high_water_mark;
  header.checksum = manifest_checksum(header);

  const auto final_path = manifest_path(data_path);
  auto tmp_path = final_path;
  tmp_path += ".tmp";

  const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "open(" + tmp_path.string() + ")");
  }
  const ssize_t written = ::write(fd, &header, sizeof(header));
  if (written != static_cast<ssize_t>(sizeof(header))) {
    const int err = errno;
    ::close(fd);
    throw std::system_error(err, std::generic_category(), "write(" + tmp_path.string() + ")");
  }
  if (::fsync(fd) != 0) {
    const int err = errno;
    ::close(fd);
    throw std::system_error(err, std::generic_category(), "fsync(" + tmp_path.string() + ")");
  }
  ::close(fd);

  std::error_code ec;
  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    throw std::system_error(ec, "rename(" + tmp_path.string() + " -> " + final_path.string() + ")");
  }
}

// Returns nullopt if the manifest is missing, truncated, has a mismatched magic or
// format_version, or fails its checksum — any of these are treated as "no checkpoint has
// ever completed for this file".
[[nodiscard]] inline auto read_manifest(const std::filesystem::path &data_path) -> std::optional<ManifestHeader> {
  const auto path = manifest_path(data_path);
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return std::nullopt;

  ManifestHeader header{};
  const ssize_t bytes_read = ::read(fd, &header, sizeof(header));
  ::close(fd);
  if (bytes_read != static_cast<ssize_t>(sizeof(header))) return std::nullopt;
  if (header.magic != kManifestMagic) return std::nullopt;
  if (header.format_version != kManifestFormatVersion) return std::nullopt;
  if (manifest_checksum(header) != header.checksum) return std::nullopt;

  return header;
}

}  // namespace pskiplist
