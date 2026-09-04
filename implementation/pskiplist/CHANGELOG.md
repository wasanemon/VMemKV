# Changelog

All notable changes to this project are documented in this file. There have been no
tagged releases yet — everything below is unreleased.

## [Unreleased]

### Added

- Lock-free `get()`/`put()`/`remove()`/`scan()` over an offset-based skip list backed by
  a single `mmap`'d, `MAP_SHARED` file (Level 0, the durable authority for correctness)
  plus volatile, DRAM-only upper levels (Level 1+) that accelerate search and are
  rebuilt from Level 0 on every recovery.
- `checkpoint()`: synchronous, crash-consistent durability boundary via epoch-based
  reclamation (EBR) shared between concurrency-safety and crash-safety, `msync()`, and
  an atomically-published manifest.
- `reclaim()`: genuine physical reclamation of removed keys' space, gated independently
  by checkpoint-confirmed durability and EBR quiescence.
- Crash-consistent in-place mutation: an update or removal of an already-checkpointed
  key shadows its prior value so `recover()` can roll back anything not covered by the
  last checkpoint, and a removal's physical unlink is deferred until checkpoint()
  confirms it's durable — a slot is never reused before that.
- A single generic lock-free "mark and help-splice" primitive shared by Level 0 and the
  upper levels, differing only in how each packs its node identity (array offset vs.
  pointer) into a marked word.
- `Value` is a real template parameter (`PSkipList<Key, Value = uint64_t, Compare>`), any
  trivially-copyable type — backed by a per-node seqlock, not a single CAS'd word, so there
  are no reserved/forbidden payload values and no bit width limit.

### Fixed

- `link_upper_levels()` searched every level from that level's own head instead of cascading
  from the level above, degenerating insert into O(N) at level 1 (where nearly every node
  lives) instead of the intended O(log N).

See [`high_level_design.md`](high_level_design.md) for the full design and
[`README.md`](README.md) for usage.
