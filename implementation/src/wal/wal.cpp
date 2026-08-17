#include "wal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

#include "../api/utils.hpp"

namespace vmemkv {

namespace {

auto compute_checksum(const WalRecordHeader &header,
                      std::span<const std::byte> key,
                      std::span<const std::byte> value) noexcept -> uint64_t {
  uint64_t hash = checksum_header(header);
  hash = fnv1a64_update(hash, key.data(), key.size());
  hash = fnv1a64_update(hash, value.data(), value.size());
  return hash;
}

constexpr mode_t kWalFilePermissions = 0600;

// Iteration count at which a yield()-retry loop below is considered "stalled" rather than paying
// its expected brief backpressure cost. High enough to never fire in normal operation, low enough
// to still surface a genuine hang as a log line within seconds even on an oversubscribed box.
constexpr uint64_t kStallWarnThreshold = 20'000'000;
constexpr uint64_t kIterationsPerMillionForLog = 1'000'000;

}  // namespace

Wal::Wal(const std::filesystem::path &path) : path_(path) {
  const int local_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, kWalFilePermissions);
  if (local_fd < 0) {
    throw std::system_error(errno, std::generic_category(), "open wal");
  }
  fd_.store(local_fd, std::memory_order_relaxed);
  try {
    validate_and_recover_tail();
  } catch (...) {
    // Thrown mid-construction: ~Wal() will never run for this partially-constructed
    // object, so fd_ must be closed here or it leaks for the life of the process.
    ::close(local_fd);
    fd_.store(-1, std::memory_order_relaxed);
    throw;
  }
  // Relaxed is fine (nothing concurrent yet); this line is about correctness, not ordering --
  // without it, next_to_flush_ would stay at 1 while next_lsn_ is already far ahead on a
  // reopened WAL, and the first leader would spin forever waiting for ring slots that were
  // never published in this process.
  next_to_flush_ = next_lsn_.load(std::memory_order_relaxed);
}

Wal::~Wal() noexcept {
  const int local_fd = fd_.load(std::memory_order_relaxed);
  if (local_fd >= 0) {
    ::close(local_fd);
  }
}

void Wal::validate_and_recover_tail() {
  const int local_fd = fd_.load(std::memory_order_relaxed);
  struct stat file_stat {};
  if (::fstat(local_fd, &file_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "fstat wal");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);

  uint64_t offset = 0;
  uint64_t last_valid_lsn = 0;

  while (file_size - offset >= sizeof(WalRecordHeader)) {
    const WalRecordHeader header = read_header_at(offset);

    if (header.magic != kWalRecordMagic || header.format_version != kWalFormatVersion) {
      break;  // Corrupt, or a record layout this build doesn't understand: stop and truncate here.
    }

    const uint64_t payload_len = static_cast<uint64_t>(header.key_len) + header.value_len;
    if (file_size - offset - sizeof(WalRecordHeader) < payload_len) {
      break;  // Torn payload: stop and truncate at this offset.
    }

    const std::vector<std::byte> payload = read_payload_at(offset + sizeof(WalRecordHeader), payload_len);

    const std::span<const std::byte> key_span(payload.data(), header.key_len);
    const std::span<const std::byte> value_span(payload.data() + header.key_len, header.value_len);
    if (compute_checksum(header, key_span, value_span) != header.checksum) {
      break;  // Corrupt: stop and truncate at this offset.
    }

    offset += sizeof(WalRecordHeader) + payload_len;
    last_valid_lsn = header.lsn;
  }

  if (offset < file_size) {
    if (::ftruncate(local_fd, static_cast<off_t>(offset)) != 0) {
      throw std::system_error(errno, std::generic_category(), "ftruncate wal");
    }
  }

  next_lsn_.store(last_valid_lsn + 1, std::memory_order_relaxed);
}

