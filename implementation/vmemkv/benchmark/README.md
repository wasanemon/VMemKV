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
- `common/run_scenario.sh`
  - Host-agnostic wrapper that runs a single Google Benchmark scenario.
  - Used identically by `run_bench.sh` (in-process) and `run_bench_aws_c6id.sh` (over SSH).
- `common/reorg_probe_common.sh`
  - Shared driver logic for `bench_kv`'s standalone `--reorg-probe` CLI mode.
  - Used by `run_background_jobs_probe.sh` and `run_organic_split_probe.sh`.
- `run_bench.sh`
  - Local-only runner.
  - Builds locally, executes locally, and saves raw Google Benchmark JSON.
  - `--quick` selects one smoke-test benchmark, and `--large-value-first` flips the value order within the active scenario.
- `run_bench_aws_c6id.sh`
  - AWS orchestration runner.
  - Provisions EC2, prepares storage, builds remotely, runs the selected cases, and collects results.
  - `--ltm-first` flips execution order, `--large-value-first` flips the value order within the active scenario, and `--quick` runs one workload per scenario.
- `run_5parallel_bench.sh`
  - Wrapper running the 4 CRUD-matrix combos plus a 5th instance for the two background-maintenance probes below, each on its own AWS spot instance.
- `run_background_jobs_probe.sh`
  - Fixed-corpus reference measurement for `reorganize()`/`checkpoint()`'s own duration and the QPS degradation they cause to concurrent Insert/Update/Scan workloads.
- `run_organic_split_probe.sh`
  - Insert-QPS impact of `ShardedT1Index`'s own automatic per-shard background splitting under sustained write load.
- `generate_report.py`
  - Generates a `benchmark_results/pages/<id>_charts.html` report from a `benchmark_results/<id>/` directory.
- `merge_vmemkv_only_results.py`
  - Merges a `--without-rivals` run's VMemKV-only results back into a prior full-matrix run's rival numbers.
- `aws/aws_clean.sh`
  - Cleanup helper for temporary AWS resources.

## Data Flow

1. `bench_kv` emits Google Benchmark JSON.
2. The runner saves that JSON to a file.
3. The AWS runner also streams progress while benchmarks are running.

Benchmark rows are encoded as flat `key=value` segments so the raw Google Benchmark JSON stays unchanged while reporting and plotting can split fields deterministically.

The current benchmark matrix uses `8B`/`1KB` for in-memory runs and `1KB`/`64KB` for LTM runs.
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
