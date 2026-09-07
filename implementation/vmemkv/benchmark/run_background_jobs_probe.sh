#!/usr/bin/env bash
# run_background_jobs_probe.sh - Fixed-corpus (1KB values, 10,000,000 records) reference
# measurement for VMemKV's two background maintenance jobs: reorganize() and checkpoint(). For
# each job, measures one call's own duration plus the QPS degradation it causes to concurrent
# Insert/Update/Scan workloads -- see bench_kv.cpp's run_background_job_probe() for the actual
# measurement (a matched-duration isolated/concurrent phase pair per workload).
#
# Not a sweep across the CRUD matrix's 4 scenario/value-size combos -- one fixed corpus, measured
# once per (job, scenario) pair, 4 data points total (reorganize/in_memory, reorganize/ltm,
# checkpoint/in_memory, checkpoint/ltm). defragment() is not measured here (permanent no-op, see
# TODO.md).
#
# Usage: run_background_jobs_probe.sh <bench_kv_binary> <output_jsonl_path> [db_dir] [scenario_filter]
#   scenario_filter: "in_memory" or "ltm" (default: both). Matches run_bench_aws_c6id.sh's
#   run_remote_probe() calling convention (in_memory unconstrained, ltm cgroup-wrapped by the
#   caller via systemd-run -- this script applies no memory limit itself).
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"

PROBE_LOG_PREFIX="background-jobs-probe"
BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [scenario_filter]}"
OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [scenario_filter]}"
DB_DIR="${3:-/tmp}"
SCENARIO_FILTER="${4:-}"

: > "$OUTPUT_PATH"

# Generous: populating 10,000,000 records plus a full checkpoint()/reorganize() cycle under LTM
# memory pressure can legitimately take longer than the smaller sweeps the other (retired) probes
# ran -- this is a fixed reference point measured once, not many points in a loop, so there's
# no per-point time budget to protect.
OUTER_TIMEOUT_SECONDS=600

SCENARIOS=("in_memory" "ltm")
if [[ -n "$SCENARIO_FILTER" ]]; then
  # Matches run_remote_probe()'s convention of passing "in_memory"/"ltm" or "in_memory:1KB"/
  # "ltm:1KB" (value-size suffix, if a caller ever adds one -- this probe is fixed at 1KB
  # regardless, so only the scenario half of the filter is meaningful here).
  SCENARIOS=("${SCENARIO_FILTER%%:*}")
fi

for scenario in "${SCENARIOS[@]}"; do
  for job in reorganize checkpoint; do
    log "=== ${scenario}/${job} ==="
    run_probe_point "$scenario" "1KB" "background_job_probe" "1.0" "${scenario}/${job}" --job="$job"
  done
done

log "done. Results written to $OUTPUT_PATH"
