// wal.hpp - Write-Ahead Log for VMemKV crash recovery.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace vmemkv {

enum class WalRecordType : uint8_t { Insert = 1, Update = 2, Delete = 3 };

inline constexpr uint32_t kWalRecordMagic = 0x574C4B31;  // ASCII "1KLW", sentinel for torn/corrupt detection.

// Discriminates on-disk record layout, independent of kWalRecordMagic (which only flags "looks
// like a header"). Bump on format changes rather than overloading magic.
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

// Derives the sibling WAL identity path for a given T2 flat-file path (path + ".wal"). Not a file
// itself -- see derive_wal_segment_path() for the actual on-disk segment files this identity
// names.
inline auto derive_wal_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".wal"};
}

// Derives one segment's on-disk path: <wal_path>.<generation>. The WAL is a sequence of these,
// oldest generation first; see Wal::rotate_segment()'s doc comment for the two-generation
// retention scheme that keeps at most two on disk at a time in steady state.
inline auto derive_wal_segment_path(const std::filesystem::path &wal_path,
                                    uint64_t generation) -> std::filesystem::path {
  return {wal_path.string() + "." + std::to_string(generation)};
}

using WalReplayCallback = std::function<void(
    WalRecordType type, std::span<const std::byte> key, std::span<const std::byte> value, uint64_t lsn)>;

// Sequential Write-Ahead Log.
//
// Contract: reserve_insert()/reserve_update()/reserve_delete() + await_durable() together append
// one record and do not consider it done until it is durably fsynced; the only record a crash can
// ever tear is the physically last one written. reserve_*() is the fast half -- builds the
// record, reserves a strictly increasing LSN, and publishes it into the group-commit ring, with
// no blocking wait. await_durable() is the slow half -- leader election and (for a follower) the
// wait for the round's writev()+fdatasync() to complete. This split lets a caller fix a record's
// LSN ordering while still holding its own per-key lock (see VMemKVImpl::insert_impl() et al. in
// vmemkv_impl.hpp), then release that lock before the much longer durability wait, without
// risking a second reserve_*() for the same key racing ahead of it.
//
// Implementation: lock-free group commit. Each reserve_*() call builds its own record buffer and
// reserves an LSN via a single atomic fetch_add, then CAS-publishes it into a fixed-size ring
// buffer slot (lsn % capacity). Whichever caller finds no flush in progress becomes "leader" for
// the round (elected via atomic exchange, same idiom as VMemKVImpl::reorganize()'s
// reorg_running_) and does the physical I/O on everyone's behalf: the round's records via as few
// writev() calls as possible (chunked to IOV_MAX), followed by one shared fdatasync(). fdatasync()
// rather than fsync(): skips syncing metadata that doesn't affect data retrieval, while still
// syncing file size (POSIX-guaranteed) -- same choice RocksDB's WAL makes. A writev()/fdatasync()
// failure fails every record in the round uniformly. All participants (including the leader) are
// woken via one shared highest_settled_lsn_ counter -- the leader does one store+notify_all() per
// round, keeping the per-round wakeup cost independent of how many records the round holds. See
// wal.cpp's Wal::drain_pending()/Wal::release_leadership() for how a leader absorbs backlog that
// arrives mid-flush and how leadership is handed off without stranding a follower.
//
// The WAL is a sequence of segment files (derive_wal_segment_path()), not one growing file.
// rotate_segment() (called once per checkpoint cycle) is how old segments get reclaimed -- see
// its own doc comment for why this needs no per-record scan. The constructor discovers every
// segment already on disk, replays them in generation order to restore next_lsn_, and truncates
// at the first invalid record (torn header, torn payload, bad magic, unrecognized format_version,
// or checksum mismatch) found in the newest (active) one -- see validate_and_recover_tail()'s
// comment for why only the active segment can legitimately have a torn tail.
class Wal {
 public:
  // One caller's fully-serialized record, queued for a group-commit round. Heap-allocated and
  // intrusively refcounted -- exactly two owners (producer and the ring/flusher path), whichever
  // releases last (atomic fetch_sub) deletes it. Refcounting rather than producer-owns-it (a
  // flusher could touch memory the producer already freed) or atomic<shared_ptr> (libstdc++ often
  // spinlocks its refcount, defeating the point of being lock-free here).
  //
  // Public only so reserve_*()/await_durable() can name it; callers must treat it as opaque.
  struct PendingRecord {
    // Plain vector rather than an inline small-buffer-optimized array: measured no throughput
    // benefit, and glibc's malloc is already fast enough here that allocation count isn't the
    // bottleneck.
    std::vector<std::byte> buffer;  // fully serialized header(lsn/checksum patched)+key+value
    uint64_t lsn = 0;
    std::exception_ptr error;
    std::atomic<int> refcount{2};  // producer's ref + ring-slot/flusher's ref

