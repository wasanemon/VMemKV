# `in_memory`/1KB Corpus Sizing Investigation (2026-08-05)

## Background

`kInMemory1KBCorpusEntries` originally scaled with the host's real RAM (`target_ratio=0.5`), uncapped, like every other non-8B case. On a large-memory instance (e.g. i4i.8xlarge's 256GB) that meant a ~127.9M-key/133GB corpus, whose populate cost alone dominated a 4-parallel run's wall-clock time far more than any other scenario.

Fixed at a small, constant value instead: roughly the same scale as the LTM/1KB scenario's own corpus (~8.26M keys/8.6GB, itself sized from a small fixed budget x ratio, not host RAM). This keeps `in_memory`/1KB directly comparable to `ltm`/1KB (same data, different memory pressure) and fast to populate on real NVMe, at the cost of no longer literally using half the host's RAM. 8B keeps its own separate, smaller fixed cap (`kInMemoryInlineCorpusEntries`) since even this same entry count at 8B's much smaller footprint would be needlessly small.

## Hang investigation

Separately, tried lowering the fixed value to 2,000,000 to mitigate an `in_memory`/1KB hang observed under heavy concurrent Update load (theory: a smaller corpus makes each reorganize cycle cheaper, narrowing whatever window lets it misbehave) — did not help. The retry hung again, on the *simplest* variant (Baseline, no `T1InlineValue`) at only 17% through the run, with a visibly different symptom (every thread spinning in `sched_yield()`, none actually blocked on I/O — no runaway write this time). Reverted; the corpus size was not the cause.

## Current value

`kInMemory1KBCorpusEntries = 8'000'000` (see `bench_kv.cpp`).