auto Wal::read_header_at(uint64_t offset) const -> WalRecordHeader {
  WalRecordHeader header;
  const ssize_t header_read =
      ::pread(fd_.load(std::memory_order_acquire), &header, sizeof(header), static_cast<off_t>(offset));
  if (header_read != static_cast<ssize_t>(sizeof(header))) {
    throw std::system_error(errno, std::generic_category(), "pread wal header");
  }
  return header;
}

auto Wal::read_payload_at(uint64_t offset, uint64_t payload_len) const -> std::vector<std::byte> {
  std::vector<std::byte> payload(payload_len);
  if (payload_len > 0) {
    const ssize_t payload_read =
        ::pread(fd_.load(std::memory_order_acquire), payload.data(), payload_len, static_cast<off_t>(offset));
    if (payload_read != static_cast<ssize_t>(payload_len)) {
      throw std::system_error(errno, std::generic_category(), "pread wal payload");
    }
  }
  return payload;
}

// See declaration in wal.hpp. noexcept: must never throw while already unwinding another
// exception.
void Wal::fail_all_pending_and_release_leadership(const std::exception_ptr &err) noexcept {
  poisoned_.store(true, std::memory_order_release);
  while (true) {
    const uint64_t target = next_lsn_.load(std::memory_order_acquire);
    while (next_to_flush_ != target) {
      const size_t slot = next_to_flush_ % kWalRingCapacity;
      PendingRecord *rec = ring_[slot].load(std::memory_order_acquire);
      while (rec == nullptr) {
        std::this_thread::yield();
        rec = ring_[slot].load(std::memory_order_acquire);
      }
      rec->error = err;
      ring_[slot].store(nullptr, std::memory_order_release);
      rec->release();
      ++next_to_flush_;
    }
    if (next_lsn_.load(std::memory_order_acquire) == target) {
      break;
    }
  }
  // One shared wake-up for everything just failed, mirroring write_and_fsync_batch's group-commit
  // notification. next_to_flush_ - 1 is the highest lsn settled so far (next_to_flush_ itself was
  // never reserved by anyone -- it's next_lsn_'s value at the point we stopped); harmless to
  // re-store/re-notify even if this particular sweep had nothing new to fail (next_to_flush_
  // starts at >= 1, so this never underflows).
  highest_settled_lsn_.store(next_to_flush_ - 1, std::memory_order_release);
  highest_settled_lsn_.notify_all();
  flushing_.store(false, std::memory_order_release);
  flushing_.notify_all();
}

// Collects up to kWalRingCapacity records into `batch`, advancing next_to_flush_. The bound is
// what makes ring-slot reuse safe: without it, a collection pass could wrap around and treat a
// stale, not-yet-nulled slot as the LSN being sought, writing the wrong record under the wrong
// LSN. Throws if a slot's occupant disagrees with its expected LSN -- should be structurally
// unreachable given the bound, but durability-critical code must not silently trust ring contents.
void Wal::collect_batch(uint64_t target, std::vector<PendingRecord *> &batch) {
  while (next_to_flush_ != target && batch.size() < kWalRingCapacity) {
    const size_t slot = next_to_flush_ % kWalRingCapacity;
    PendingRecord *rec = ring_[slot].load(std::memory_order_acquire);
    uint64_t spins = 0;
    while (rec == nullptr) {
      // fetch_add for this LSN already happened (it's < target), but the producer's CAS-publish
      // into the slot hasn't landed yet -- an unavoidable, brief window.
      std::this_thread::yield();
      rec = ring_[slot].load(std::memory_order_acquire);
      if (++spins == kStallWarnThreshold) {
        // Diagnostic tripwire, not a correctness fix: if the leader is stuck here, no thread is
        // doing real write()/fsync() I/O yet (write_and_fsync_batch() isn't entered until this
        // loop returns). Fires at most once per stall to avoid spamming the log.
        // <print>/std::println needs GCC 14+; this toolchain (GCC 13) doesn't have it yet.
        std::fprintf(  // NOLINT(modernize-use-std-print)
            stderr,
            "wal: leader stalled >%luM yields waiting for lsn=%lu (slot=%zu) to be "
            "published (next_lsn_=%lu)\n",
            static_cast<unsigned long>(kStallWarnThreshold / kIterationsPerMillionForLog),
            static_cast<unsigned long>(next_to_flush_),
            slot,
            static_cast<unsigned long>(next_lsn_.load(std::memory_order_relaxed)));
      }
    }
    if (rec->lsn != next_to_flush_) {
      throw std::logic_error("wal ring buffer invariant violated: slot lsn mismatch");
    }
    batch.push_back(rec);
    ++next_to_flush_;
  }
}

