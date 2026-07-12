# Benchmark Runner Layout

This directory contains the benchmark executable, the local runner, the AWS runner, and shared runner assets.

## Files

- `bench_kv.cpp`
  - Google Benchmark registrations and benchmark bodies.
  - Owns the benchmark families and the store lifecycle behavior.
- `common/benchmark_matrix.sh`
  - Shell source of truth for scenario metadata.
  - Defines filters, environment prefixes, and result file names.
- `common/benchmark_common.sh`
  - Shared shell helpers used by both runners.
  - Keeps build/run plumbing out of the runners.
- `common/benchmark_progress.awk`
  - Shared output prefixer for time and progress counters.
- `common/benchmark_dimensions.json`
  - Static ID/name table for frontend consumers.
  - Keep this in sync with the raw benchmark names emitted by `bench_kv.cpp`.
- `run_bench.sh`
  - Local-only runner.
  - Builds locally, executes locally, and saves raw Google Benchmark JSON.
  - `--quick` selects one smoke-test benchmark, and `--large-value-first` flips the value order within the active scenario.
- `run_bench_aws_c6id.sh`
  - AWS orchestration runner.
  - Provisions EC2, prepares storage, builds remotely, runs the selected cases, and collects results.
  - `--ltm-first` flips execution order, `--large-value-first` flips the value order within the active scenario, and `--quick` runs one workload per scenario.
- `aws/aws_clean.sh`
  - Cleanup helper for temporary AWS resources.

## Data Flow

1. `bench_kv` emits Google Benchmark JSON.
2. The runner saves that JSON to a file.
3. The AWS runner also streams progress while benchmarks are running.

Benchmark rows are encoded as flat `key=value` segments so the raw Google Benchmark JSON stays unchanged while reporting and plotting can split fields deterministically.

Frontend code that wants stable labels without parsing benchmark names should read `common/benchmark_dimensions.json` alongside the raw Google Benchmark JSON. The raw JSON carries the human-readable benchmark name, and the counters expose numeric IDs plus corpus sizes.
The current benchmark matrix uses `8B` for in-memory runs and `1KB`/`64KB` for LTM runs.
`bench_kv` derives the baseline from the effective machine memory limit (`cgroup` when present, otherwise `/proc/meminfo`). The AWS runner places LTM benchmark runs in a 1GiB cgroup so the corpus is scaled from that budget.

## How to Extend

- Add or adjust benchmark families in `bench_kv.cpp`.
- Update filters or execution metadata in `common/benchmark_matrix.sh`.
- Keep environment-specific orchestration in the runners.
- Keep progress prefixes generic so terminal output stays readable.

## Design Notes

- Shell is the source of truth for the execution matrix because the runners already live in shell.
- AWS provisioning stays in the AWS runner because it is environment-specific.
- AWS LTM uses a fixed 1GiB cgroup budget so the logical workload stays the same while the corpus scales from the effective memory limit.