    void release() {
      if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
      }
    }
  };

  explicit Wal(const std::filesystem::path &path);
  ~Wal() noexcept;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;
  Wal(Wal &&) = delete;
  auto operator=(Wal &&) -> Wal & = delete;

  // See class contract above. Each reserve_*() return value must be passed to
  // exactly one await_durable() call: skipping it leaks the record (the leader still writes it
  // regardless) and the caller never learns if the write succeeded. [[nodiscard]] catches that
  // mistake at compile time.
  [[nodiscard]] auto reserve_insert(std::span<const std::byte> key,
                                    std::span<const std::byte> value) -> PendingRecord *;
  [[nodiscard]] auto reserve_update(std::span<const std::byte> key,
                                    std::span<const std::byte> value) -> PendingRecord *;
  [[nodiscard]] auto reserve_delete(std::span<const std::byte> key) -> PendingRecord *;
  auto await_durable(PendingRecord *rec) -> uint64_t;

  // Reads the whole log from the beginning and invokes callback for each valid record in order.
  // Returns the number of records replayed.
  // NOLINTNEXTLINE(modernize-use-nodiscard) -- recover_from_wal() legitimately ignores the count.
  auto replay(const WalReplayCallback &callback) const -> uint64_t;

  // Every generation with an existing segment file on disk for `wal_path`, ascending. Empty if
  // none exist yet. Public so tests/tooling can enumerate segments at the filesystem level
  // (inspection, corruption injection, cleanup) without a live Wal instance -- production code
  // never needs this, since a constructed Wal already tracks its own active fd.
  static auto discover_segments(const std::filesystem::path &wal_path) -> std::vector<uint64_t>;

  // Rolls over to a fresh segment (low_level_design.md 5.5): opens
  // derive_wal_segment_path(path_, generation+1), publishes it as the new active fd, and deletes
  // the segment two generations back (safe by construction -- see the doc comment on the .cpp
  // definition for the argument). Takes no LSN: needs no knowledge of checkpoint_lsn at all, and
  // thus no per-record scan to find where it falls.
  // Briefly becomes group-commit leader only to swap fd_ without racing an in-flight
  // write_and_fsync_batch() against the old fd -- does not drain first (a record landing in
  // either the old or new segment is equally correct, see that same comment).
  void rotate_segment();

  [[nodiscard]] auto next_lsn() const noexcept -> uint64_t;

  // Current active segment's on-disk size -- doubles as "bytes accumulated since the last
  // checkpoint" for the WAL-size trigger (low_level_design.md 4.4), since rotate_segment() starts
  // a fresh, empty active segment exactly once per checkpoint cycle.
  [[nodiscard]] auto size_bytes() const -> uint64_t;

  // How long the most recent rotate_segment() call spent waiting to become group-commit leader
  // (see that function's own comment) before it could safely swap fd_ -- found, via direct
  // measurement, to dominate checkpoint_internal()'s own wall-clock cost under sustained
  // concurrent writers, far more than the msync()/T1-reorganize work checkpoint's duration was
  // originally assumed to be spent on. Scales with how continuously busy the WAL's group-commit
  // leader stays, not with data volume or checkpoint trigger frequency.
  [[nodiscard]] auto last_rotate_leader_wait_us() const noexcept -> uint64_t {
    return last_rotate_leader_wait_us_.load(std::memory_order_relaxed);
  }

 private:
  // Sized above the 256-way key-stripe locking's max concurrent callers (throughput/backpressure
  // tuning only -- NOT what makes wraparound safe; that's collect_batch()'s per-pass bound plus
  // its slot/LSN check, see wal.cpp).
  static constexpr size_t kWalRingCapacity = 4096;
  static_assert((kWalRingCapacity & (kWalRingCapacity - 1)) == 0, "must be a power of two");

  // The fast half of the split API: builds+serializes the record, reserves its LSN via
  // fetch_add, and CAS-publishes it into the ring. No leader election, no blocking wait. Shared
  // implementation behind reserve_insert()/reserve_update()/reserve_delete().
  [[nodiscard]] auto reserve_record(WalRecordType type,
                                    std::span<const std::byte> key,
                                    std::span<const std::byte> value) -> PendingRecord *;

  // Scans one segment fd from the beginning, validating each record's magic/format/checksum and
  // detecting torn payloads, returning the last valid lsn found (0 if none). `allow_truncate`
  // must be true only for the active (newest) segment -- the only one that could have been
  // mid-write at crash time, since every older segment was retired under rotate_segment()'s
  // exclusive leadership, which by construction cannot overlap an in-flight
  // write_and_fsync_batch() (see that method's own comment). A torn tail found with
  // allow_truncate=false is therefore a genuine integrity failure, not an ordinary crash
  // artifact, and throws instead of silently truncating.
  auto scan_and_validate(int fd, bool allow_truncate) const -> uint64_t;

  // Drains and writes+fsyncs everything assigned an LSN so far, looping to absorb backlog that
  // arrives mid-flush. Caller must already hold leadership. Returns the next_lsn_ snapshot caught
  // up to, for release_leadership(). Any exception (including one unrelated to the write/fsync
  // syscalls, e.g. an allocation failure) is funneled through
  // fail_all_pending_and_release_leadership() below, which sweeps and fails every outstanding
  // record and unconditionally releases flushing_ before the exception propagates -- nobody is
  // ever left stranded.
  auto drain_pending() -> uint64_t;

  // Collects up to kWalRingCapacity pending records (starting at next_to_flush_, up to `target`)
  // into `batch`, advancing next_to_flush_. See wal.cpp for why the bound is what makes ring-slot
  // reuse safe, and why a slot/LSN mismatch throws rather than being silently trusted.
  void collect_batch(uint64_t target, std::vector<PendingRecord *> &batch);

  // Writes every record in `batch` via writev() (chunked to IOV_MAX at a time), then one shared
  // fsync() covering the batch. Notifies and releases every record in `batch` regardless of
  // outcome. See wal.cpp for the failure-semantics and ftruncate-on-failure reasoning.
  void write_and_fsync_batch(const std::vector<PendingRecord *> &batch);

  // Sweeps and fails everything outstanding (re-checking next_lsn_ in a loop, so a record
  // published mid-sweep is still caught), poisons the Wal, and unconditionally releases and
  // notifies flushing_. Called only from drain_pending()'s catch block; never throws.
  void fail_all_pending_and_release_leadership(const std::exception_ptr &err) noexcept;

  // Hands off leadership: releases flushing_, then re-checks next_lsn_ for anyone who reserved an
  // LSN in that exact window. If so, reclaims leadership itself (returns true, caller should
  // drain_pending() again) rather than stranding that record. Returns false once genuinely idle
  // or once another thread has already reclaimed leadership.
  auto release_leadership(uint64_t observed_target) -> bool;

  // Shared pread wrappers used by scan_and_validate()/replay(), which otherwise would each
  // hand-roll an identical "pread this many bytes at this offset or throw" sequence. Take an
  // explicit fd (not fd_) since both callers read arbitrary segments, not just the active one.
  [[nodiscard]] auto read_header_at(int fd, uint64_t offset) const -> WalRecordHeader;
  [[nodiscard]] auto read_payload_at(int fd, uint64_t offset, uint64_t payload_len) const -> std::vector<std::byte>;

  // Atomic because replay()/size_bytes() can read fd_ concurrently with rotate_segment()'s
  // close()-then-reassign; a plain int would be a data race (UB) and risk a
  // close-then-fd-number-reused hazard.
  std::atomic<int> fd_{-1};
  // Published by rotate_segment(); see last_rotate_leader_wait_us()'s own comment.
  std::atomic<uint64_t> last_rotate_leader_wait_us_{0};
  std::filesystem::path path_;
  // The active segment's generation number. Touched only by whoever holds flushing_ leadership
  // (rotate_segment() is its only writer), so -- like next_to_flush_ -- no atomic is needed.
  uint64_t active_generation_ = 1;

  // Slots hold pointers only; record lifetime is governed by PendingRecord::refcount,
  // independent of slot reuse.
  std::array<std::atomic<PendingRecord *>, kWalRingCapacity> ring_{};
  std::atomic<uint64_t> next_lsn_{1};  // fetch_add assigns both the LSN and its ring slot.
  // Touched only by the current leader (flushing_ == true); no atomic needed since exactly one
  // thread holds it at a time.
  uint64_t next_to_flush_ = 1;
  std::atomic<bool> flushing_{false};  // CAS/exchange-elected leader flag (mirrors reorg_running_).
  // Highest LSN whose PendingRecord::error is safe to read ("settled", not necessarily durable).
  // One shared notify_all() per round instead of a per-record done-flag; see class contract above.
  // Guarded by settled_mutex_ (not a plain atomic): a follower's wait in await_durable() needs a
  // genuinely bounded wait_for(), which condition_variable provides and atomic<uint64_t>::wait()
  // does not (no timed overload) -- see await_durable()'s own comment for the leadership-handoff
  // gap this closes.
  mutable std::mutex settled_mutex_;
  mutable std::condition_variable settled_cv_;
  uint64_t highest_settled_lsn_ = 0;
  // Sticky once a write()/fsync() round fails: later append_record() calls fail fast. Treat as
  // fatal to the process, not just to WAL calls.
  std::atomic<bool> poisoned_{false};
};

// Test/tooling helper: the currently-active (highest-generation) segment's on-disk path, or
// nullopt if none exist yet. Lets tests inspect or inject bytes into the WAL at the filesystem
// level, bypassing the Wal class entirely -- no production code needs this, since a live Wal
// instance already tracks its own active fd.
inline auto find_active_wal_segment(const std::filesystem::path &wal_path) -> std::optional<std::filesystem::path> {
  const auto generations = Wal::discover_segments(wal_path);
  if (generations.empty()) {
    return std::nullopt;
  }
  return derive_wal_segment_path(wal_path, generations.back());
}

// Test/tooling helper: removes every existing segment file for this WAL identity.
inline void remove_wal_segments(const std::filesystem::path &wal_path) {
  for (const uint64_t generation : Wal::discover_segments(wal_path)) {
    std::error_code ignored;
    std::filesystem::remove(derive_wal_segment_path(wal_path, generation), ignored);
  }
}

}  // namespace vmemkv