// Writes `batch` via writev() (chunked to IOV_MAX iovecs), then one shared fdatasync(). A single
// writev()/fdatasync() failure fails every record in the batch. On failure, best-effort
// ftruncate()s back to the batch's starting length (so a failed record can never resurface on a
// later replay) and poisons the Wal. Notifies and releases every record regardless of outcome.
void Wal::write_and_fsync_batch(const std::vector<PendingRecord *> &batch) {
  const int local_fd = fd_.load(std::memory_order_acquire);
  struct stat file_stat {};
  const uint64_t saved_length = (::fstat(local_fd, &file_stat) == 0) ? static_cast<uint64_t>(file_stat.st_size) : 0;

  std::exception_ptr io_err;
  try {
    std::vector<iovec> iov;
    iov.reserve(std::min(batch.size(), static_cast<size_t>(IOV_MAX)));
    size_t offset = 0;
    while (offset < batch.size()) {
      const size_t chunk = std::min(batch.size() - offset, static_cast<size_t>(IOV_MAX));
      iov.clear();
      size_t chunk_bytes = 0;
      for (size_t i = 0; i < chunk; ++i) {
        PendingRecord *rec = batch[offset + i];
        // writev() takes non-const iovecs by convention only; it never writes through them.
        iov.push_back(iovec{const_cast<std::byte *>(rec->buffer.data()), rec->buffer.size()});
        chunk_bytes += rec->buffer.size();
      }
      const ssize_t written = ::writev(local_fd, iov.data(), static_cast<int>(iov.size()));
      if (written != static_cast<ssize_t>(chunk_bytes)) {
        throw std::system_error(errno, std::generic_category(), "writev wal batch");
      }
      offset += chunk;
    }
    // fdatasync() rather than fsync() -- see class contract in wal.hpp for why.
    if (::fdatasync(local_fd) != 0) {
      throw std::system_error(errno, std::generic_category(), "fdatasync wal");
    }
  } catch (...) {
    io_err = std::current_exception();
    const int truncate_result = ::ftruncate(local_fd, static_cast<off_t>(saved_length));
    (void)truncate_result;  // Best-effort; nothing more we can do if this also fails.
    poisoned_.store(true, std::memory_order_release);
  }

  // Captured before anything below can release() a record -- batch is ordered by increasing lsn
  // (collect_batch appends in next_to_flush_ order), so back() is this round's highest.
  const uint64_t round_last_lsn = batch.back()->lsn;

  for (auto *rec : batch) {
    rec->error = io_err;
  }

  // One shared wake-up for the whole round instead of a per-record notify_one() loop -- see class
  // contract in wal.hpp.
  highest_settled_lsn_.store(round_last_lsn, std::memory_order_release);
  highest_settled_lsn_.notify_all();

  for (auto *rec : batch) {
    ring_[rec->lsn % kWalRingCapacity].store(nullptr, std::memory_order_release);
    rec->release();
  }
}

