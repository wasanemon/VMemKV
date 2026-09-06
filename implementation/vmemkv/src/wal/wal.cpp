#include "wal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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

Wal::Wal(std::filesystem::path path) : path_(std::move(path)) {
  const std::vector<uint64_t> generations = discover_segments(path_);

  if (generations.empty()) {
    active_generation_ = 1;
    const auto seg_path = derive_wal_segment_path(path_, active_generation_);
    const int local_fd = ::open(seg_path.c_str(), O_RDWR | O_CREAT | O_APPEND, kWalFilePermissions);
    if (local_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open wal segment");
    }
    fd_.store(local_fd, std::memory_order_relaxed);
  } else {
    active_generation_ = generations.back();
    uint64_t last_valid_lsn = 0;
    for (const uint64_t generation : generations) {
      const bool is_active = (generation == active_generation_);
      const auto seg_path = derive_wal_segment_path(path_, generation);
      const int seg_fd = ::open(seg_path.c_str(), is_active ? (O_RDWR | O_APPEND) : O_RDONLY, kWalFilePermissions);
      if (seg_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open wal segment");
      }
      try {
        // 0 means "this segment contributed nothing" (lsn 0 never exists) -- must not overwrite
        // a real value already found in an earlier generation, e.g. when the active segment is
        // still empty right after a rollover.
        const uint64_t found = scan_and_validate(seg_fd, is_active);
        if (found > 0) {
          last_valid_lsn = found;
        }
      } catch (...) {
        ::close(seg_fd);
        throw;
      }
      if (is_active) {
        fd_.store(seg_fd, std::memory_order_relaxed);
      } else {
        ::close(seg_fd);
      }
    }
    next_lsn_.store(last_valid_lsn + 1, std::memory_order_relaxed);
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

auto Wal::discover_segments(const std::filesystem::path &wal_path) -> std::vector<uint64_t> {
  std::vector<uint64_t> generations;
  const std::filesystem::path dir = wal_path.has_parent_path() ? wal_path.parent_path() : ".";
  if (!std::filesystem::exists(dir)) {
    return generations;
  }
  const std::string prefix = wal_path.filename().string() + ".";
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    const std::string suffix = name.substr(prefix.size());
    // Pure-digit suffixes only: guards against unrelated files sharing the prefix.
    if (!suffix.empty() && suffix.find_first_not_of("0123456789") == std::string::npos) {
      generations.push_back(std::stoull(suffix));
    }
  }
  std::sort(generations.begin(), generations.end());
  return generations;
}

auto Wal::scan_and_validate(int segment_fd, bool allow_truncate) -> uint64_t {
  struct stat file_stat {};
  if (::fstat(segment_fd, &file_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "fstat wal segment");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);

  uint64_t offset = 0;
  uint64_t last_valid_lsn = 0;

  while (file_size - offset >= sizeof(WalRecordHeader)) {
    const WalRecordHeader header = read_header_at(segment_fd, offset);

    if (header.magic != kWalRecordMagic || header.format_version != kWalFormatVersion) {
      break;  // Corrupt, or a record layout this build doesn't understand: stop here.
    }

    const uint64_t payload_len = static_cast<uint64_t>(header.key_len) + header.value_len;
    if (file_size - offset - sizeof(WalRecordHeader) < payload_len) {
      break;  // Torn payload: stop here.
    }

    const std::vector<std::byte> payload = read_payload_at(segment_fd, offset + sizeof(WalRecordHeader), payload_len);

    const std::span<const std::byte> key_span(payload.data(), header.key_len);
    const std::span<const std::byte> value_span(payload.data() + header.key_len, header.value_len);
    if (compute_checksum(header, key_span, value_span) != header.checksum) {
      break;  // Corrupt: stop here.
    }

    offset += sizeof(WalRecordHeader) + payload_len;
    last_valid_lsn = header.lsn;
  }

  if (offset < file_size) {
    if (!allow_truncate) {
      throw std::runtime_error("wal: corrupt tail found in a retired (non-active) segment");
    }
    if (::ftruncate(segment_fd, static_cast<off_t>(offset)) != 0) {
      throw std::system_error(errno, std::generic_category(), "ftruncate wal");
    }
  }

  return last_valid_lsn;
}

auto Wal::read_header_at(int segment_fd, uint64_t offset) -> WalRecordHeader {
  WalRecordHeader header;
  const ssize_t header_read = ::pread(segment_fd, &header, sizeof(header), static_cast<off_t>(offset));
  if (header_read != static_cast<ssize_t>(sizeof(header))) {
    throw std::system_error(errno, std::generic_category(), "pread wal header");
  }
  return header;
}

auto Wal::read_payload_at(int segment_fd, uint64_t offset, uint64_t payload_len) -> std::vector<std::byte> {
  std::vector<std::byte> payload(payload_len);
  if (payload_len > 0) {
    const ssize_t payload_read = ::pread(segment_fd, payload.data(), payload_len, static_cast<off_t>(offset));
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
  {
    const std::lock_guard<std::mutex> lock(settled_mutex_);
    highest_settled_lsn_ = next_to_flush_ - 1;
  }
  settled_cv_.notify_all();
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
  {
    const std::lock_guard<std::mutex> lock(settled_mutex_);
    highest_settled_lsn_ = round_last_lsn;
  }
  settled_cv_.notify_all();

  for (auto *rec : batch) {
    ring_[rec->lsn % kWalRingCapacity].store(nullptr, std::memory_order_release);
    rec->release();
  }
}

auto Wal::drain_pending() -> uint64_t {
  // Snapshot the target once, not fresh on every iteration: under sustained concurrent writers,
  // next_lsn_ keeps advancing, so re-reading it each pass would chase a moving target and never
  // return. Everything reserved as of this call's start (this snapshot) is what durability up to
  // "now" means; anything reserved after is the next drain_pending() call's job -- via
  // release_leadership()'s own single-retry handling for its callers, not this function chasing
  // it internally. The loop below still repeats -- collect_batch() caps each round at
  // kWalRingCapacity, so more than one round can be needed to reach even a fixed target -- but
  // that's bounded by the backlog size at entry, not by how long writers keep arriving.
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
  bool is_leader = !flushing_.exchange(true, std::memory_order_acq_rel);

  if (!is_leader) {
    // Bounded wait_for(), not a spin: spinning steals cycles from other concurrent work.
    //
    // A short timeout closes a gap a plain (unbounded) wait cannot: this follower's own leader
    // may hand off leadership to a third thread in the gap between `flushing_`'s release and this
    // follower re-checking it (see release_leadership()'s contract), and nothing then guarantees
    // that new leader's own drain covers this follower's specific LSN. Bounding the wait means a
    // follower left stranded this way notices within kFollowerWaitTimeout, at which point --
    // finding no leader active for its still-unsettled LSN -- it elects itself rather than
    // continuing to trust a notify_all() that may never name it.
    constexpr auto kFollowerWaitTimeout = std::chrono::milliseconds(5);
    std::unique_lock<std::mutex> lock(settled_mutex_);
    while (highest_settled_lsn_ < lsn) {
      const bool timed_out = settled_cv_.wait_for(lock, kFollowerWaitTimeout) == std::cv_status::timeout;
      if (timed_out && highest_settled_lsn_ < lsn) {
        lock.unlock();
        if (!flushing_.exchange(true, std::memory_order_acq_rel)) {
          is_leader = true;
          break;
        }
        lock.lock();
      }
    }
  }

  if (!is_leader) {
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
  uint64_t count = 0;

  for (const uint64_t generation : discover_segments(path_)) {
    const auto seg_path = derive_wal_segment_path(path_, generation);
    const int seg_fd = ::open(seg_path.c_str(), O_RDONLY);
    if (seg_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "open wal segment for replay");
    }
    struct FdGuard {
      int local_fd;
      ~FdGuard() {
        if (local_fd >= 0) {
          ::close(local_fd);
        }
      }
    } fd_guard{seg_fd};

    struct stat file_stat {};
    if (::fstat(seg_fd, &file_stat) != 0) {
      throw std::system_error(errno, std::generic_category(), "fstat wal segment replay");
    }
    const auto file_size = static_cast<uint64_t>(file_stat.st_size);

    uint64_t offset = 0;
    while (file_size - offset >= sizeof(WalRecordHeader)) {
      const WalRecordHeader header = read_header_at(seg_fd, offset);
      const uint64_t payload_len = static_cast<uint64_t>(header.key_len) + header.value_len;
      const std::vector<std::byte> payload = read_payload_at(seg_fd, offset + sizeof(WalRecordHeader), payload_len);

      const std::span<const std::byte> key_span(payload.data(), header.key_len);
      const std::span<const std::byte> value_span(payload.data() + header.key_len, header.value_len);

      callback(static_cast<WalRecordType>(header.type), key_span, value_span, header.lsn);
      ++count;

      offset += sizeof(WalRecordHeader) + payload_len;
    }
  }

  return count;
}

// Retention argument for why deleting generation (new_generation - 2) is always safe: checkpoint
// cycles run single-flight (reorg_running_), so this call retires exactly the generation active
// during the *previous* cycle. That generation holds two kinds of record: what was already
// covered by the previous cycle's own checkpoint_lsn, plus whatever landed there from writers
// racing that cycle's own execution (reserved after checkpoint_lsn but before this rollover) --
// records this rotate_segment() doesn't know about individually. But *this* cycle's checkpoint_lsn
// (captured before this call, in the current VMemKVImpl::checkpoint_internal()) is itself >=
// everything reserved as of the previous rollover, precisely because that rollover already
// happened before this cycle could start (again, single-flight) -- so by the time *this* call
// runs, the previous generation is fully covered by the checkpoint about to commit. One
// generation of slack (never deleting the immediately-previous one, only the one before that) is
// exactly what covers that in-between spillover, with no per-record bookkeeping at all.
void Wal::rotate_segment() {
  if (poisoned_.load(std::memory_order_acquire)) {
    throw std::runtime_error("WAL is poisoned, refusing to rotate");
  }

  // Become leader ourselves rather than "wait until flushing_ is false" -- the latter has a TOCTOU
  // where another thread could grab leadership between this call waking and acting on fd_. No
  // drain_pending() first: a record that reserved its lsn just before this swap may still end up
  // physically written to either the old or the new segment depending
  // on exactly when its own group-commit round runs (write_and_fsync_batch() reads fd_ fresh) --
  // both are correct, since replay() reads every segment currently on disk regardless of which one
  // a given record landed in (see this file's own comment above and low_level_design.md 5.5).
  // Leadership here exists only so no write_and_fsync_batch() round is ever mid-flight against the
  // fd being closed below.
  //
  // Timed (see last_rotate_leader_wait_us()'s own comment): under sustained concurrent writers,
  // this wait -- not open()/close()/the generation-2-back unlink() below, all consistently
  // sub-millisecond -- is what dominates rotate_segment()'s, and therefore checkpoint_internal()'s,
  // wall-clock cost.
  const auto leader_wait_start = std::chrono::steady_clock::now();
  while (flushing_.exchange(true, std::memory_order_acq_rel)) {
    flushing_.wait(true, std::memory_order_acquire);
  }
  last_rotate_leader_wait_us_.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                              std::chrono::steady_clock::now() - leader_wait_start)
                                                              .count()),
                                    std::memory_order_relaxed);

  const int old_fd = fd_.load(std::memory_order_acquire);
  const uint64_t new_generation = active_generation_ + 1;
  const auto new_path = derive_wal_segment_path(path_, new_generation);
  const int new_fd = ::open(new_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_APPEND, kWalFilePermissions);
  if (new_fd < 0) {
    const auto err = std::make_exception_ptr(std::system_error(errno, std::generic_category(), "open new wal segment"));
    fail_all_pending_and_release_leadership(err);
    std::rethrow_exception(err);
  }

  fd_.store(new_fd, std::memory_order_release);
  active_generation_ = new_generation;
  flushing_.store(false, std::memory_order_release);
  flushing_.notify_all();

  // See size_bytes()'s own comment: this lock is what actually prevents fstat(old_fd) there from
  // racing this close(old_fd) -- fd_ already pointing at new_fd by this point doesn't, since a
  // concurrent size_bytes() call may have snapshotted old_fd before the store above.
  {
    std::lock_guard<std::mutex> lock(fd_close_mu_);
    ::close(old_fd);
  }

  // Safe even if generation (new_generation - 2) doesn't exist (first two cycles) or was already
  // deleted by an interrupted prior cycle -- see this function's own doc comment.
  if (new_generation >= 2) {
    std::error_code ignored;
    std::filesystem::remove(derive_wal_segment_path(path_, new_generation - 2), ignored);
  }
}

auto Wal::next_lsn() const noexcept -> uint64_t { return next_lsn_.load(std::memory_order_relaxed); }

auto Wal::size_bytes() const -> uint64_t {
  // fd_close_mu_ pairs with rotate_segment()'s own lock around close(old_fd): without it, this
  // fstat() can race the close() of the exact fd it just loaded (fd_ being updated to new_fd
  // first doesn't help -- the snapshot here can still be the outgoing one). That's a real TOCTOU,
  // not just a lint: fstat-after-close throws EBADF, or, in the rarer case another open()
  // elsewhere already reused the fd number, silently reports an unrelated file's size. Contention
  // is negligible -- rotate_segment() takes this lock only around the close() call itself, and
  // size_bytes() is already sampled every kWalCheckStride writes, not per-write.
  std::lock_guard<std::mutex> lock(fd_close_mu_);
  const int fd = fd_.load(std::memory_order_acquire);
  struct stat file_stat {};
  if (::fstat(fd, &file_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "fstat wal (size_bytes)");
  }
  return static_cast<uint64_t>(file_stat.st_size);
}

}  // namespace vmemkv
