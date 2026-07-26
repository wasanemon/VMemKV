#include "wal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <system_error>
#include <vector>

namespace vmemkv {

namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

auto fnv1a64_update(uint64_t hash, const void *data, size_t size) noexcept -> uint64_t {
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
  return hash;
}

auto compute_checksum(const WalRecordHeader &header,
                      std::span<const std::byte> key,
                      std::span<const std::byte> value) noexcept -> uint64_t {
  WalRecordHeader header_for_hash = header;
  header_for_hash.checksum = 0;
  uint64_t hash = kFnvOffsetBasis;
  hash = fnv1a64_update(hash, &header_for_hash, sizeof(header_for_hash));
  hash = fnv1a64_update(hash, key.data(), key.size());
  hash = fnv1a64_update(hash, value.data(), value.size());
  return hash;
}

constexpr mode_t kWalFilePermissions = 0600;

}  // namespace

Wal::Wal(const std::filesystem::path &path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, kWalFilePermissions);
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "open wal");
  }
  try {
    validate_and_recover_tail();
  } catch (...) {
    // Thrown mid-construction: ~Wal() will never run for this partially-constructed
    // object, so fd_ must be closed here or it leaks for the life of the process.
    ::close(fd_);
    fd_ = -1;
    throw;
  }
}

Wal::~Wal() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void Wal::validate_and_recover_tail() {
  struct stat file_stat {};
  if (::fstat(fd_, &file_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "fstat wal");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);

  uint64_t offset = 0;
  uint64_t last_valid_lsn = 0;

  while (file_size - offset >= sizeof(WalRecordHeader)) {
    WalRecordHeader header;
    const ssize_t header_read = ::pread(fd_, &header, sizeof(header), static_cast<off_t>(offset));
    if (header_read != static_cast<ssize_t>(sizeof(header))) {
      throw std::system_error(errno, std::generic_category(), "pread wal header");
    }

    if (header.magic != kWalRecordMagic || header.format_version != kWalFormatVersion) {
      break;  // Corrupt, or a record layout this build doesn't understand: stop and truncate here.
    }

    const uint64_t payload_len = static_cast<uint64_t>(header.key_len) + header.value_len;
    if (file_size - offset - sizeof(WalRecordHeader) < payload_len) {
      break;  // Torn payload: stop and truncate at this offset.
    }

    std::vector<std::byte> payload(payload_len);
    if (payload_len > 0) {
      const ssize_t payload_read =
          ::pread(fd_, payload.data(), payload_len, static_cast<off_t>(offset + sizeof(WalRecordHeader)));
      if (payload_read != static_cast<ssize_t>(payload_len)) {
        throw std::system_error(errno, std::generic_category(), "pread wal payload");
      }
    }

    const std::span<const std::byte> key_span(payload.data(), header.key_len);
    const std::span<const std::byte> value_span(payload.data() + header.key_len, header.value_len);
    if (compute_checksum(header, key_span, value_span) != header.checksum) {
      break;  // Corrupt: stop and truncate at this offset.
    }

    offset += sizeof(WalRecordHeader) + payload_len;
    last_valid_lsn = header.lsn;
  }

  if (offset < file_size) {
    if (::ftruncate(fd_, static_cast<off_t>(offset)) != 0) {
      throw std::system_error(errno, std::generic_category(), "ftruncate wal");
    }
  }

  next_lsn_.store(last_valid_lsn + 1, std::memory_order_relaxed);
}

auto Wal::append_record(WalRecordType type,
                        std::span<const std::byte> key,
                        std::span<const std::byte> value) -> uint64_t {
  WalRecordHeader header;
  header.key_len = static_cast<uint32_t>(key.size());
  header.value_len = static_cast<uint32_t>(value.size());
  header.type = static_cast<uint8_t>(type);

  std::vector<std::byte> buffer(sizeof(header) + key.size() + value.size());

  std::lock_guard<std::mutex> lock(append_mutex_);
  const uint64_t lsn = next_lsn_.fetch_add(1, std::memory_order_relaxed);
  header.lsn = lsn;
  header.checksum = compute_checksum(header, key, value);

  std::memcpy(buffer.data(), &header, sizeof(header));
  if (!key.empty()) {
    std::memcpy(buffer.data() + sizeof(header), key.data(), key.size());
  }
  if (!value.empty()) {
    std::memcpy(buffer.data() + sizeof(header) + key.size(), value.data(), value.size());
  }

  const ssize_t written = ::write(fd_, buffer.data(), buffer.size());
  if (written != static_cast<ssize_t>(buffer.size())) {
    throw std::system_error(errno, std::generic_category(), "write wal record");
  }
  if (::fsync(fd_) != 0) {
    throw std::system_error(errno, std::generic_category(), "fsync wal");
  }

  return lsn;
}

auto Wal::append_insert(std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t {
  return append_record(WalRecordType::Insert, key, value);
}

auto Wal::append_update(std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t {
  return append_record(WalRecordType::Update, key, value);
}

auto Wal::append_delete(std::span<const std::byte> key) -> uint64_t {
  return append_record(WalRecordType::Delete, key, std::span<const std::byte>{});
}

auto Wal::replay(const WalReplayCallback &callback) const -> uint64_t {
  std::lock_guard<std::mutex> lock(append_mutex_);

  struct stat file_stat {};
  if (::fstat(fd_, &file_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "fstat wal replay");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);

  uint64_t offset = 0;
  uint64_t count = 0;

  while (file_size - offset >= sizeof(WalRecordHeader)) {
    WalRecordHeader header;
    const ssize_t header_read = ::pread(fd_, &header, sizeof(header), static_cast<off_t>(offset));
    if (header_read != static_cast<ssize_t>(sizeof(header))) {
      throw std::system_error(errno, std::generic_category(), "pread wal header (replay)");
    }

    const uint64_t payload_len = static_cast<uint64_t>(header.key_len) + header.value_len;
    std::vector<std::byte> payload(payload_len);
    if (payload_len > 0) {
      const ssize_t payload_read =
          ::pread(fd_, payload.data(), payload_len, static_cast<off_t>(offset + sizeof(WalRecordHeader)));
      if (payload_read != static_cast<ssize_t>(payload_len)) {
        throw std::system_error(errno, std::generic_category(), "pread wal payload (replay)");
      }
    }

    const std::span<const std::byte> key_span(payload.data(), header.key_len);
    const std::span<const std::byte> value_span(payload.data() + header.key_len, header.value_len);

    callback(static_cast<WalRecordType>(header.type), key_span, value_span, header.lsn);
    ++count;

    offset += sizeof(WalRecordHeader) + payload_len;
  }

  return count;
}

auto Wal::next_lsn() const noexcept -> uint64_t { return next_lsn_.load(std::memory_order_relaxed); }

}  // namespace vmemkv
