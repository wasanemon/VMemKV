// wal.hpp - Write-Ahead Log for VMemKV crash recovery.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <type_traits>

namespace vmemkv {

enum class WalRecordType : uint8_t { Insert = 1, Update = 2, Delete = 3 };

inline constexpr uint32_t kWalRecordMagic = 0x574C4B31;  // ASCII "1KLW", sentinel for torn/corrupt detection.

// Discriminates the on-disk record layout, independent of kWalRecordMagic (which only signals
// "this looks like a record header at all", not which layout it follows). A future format change
// bumps this rather than overloading magic, giving readers an explicit place to branch instead of
// having to infer format from incidental byte patterns.
inline constexpr uint8_t kWalFormatVersion = 1;

// 8+8+4+4+4+1+1+2 = 32B, naturally aligned, no implicit padding.
struct WalRecordHeader {
  uint64_t lsn = 0;
  uint64_t checksum = 0;  // FNV-1a64 over header(checksum zeroed)+key+value.
  uint32_t magic = kWalRecordMagic;
  uint32_t key_len = 0;
  uint32_t value_len = 0;  // 0 for Delete.
  uint8_t type = 0;
  uint8_t format_version = kWalFormatVersion;
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  uint8_t reserved[2] = {};  // Explicit so the checksum input has no UB padding bytes.
};
inline constexpr size_t kWalRecordHeaderBytes = 32;
static_assert(sizeof(WalRecordHeader) == kWalRecordHeaderBytes);
static_assert(std::is_standard_layout_v<WalRecordHeader>);

// Derives the sibling WAL path for a given T2 flat-file path (path + ".wal").
inline auto derive_wal_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".wal"};
}

using WalReplayCallback = std::function<void(
    WalRecordType type, std::span<const std::byte> key, std::span<const std::byte> value, uint64_t lsn)>;

// Sequential Write-Ahead Log.
//
// Contract: an append_*() call does not return until its record is durably fsynced, and the
// only record a crash can ever tear is the physically last one written. Today that contract is
// met by the simplest possible strategy -- one write() immediately followed by one fsync() per
// call, serialized under a single mutex -- but callers must not rely on that one-syscall-pair-
// per-call mechanism itself: a future group-commit rewrite (batching several pending callers'
// records into one write()+fsync(), waking them all once it completes) is a valid internal
// change under this same contract and would not require touching append_insert/update/delete's
// signatures or any call site.
//
// The constructor scans the file once and truncates at the first invalid record (torn header,
// torn payload, bad magic, unrecognized format_version, or checksum mismatch), guaranteeing the
// file is clean by the time construction completes.
class Wal {
 public:
  explicit Wal(const std::filesystem::path &path);
  ~Wal() noexcept;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;
  Wal(Wal &&) = delete;
  auto operator=(Wal &&) -> Wal & = delete;

  // Each call durably appends one record before returning (see class contract above).
  auto append_insert(std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t;
  auto append_update(std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t;
  auto append_delete(std::span<const std::byte> key) -> uint64_t;

  // Reads the whole log from the beginning and invokes callback for each valid record in order.
  // Returns the number of records replayed.
  auto replay(const WalReplayCallback &callback) const -> uint64_t;

  [[nodiscard]] auto next_lsn() const noexcept -> uint64_t;

 private:
  auto append_record(WalRecordType type, std::span<const std::byte> key, std::span<const std::byte> value) -> uint64_t;
  void validate_and_recover_tail();  // ctor-only: classify+truncate corrupt tail.

  int fd_ = -1;
  mutable std::mutex append_mutex_;  // One physical sequential file needs one writer order.
  std::atomic<uint64_t> next_lsn_{1};
};

}  // namespace vmemkv
