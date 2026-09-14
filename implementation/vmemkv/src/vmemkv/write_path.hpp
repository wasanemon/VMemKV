// write_path.hpp - Write-path helpers for VMemKVImpl: stripe discipline and WAL batching.
//
// Stripe protocol (type-enforced by with_key_stripe() + StripeResult): the key's stripe lock
// is held only for the T1/T2 mutation and the WAL reservation (which fixes LSN order among
// same-key writers). The multi-millisecond await_durable() wait and the reorg check run
// after the lock is released -- holding the stripe through either would needlessly serialize
// other writers on the same stripe, and holding a T2 write handle into
// maybe_reorganize_if_needed() can deadlock against a concurrent reorganize cycle.
#pragma once

#include <cstddef>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "api/utils.hpp"
#include "t1_index/t1_index.hpp"
#include "t2_flat_file/t2_flat_file.hpp"
#include "vmemkv/read_path.hpp"
#include "vmemkv/t2_ownership.hpp"
#include "wal/wal.hpp"

namespace vmemkv {

// What a with_key_stripe() callback returns: whether the mutation applied, the WAL record
// reserved under the stripe (to be awaited after release), and whether the call took the
// append-region path (to be reorg-checked after release). Plain data -- the wrapper below
// acts on it once the lock is gone.
struct StripeResult {
  bool applied = false;
  Wal::PendingRecord *pending = nullptr;
  bool need_reorg_check = false;
};

// Holds the key's stripe lock for exactly the callback's duration, then awaits the reserved
// WAL record and runs the reorg check with the lock released. LockStripe maps a key to its
// stripe mutex; AwaitDurable/CheckReorg are the post-release steps (bound to the caller's
// wal_/reorg state). Returns whether the mutation applied.
template <typename LockStripe, typename AwaitDurable, typename CheckReorg, typename Fn>
auto with_key_stripe(LockStripe &&lock_stripe,
                     AwaitDurable &&await_durable,
                     CheckReorg &&check_reorg,
                     std::span<const std::byte> key,
                     Fn &&fn) -> bool {
  StripeResult result;
  {
    std::lock_guard<std::mutex> key_lock(lock_stripe(key));
    result = fn();
  }
  if (result.pending != nullptr) {
    await_durable(result.pending);
  }
  if (result.need_reorg_check) {
    check_reorg();
  }
  return result.applied;
}

// update_impl()'s three possible outcomes for a non-inline entry: Aborted means update_impl()
// itself must return false immediately (the key vanished, or T2FlatFile::update_value_at()
// failed); FellThrough means the caller must fall back to write_entry_lockfree()
// (append-region path); Applied means the in-place write already happened and `updated`/
// `pending` carry its result.
enum class InPlaceOutcome { Aborted, FellThrough, Applied };
struct InPlaceUpdateResult {
  InPlaceOutcome outcome;
  Wal::PendingRecord *pending = nullptr;
};

// Decides T1 inlining for a (key, value) pair. Restricted to keys <= 16 bytes: T1 only
// stores a 16-byte prefix, so for longer keys the full key must live in T2. Also restricted
// to keys that don't end in a 0x00 byte: an inline entry has no T2 record, so scan_impl()
// must recover the original key length from the zero-padded prefix alone via
// inline_key_len()'s trailing-zero trim, which is only unambiguous when every trailing zero
// byte is padding.
template <typename ConfigT>
auto try_make_inline_payload(std::span<const std::byte> full_key,
                             std::span<const std::byte> value,
                             uint8_t &out_size) noexcept -> std::optional<uint64_t> {
  if constexpr (ConfigT::UseT1InlineValue) {
    if (full_key.size() <= t1_detail::kPrefixBytes && (full_key.empty() || full_key.back() != std::byte{0})) {
      if (!value.empty() && value.size() <= t1_detail::kInlineValueByteCount) {
        uint64_t payload = 0;
        std::memcpy(&payload, value.data(), value.size());
        // A full-width (8-byte) value that happens to be all-1-bits is bit-for-bit identical to
        // T1's STORE_NOT_FOUND sentinel -- inlining it would make every read path treat this
        // live entry as permanently absent. Only possible at full width: shorter values leave
        // payload's upper bytes zeroed, so they can never reach ~0ULL. Falls through to the
        // normal T2-record path instead, whose offset payload cannot collide this way.
        if (payload == vmemkv::STORE_NOT_FOUND) {
          return std::nullopt;
        }
        out_size = static_cast<uint8_t>(value.size());
        return payload;
      }
    }
  }
  return std::nullopt;
}

// Appends a (key, value) pair to T2 and publishes it in T1, retrying across AppendRegionFull.
// Returns true once the T1 publish applies (or throws on unrepresentable records); the WAL
// reservation stays the caller's job, after the mutation applies.
template <typename ConfigT, typename T1, typename MaybeReorg>
auto write_entry_lockfree(T1 &t1,
                          T2FlatFile &t2,
                          T2Ownership<ConfigT> &own,
                          MaybeReorg &&maybe_reorg,
                          std::span<const std::byte> full_key,
                          std::span<const std::byte> value,
                          std::optional<typename T1::LookupResult> prev) -> bool {
  while (true) {
    uint8_t inline_size = 0;
    if (const auto inline_payload = try_make_inline_payload<ConfigT>(full_key, value, inline_size)) {
      // Resolve the previous entry for segment accounting: callers that already looked the
      // key up pass it in; WAL replay and bulk load (both single-threaded, neither holding a
      // stripe lock) fetch it here instead. One extra T1 lookup per append-write -- negligible
      // next to the T2 memcpy and the WAL fsync that always follow on this path.
      typename T1::LookupResult old = prev.has_value() ? *prev : t1.get_with_hash(full_key);
      own.note_replace(old.payload_bits, old.raw_hash, /*new_is_offset=*/false, 0);
      const auto put_result = t1.put(full_key, *inline_payload, true, inline_size);
      if (put_result == T1::PutResult::Applied) {
        return true;
      }
      // AppendRegionFull: fall through and retry from the top.
      maybe_reorg();
      continue;
    }

    // Checked before touching T2 at all (not just asserted post-append): block_count must fit
    // the 16 bits the payload codec reserves for it, or it would silently wrap, corrupting the
    // embedded size hint try_read_base_record() uses for its fast-path reads.
    uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + full_key.size() + value.size());
    uint64_t block_count = aligned_len / detail::kRecordBlockAlignment;
    if (block_count >= 65536) {
      throw std::runtime_error("Record size exceeds 1.04MB limit");
    }

    // acquire_write_handle() (not a plain get_memory()) defers while checkpoint_internal()
    // has new writers stopped for its target-capture window, and -- critically -- `mem` is
    // held alive across both the T2 append below and the T1 publish attempt, not released in
    // between. That pairing is what closes checkpoint_internal()'s residual-window race.
    typename T1::PutResult put_result;
    {
      T2FlatFile::T2MemoryHandle mem = t2.acquire_write_handle();
      uint64_t offset = vmemkv::T2FlatFile::append_default(mem, full_key, value);
      uint64_t encoded_payload = offset | (block_count << detail::kPayloadSizeShift);
      typename T1::LookupResult old = prev.has_value() ? *prev : t1.get_with_hash(full_key);
      own.note_replace(old.payload_bits, old.raw_hash, /*new_is_offset=*/true, encoded_payload);
      put_result = t1.put(full_key, encoded_payload, false, 0);
    }
    // `mem` is released here, strictly before maybe_reorg() below: that call can block this
    // thread waiting for a concurrent reorganize() to finish, and that same reorganize() may
    // in turn be blocked waiting for *this* handle to drain -- holding it any longer would
    // deadlock the two threads against each other.
    if (put_result == T1::PutResult::Applied) {
      return true;
    }
    // AppendRegionFull: the appended T2 record becomes unreachable garbage, reclaimed by a
    // future reorganize() once nothing references it -- retry from the top.
    maybe_reorg();
  }
}

