// hooks.hpp - Checkpoint test seams and the in-place-update barrier.
//
// Every seam on the checkpoint path is a nullary callable fired exactly once. Any
// type satisfying CheckpointHook works, including plain lambdas; the NoOp structs
// below are the production defaults.
#pragma once

#include <concepts>

#include "core/reference_tracker.hpp"
#include "t2_flat_file/t2_flat_file.hpp"

namespace vmemkv {

template <typename Hook>
concept CheckpointHook = std::invocable<Hook &>;

// Fires once inside checkpoint_internal(), right before T2FlatFile::stop_writers_and_wait()
// is called (i.e. while writes are still handed out normally by acquire_write_handle()).
// Lets a test deterministically get a writer's T2MemoryHandle registered *before* the stop
// flag goes up, so the subsequent stop-and-wait has a real, still-in-flight writer to wait
// for -- exercising the exact handshake that closes the residual-window race (see
// T2FlatFile::stop_writers_and_wait()'s declaration). No-op in production.
struct NoOpPreStopHook {
  void operator()() const noexcept {}
};

// Fires once inside checkpoint_internal(), strictly before T1 is ever touched (T1 is only
// published once, later, from a single I/O-free t1_.reorganize() call). Lets a test throw
// here to verify that any failure up to and including this point leaves T1 completely
// untouched -- the T2-ownership watermark is the only state a caught exception needs to roll
// back (see checkpoint_internal()'s own try/catch). No-op in production.
struct NoOpPreFinishHook {
  void operator()() const {}
};

// checkpoint_internal() briefly stops writers (T2FlatFile::stop_writers_and_wait()) while
// finishing a cycle and must resume them on every exit path, including exceptions thrown
// partway through.
struct WriterResumeGuard {
  T2FlatFile *t2;
  ~WriterResumeGuard();
};

// Contract: once wait_until_retired(boundary) returns, no in-place T2 update whose target offset
// is < boundary is still in flight -- it has either fully completed its write or not yet started.
// Registration tracks the actual offset (not a generic epoch counter), so the contract reads
// literally rather than needing translation at each call site. This only drains writes already in
// flight; the caller is responsible for separately ensuring no *new* one can start below
// `boundary` (checkpoint_internal() uses the T2-ownership watermark for that) before relying on
// the result, or a fresh enter() could race back in immediately after this returns.
class InPlaceUpdateBarrier {
 public:
  using Guard = typename vmemkv::ThreadReferenceTracker<uint64_t>::Guard;

  // Registers the calling thread as about to attempt an in-place write targeting `offset`. Hold
  // the returned guard for exactly the duration of that attempt (through its
  // T2FlatFile::update_value_at() call, success or failure) -- release it as soon as the attempt
  // is decided, including on every early-return path that doesn't end up writing.
  [[nodiscard]] auto enter(uint64_t offset) const noexcept -> Guard {
    // +1: the tracker's "not registered" sentinel is T{} == 0, which offset 0 itself would
    // otherwise collide with (see ThreadReferenceTracker::wait_until_epoch()'s T{} check).
    return Guard(offsets_, offset + 1);
  }

  void wait_until_retired(uint64_t boundary) const noexcept { offsets_.wait_until_epoch(boundary + 1); }

 private:
  mutable vmemkv::ThreadReferenceTracker<uint64_t> offsets_;
};

inline WriterResumeGuard::~WriterResumeGuard() { t2->resume_writers(); }

}  // namespace vmemkv