auto Wal::drain_pending() -> uint64_t {
  // Snapshot the target once, not fresh on every iteration: under sustained concurrent writers,
  // next_lsn_ keeps advancing, so re-reading it each pass chases a moving target and never
  // returns (measured directly: rotate()'s post-swap drain hung 10+ seconds under 19 concurrent
  // insert threads, even though it kept making real, useful progress -- see rotate()'s own
  // comment). Everything reserved as of this call's start (this snapshot) is what durability up
  // to "now" means; anything reserved after is the next drain_pending() call's job -- via
  // release_leadership()'s own single-retry handling for its callers, not this function chasing
  // it internally. The loop below still repeats -- collect_batch() caps each round at
  // kWalRingCapacity, so more than one round can be needed to reach even a fixed target -- but
  // that's now bounded by the backlog size at entry, not by how long writers keep arriving.
  const uint64_t target = next_lsn_.load(std::memory_order_acquire);
  while (next_to_flush_ != target) {
    try {
      std::vector<PendingRecord *> batch;
      collect_batch(target, batch);
      write_and_fsync_batch(batch);
    } catch (...) {
      fail_all_pending_and_release_leadership(std::current_exception());
      throw;
    }
  }
  return target;
}

auto Wal::release_leadership(uint64_t observed_target) -> bool {
  flushing_.store(false, std::memory_order_release);
  flushing_.notify_all();
  if (next_lsn_.load(std::memory_order_acquire) == observed_target) {
    return false;  // Genuinely caught up as of `observed_target` -- safe to stop being leader.
  }
  // Someone reserved an LSN in the exact window between the last check and this release --
  // reclaim leadership rather than stranding their record.
  if (flushing_.exchange(true, std::memory_order_acq_rel)) {
    return false;  // Someone else already reclaimed leadership in that same window; their problem now.
  }
  return true;  // We reclaimed it -- caller should drain_pending() again.
}

auto Wal::reserve_record(WalRecordType type,
                         std::span<const std::byte> key,
                         std::span<const std::byte> value) -> PendingRecord * {
  if (poisoned_.load(std::memory_order_acquire)) {
    throw std::runtime_error("WAL is poisoned after an earlier durability failure");
  }

  WalRecordHeader header;
  header.key_len = static_cast<uint32_t>(key.size());
  header.value_len = static_cast<uint32_t>(value.size());
  header.type = static_cast<uint8_t>(type);

  auto owned_rec = std::make_unique<PendingRecord>();
  owned_rec->buffer.resize(sizeof(header) + key.size() + value.size());
  if (!key.empty()) {
    std::memcpy(owned_rec->buffer.data() + sizeof(header), key.data(), key.size());
  }
  if (!value.empty()) {
    std::memcpy(owned_rec->buffer.data() + sizeof(header) + key.size(), value.data(), value.size());
  }

  const uint64_t lsn = next_lsn_.fetch_add(1, std::memory_order_acq_rel);
  owned_rec->lsn = lsn;
  header.lsn = lsn;
  header.checksum = compute_checksum(header, key, value);
  std::memcpy(owned_rec->buffer.data(), &header, sizeof(header));

  // Ownership transfers to the ring (PendingRecord::refcount already accounts for both the
  // producer's and the ring/flusher's reference -- see its declaration).
  PendingRecord *rec = owned_rec.release();

  const size_t slot = lsn % kWalRingCapacity;
  PendingRecord *expected = nullptr;
  uint64_t publish_spins = 0;
  while (!ring_[slot].compare_exchange_weak(expected, rec, std::memory_order_acq_rel)) {
    expected = nullptr;
    // Backpressure: this slot's prior occupant hasn't been retired yet. Should rarely spin
    // meaningfully given kWalRingCapacity's margin over max concurrent callers.
    std::this_thread::yield();
    if (++publish_spins == kStallWarnThreshold) {
      // Matching tripwire to collect_batch()'s: fires when a producer is stuck waiting for the
      // ring to make room, i.e. the leader isn't retiring slots. Seeing this without the
      // collect_batch() tripwire narrows down where the stall is.
      std::fprintf(  // NOLINT(modernize-use-std-print) -- see the matching NOLINT in collect_batch() above.
          stderr,
          "wal: producer stalled >%luM yields waiting for slot=%zu to free (lsn=%lu, "
          "next_lsn_=%lu)\n",
          static_cast<unsigned long>(kStallWarnThreshold / kIterationsPerMillionForLog),
          slot,
          static_cast<unsigned long>(lsn),
          static_cast<unsigned long>(next_lsn_.load(std::memory_order_relaxed)));
    }
  }

  return rec;
}