// update_impl()'s in-place-update decision for a non-inline entry: either applies the update
// in place or conclusively decides it must fall through to write_entry_lockfree().
template <typename ConfigT, typename T1>
auto try_in_place_update(T1 &t1,
                         T2FlatFile &t2,
                         T2Ownership<ConfigT> &own,
                         Wal &wal,
                         std::span<const std::byte> full_key,
                         std::span<const std::byte> value) -> InPlaceUpdateResult {
  const auto res = t1.get_with_hash(full_key);
  if (res.payload_bits == vmemkv::STORE_NOT_FOUND) {
    return {InPlaceOutcome::Aborted};
  }
  if (t1_detail::is_inline(res.raw_hash)) {
    return {InPlaceOutcome::FellThrough};  // Inline entry -- fall through to the append path.
  }

  const T2Memory *mem = t2.get_memory();

  // Read key/alloc_len fresh under the seqlock: an unprotected t2_.at() call isn't safe here.
  bool key_matches = false;
  uint32_t alloc_len = 0;
  read_t2_record_seqlock([&]() -> T2RecordView { return t2.at(res.payload_bits & detail::kPayloadOffsetMask, mem); },
                         [&](const T2RecordView &record) -> bool {
                           key_matches = byte_span_equal(record.key, full_key);
                           alloc_len = record.header->alloc_len;
                           return true;
                         });
  const uint64_t offset = res.payload_bits & detail::kPayloadOffsetMask;
  // Registered *before* reading the watermark/base_boundary below, and held through the
  // write: whichever of this registration or checkpoint_internal()'s claim+wait happens
  // first, the other side sees it. If the claim lands first, allow_in_place below already
  // observes the raised watermark and refuses to write. If this registration lands first,
  // checkpoint_internal()'s drain blocks until this guard releases -- so base_boundary can
  // never publish past an offset this write is still touching.
  const auto write_guard = own.enter_in_place(offset);

  // T2's base region is read seqlock-free (see T2Memory::base_boundary), which is only safe
  // if base offsets never change after being written -- so an in-place update targeting the
  // base is redirected out-of-place instead. The watermark extends the same redirect to a
  // range checkpoint_internal() has already claimed *this cycle*, before base_boundary
  // itself has advanced to cover it.
  if (!key_matches) {
    return {InPlaceOutcome::FellThrough};
  }
  if (value.size() > alloc_len) {
    return {InPlaceOutcome::FellThrough};
  }
  const uint64_t base_boundary = mem->base_boundary.load(std::memory_order_acquire);
  if (offset < base_boundary || offset < own.load_watermark()) {
    return {InPlaceOutcome::FellThrough};
  }
  if (!t2.update_value_at(offset, value, mem)) {
    return {InPlaceOutcome::Aborted};
  }
  return {InPlaceOutcome::Applied, wal.reserve_update(full_key, value)};
}

// Batches WAL pendings for group-commit fusion: reserves accumulate, one drain awaits them
// together. Every reserved record is awaited exactly once, including on exception paths --
// the destructor drains leftovers (swallowing await errors there: it only ever runs with
// leftovers while another exception is already in flight, where replacing it would be worse).
class WalGroupAwaiter {
 public:
  explicit WalGroupAwaiter(Wal &wal) noexcept : wal_(wal) {}

  WalGroupAwaiter(const WalGroupAwaiter &) = delete;
  auto operator=(const WalGroupAwaiter &) -> WalGroupAwaiter & = delete;

  ~WalGroupAwaiter() noexcept {
    if (!pendings_.empty()) {
      try {
        drain();
      } catch (...) {
      }
    }
  }

  void add(Wal::PendingRecord *pending) { pendings_.push_back(pending); }

  auto size() const noexcept -> size_t { return pendings_.size(); }

  void drain() {
    for (Wal::PendingRecord *pending : pendings_) {
      wal_.await_durable(pending);
    }
    pendings_.clear();
  }

 private:
  Wal &wal_;
  std::vector<Wal::PendingRecord *> pendings_;
};

}  // namespace vmemkv
