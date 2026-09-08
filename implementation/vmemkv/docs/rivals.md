# Rival Backend Contracts

Comparison baselines in `benchmark/` and their durability, threading, and value semantics.
All rivals take a bare path (no T2 capacity argument) and expose `CloneFromMasterTag` for
master-build + clone. `generate_report.py` renders unknown variants as `n/a`; the matrix
excludes backend/scenario combinations a backend cannot store (currently LeanStore × 64KB).

## RocksDB / RocksDB-BlobDB

- Writes use `sync=true` (per-write WAL fsync), matching VMemKV's per-write fsync contract.
  The default `sync=false` would understate RocksDB's cost at the durability level this
  project targets.
- Shared tuning lives in `src/rivals/rocksdb_common.hpp` (no compression, 256MiB buffers,
  leveled compaction). BlobDB differs only in tuning plus key-value separation.
- Bulk load uses `disableWAL=true` batches; measurement writes are durable.
- Clones share immutable SST files via `Checkpoint` hardlinks.

## LMDB

- Opened `MDB_WRITEMAP` without `MDB_MAPASYNC`: every commit synchronously msyncs, matching
  the same durability contract. Bulk load toggles `MDB_MAPASYNC` for its own duration only.
- Unlimited MVCC readers; exactly one writer at a time (B+Tree/CoW design), reported as-is.
- No reorganize concept (freelist reclaims internally); `reorganize()` is a no-op.
- Clones are real copies (`mdb_env_copy2` with `MDB_CP_COMPACT`), never hardlinks: LMDB
  writes pages in place.

## LeanStore

Pinned by commit in `CMakeLists.txt` (master = pointer-swizzling buffer-pool engine).
Optional via `ENABLE_LEANSTORE` (`--no-leanstore` locally; AWS installs
`libtbb-dev libaio-dev`). Sandbox builds without root use `LEANSTORE_SYSROOT`.

- Durability: WAL on with group-commit fdatasync (`wal_fsync`). Residual gap: `commitTX`
  enqueues to the group committer and returns without waiting for that group's fsync, so
  commit return precedes physical durability by up to one group interval. No
  synchronous-commit knob exists to close it.
- Isolation: snapshot isolation, one TX per operation, aborts retried inside the adapter.
  Same-key write races resolve as abort + retry (outcome-equivalent to the other backends'
  serialization); cross-key scans see an SI snapshot like RocksDB iterators and LMDB MVCC.
- Keys/values are raw byte spans through `BTreeVI` (variable-length, memcmp-ordered).
- Value ceiling: `BTreeVI` lengths are u16, so values above 65535 bytes are rejected. The
  64KB corpus (65536-byte values) is excluded by `benchmark_matrix.sh`.
- Update is same-size in-place only; size-changing updates fail fast. Benchmark updates are
  index-derived same-size, so this never triggers there.
- Insert-after-remove has no engine path (upstream TODO): reinserting a removed key aborts.
  Benchmark flows never do this; three unit cases covering it (re-insert after remove,
  long-prefix CRUD, large-value grow/shrink) skip this backend with a message.
- Point reads pair a scan liveness probe with a lookup for the value. Neither primitive
  alone is correct on this version: point lookup ignores tombstones (deleted keys read as
  live, same- and cross-worker), while the scan fast path overstates chained value lengths
  by the tuple header size. Range scans collect keys first, then resolve values, in one TX.
- Master/clone mirrors LMDB (build once with WAL off, persist-flush, file copy), reopened
  through WAL recovery from the copied state file.
- Memory posture: 1GiB DRAM under `VMEMKV_BENCH_LTM`, 16GiB otherwise
  (`LEANSTORE_DRAM_GIB` overrides; `LEANSTORE_WORKER_THREADS` and `LEANSTORE_PP_THREADS`
  exist for experiments).
- Teardown is timing-sensitive upstream (worker startup barrier vs destroy spin): instances
  run one round-trip job per worker at open as a startup barrier. If teardown hangs
  reappear under load, suspect that race first.