auto Wal::await_durable(PendingRecord *rec) -> uint64_t {
  const uint64_t lsn = rec->lsn;
  const bool is_leader = !flushing_.exchange(true, std::memory_order_acq_rel);

  if (!is_leader) {
    uint64_t settled = highest_settled_lsn_.load(std::memory_order_acquire);
    while (settled < lsn) {
      // Plain wait(), not a spin-then-wait hybrid: a PAUSE-spin-then-yield() hybrid (mirroring
      // RocksDB's WriteThread::AwaitState()) was tried here to let the leader's notify skip its
      // FUTEX_WAKE syscall. It helped in an isolated WAL-only benchmark but made the full system
      // worse -- spinning steals cycles from other concurrent work that isn't free outside
      // isolation. Reverted to a plain wait().
      highest_settled_lsn_.wait(settled, std::memory_order_acquire);
      settled = highest_settled_lsn_.load(std::memory_order_acquire);
    }
    const std::exception_ptr err = rec->error;
    rec->release();
    if (err) {
      std::rethrow_exception(err);
    }
    return lsn;
  }

  while (true) {
    const uint64_t target = drain_pending();
    if (!release_leadership(target)) {
      break;
    }
  }

  const std::exception_ptr err = rec->error;
  rec->release();
  if (err) {
    std::rethrow_exception(err);
  }
  return lsn;
}

auto Wal::reserve_insert(std::span<const std::byte> key, std::span<const std::byte> value) -> PendingRecord * {
  return reserve_record(WalRecordType::Insert, key, value);
}

auto Wal::reserve_update(std::span<const std::byte> key, std::span<const std::byte> value) -> PendingRecord * {
  return reserve_record(WalRecordType::Update, key, value);
}

auto Wal::reserve_delete(std::span<const std::byte> key) -> PendingRecord * {
  return reserve_record(WalRecordType::Delete, key, std::span<const std::byte>{});
}

auto Wal::replay(const WalReplayCallback &callback) const -> uint64_t {
  const int local_fd = fd_.load(std::memory_order_acquire);
  struct stat file_stat {};
  if (::fstat(local_fd, &file_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "fstat wal replay");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);

  uint64_t offset = 0;
  uint64_t count = 0;

  while (file_size - offset >= sizeof(WalRecordHeader)) {
    const WalRecordHeader header = read_header_at(offset);
    const uint64_t payload_len = static_cast<uint64_t>(header.key_len) + header.value_len;
    const std::vector<std::byte> payload = read_payload_at(offset + sizeof(WalRecordHeader), payload_len);

    const std::span<const std::byte> key_span(payload.data(), header.key_len);
    const std::span<const std::byte> value_span(payload.data() + header.key_len, header.value_len);

    callback(static_cast<WalRecordType>(header.type), key_span, value_span, header.lsn);
    ++count;

    offset += sizeof(WalRecordHeader) + payload_len;
  }

  return count;
}

void Wal::rotate(uint64_t checkpoint_lsn) {
  if (poisoned_.load(std::memory_order_acquire)) {
    throw std::runtime_error("WAL is poisoned, refusing to rotate");
  }

  // Become leader ourselves rather than "wait until flushing_ is false" -- the latter has a TOCTOU
  // where another thread could grab leadership between this call waking and acting on fd_.
  while (flushing_.exchange(true, std::memory_order_acq_rel)) {
    flushing_.wait(true, std::memory_order_acquire);
  }

  (void)drain_pending();  // Catch up on any backlog before touching fd_ at all.

  const int old_fd = fd_.load(std::memory_order_acquire);
  struct stat file_stat {};
  if (::fstat(old_fd, &file_stat) != 0) {
    fail_all_pending_and_release_leadership(
        std::make_exception_ptr(std::system_error(errno, std::generic_category(), "fstat wal (rotate)")));
    throw std::system_error(errno, std::generic_category(), "fstat wal (rotate)");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);

  const std::filesystem::path temp_path = path_.string() + ".rotate_tmp";
  const int new_fd = ::open(temp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, kWalFilePermissions);
  if (new_fd < 0) {
    const auto err =
        std::make_exception_ptr(std::system_error(errno, std::generic_category(), "open wal rotation temp file"));
    fail_all_pending_and_release_leadership(err);
    std::rethrow_exception(err);
  }

  struct NewFdGuard {
    int local_fd;
    ~NewFdGuard() {
      if (local_fd >= 0) {
        ::close(local_fd);
      }
    }
  } new_fd_guard{new_fd};

  try {
    uint64_t offset = 0;

    // Source file trusted as-is: rotate() only runs against a Wal this process has appended to
    // since a clean construction, so no magic/checksum re-validation is needed here.
    while (file_size - offset >= sizeof(WalRecordHeader)) {
      const WalRecordHeader header = read_header_at(offset);
      const uint64_t record_len = sizeof(header) + header.key_len + header.value_len;

      if (header.lsn > checkpoint_lsn) {
        const std::vector<std::byte> record_buf = read_payload_at(offset, record_len);
        const ssize_t written = ::write(new_fd, record_buf.data(), record_len);
        if (written != static_cast<ssize_t>(record_len)) {
          throw std::system_error(errno, std::generic_category(), "write wal record (rotate)");
        }
      }

      offset += record_len;
    }

    if (::fsync(new_fd) != 0) {
      throw std::system_error(errno, std::generic_category(), "fsync rotated wal");
    }

    std::error_code rename_ec;
    std::filesystem::rename(temp_path, path_, rename_ec);
    if (rename_ec) {
      throw std::system_error(rename_ec, "rename rotated wal");
    }
  } catch (...) {
    fail_all_pending_and_release_leadership(std::current_exception());
    throw;
  }

  ::close(old_fd);
  fd_.store(new_fd_guard.local_fd, std::memory_order_release);
  new_fd_guard.local_fd = -1;  // Ownership transferred to fd_; the guard must not close it too.
  // Deliberately NOT touching next_lsn_ here: a producer can fetch_add+publish a new LSN into the
  // ring while this scan is running, invisible to this file-based scan since it isn't on disk yet.
  // Resetting next_lsn_ from scan results could roll it backward past an already-handed-out LSN,
  // corrupting the "byte-offset order == LSN order" invariant replay/recovery depend on. next_lsn_
  // is self-sufficient; rotation only discards old survivors, it never renumbers anyone.

  // Flush whatever accumulated in the ring while we held leadership for the file swap, then hand
  // off leadership plainly -- deliberately not release_leadership()'s reclaim-on-straggler retry
  // (used by the steady-state write path at the top of this file): under sustained concurrent
  // write load, every backlogged writer is parked waiting on highest_settled_lsn_, not competing
  // for `flushing_`, until its own LSN settles -- so rotate()'s thread was structurally the only
  // party ever re-attempting that reclaim, kept "winning" against its own moving target, and never
  // returned (measured directly: hung 10+ seconds under 19 concurrent insert threads with zero
  // other reclaim attempts). A writer reserved after this plain release still gets serviced
  // exactly like any other moment with no rotation in progress: its own reserve+await call makes
  // its own fresh `!flushing_.exchange(true)` attempt right after reserving, so it either becomes
  // leader itself or waits for whoever does -- nothing is left permanently stranded, just no
  // longer rotate()'s personal responsibility to chase.
  (void)drain_pending();
  flushing_.store(false, std::memory_order_release);
  flushing_.notify_all();
}

auto Wal::next_lsn() const noexcept -> uint64_t { return next_lsn_.load(std::memory_order_relaxed); }

auto Wal::size_bytes() const -> uint64_t {
  struct stat file_stat {};
  if (::fstat(fd_.load(std::memory_order_acquire), &file_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "fstat wal (size_bytes)");
  }
  return static_cast<uint64_t>(file_stat.st_size);
}

}  // namespace vmemkv
