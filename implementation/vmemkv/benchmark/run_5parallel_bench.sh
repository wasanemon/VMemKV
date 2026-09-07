#!/usr/bin/env bash
# run_5parallel_bench.sh - Wrapper to run 5 parallel AWS spot benchmarks: the 4 CRUD-matrix
# combos plus a 5th dedicated instance for two background-maintenance probes: background-jobs
# (reorganize()/checkpoint()'s own duration + Insert/Update/Scan QPS degradation while a forced,
# whole-store call runs -- see run_background_jobs_probe.sh) and organic-split (the same
# QPS-degradation question, but for ShardedT1Index's own automatic per-shard splitting under
# sustained write load, the maintenance path that actually runs during ordinary operation -- see
# run_organic_split_probe.sh). The 5th task gets its own instance (--skip-matrix, no
# --scenario/--value-size) rather than riding along on one of the other 4: both probes' corpora
# are unrelated to any one (scenario, value_size) combo, and each runs both its own in_memory and
# ltm points there, so neither partitions cleanly onto one of the 4 combo-scoped instances.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --without-rivals: pass straight through to each of the 4 CRUD-matrix instances (see
# run_bench_aws_c6id.sh --help). Use this for a regression-check run after a VMemKV-internal-only
# change, where RocksDB/RocksDB-BlobDB/LMDB's numbers are unaffected and re-measuring them is pure
# wasted AWS time/cost -- reuse the most recent full-bench run's rival-only numbers instead (see
# merge_vmemkv_only_results.py, referenced from vmemkv_matrix::scenario_filter()'s comment).
# Downloaded results are tagged with a _vmemkv_only suffix so they never collide with a full run's.
# Meaningless for the 5th (background-jobs-only, --skip-matrix) instance -- no CRUD matrix runs
# there to have rivals in the first place -- so it's simply not passed to that task.
WITHOUT_RIVALS_FLAG=""
if [[ "${1:-}" == "--without-rivals" ]]; then
  WITHOUT_RIVALS_FLAG="--without-rivals"
  echo "[without-rivals] Running VMemKV variants only -- reuse the last full-bench run's rival numbers."
fi

echo "========================================================"
echo " Starting 5-Parallel AWS Spot Benchmarks for VMemKV     "
echo "========================================================"

# Define the 4 CRUD-matrix tasks. Format: "scenario value_size log_prefix"
declare -a TASKS=(
  "in_memory 8B inmem_8b"
  "in_memory 1KB inmem_1kb"
  "ltm 1KB ltm_1kb"
  "ltm 64KB ltm_64kb"
)

pids=()
declare -A task_logs

for task in "${TASKS[@]}"; do
  read -r scenario val_size log_prefix <<< "$task"
  log_file="/tmp/vmemkv_parallel_${log_prefix}.log"
  task_logs["$log_prefix"]="$log_file"

  echo "Launching Instance for: Scenario=$scenario, ValueSize=$val_size (Log: $log_file)"

  extra_flags=()
  if [[ -n "$WITHOUT_RIVALS_FLAG" ]]; then
    extra_flags+=("$WITHOUT_RIVALS_FLAG")
  fi
  "$SCRIPT_DIR/run_bench_aws_c6id.sh" \
    --scenario "$scenario" \
    --value-size "$val_size" \
    "${extra_flags[@]}" \
    > "$log_file" 2>&1 &
  pids+=($!)

  # Stagger instance launches to prevent AWS OAuth/token 429 Rate Limit Errors
  sleep 12
done

# 5th task: dedicated background-maintenance-probes instance (each probe's own scenario loop
# covers both in_memory and ltm internally -- see run_background_jobs_probe.sh/
# run_organic_split_probe.sh), no CRUD matrix.
echo "Launching Instance for: Background Jobs + Organic Split Probes (Log: /tmp/vmemkv_parallel_bgjobs.log)"
"$SCRIPT_DIR/run_bench_aws_c6id.sh" \
  --skip-matrix \
  --background-jobs-probe \
  --organic-split-probe \
  > "/tmp/vmemkv_parallel_bgjobs.log" 2>&1 &
pids+=($!)
TASKS+=("- - bgjobs")
task_logs["bgjobs"]="/tmp/vmemkv_parallel_bgjobs.log"

echo "--------------------------------------------------------"
echo "All 5 Spot Instances requested. Waiting for execution..."
echo "You can tail the logs in /tmp/vmemkv_parallel_*.log"
echo "--------------------------------------------------------"

# Monitor loop
failed=0
for i in "${!pids[@]}"; do
  pid="${pids[$i]}"
  read -r scenario val_size log_prefix <<< "${TASKS[$i]}"

  set +e
  wait "$pid"
  status=$?
  set -e

  if [[ "$status" -ne 0 ]]; then
    echo "[ERROR] Task Scenario=$scenario ValSize=$val_size failed with exit code $status" >&2
    echo "Check log: ${task_logs[$log_prefix]}" >&2
    failed=$((failed + 1))
  else
    echo "[SUCCESS] Task Scenario=$scenario ValSize=$val_size finished successfully."
  fi
done

if [[ "$failed" -eq 0 ]]; then
  echo "========================================================"
  echo " All 5 Parallel Benchmarks Finished SUCCESSFULLY!       "
  echo " Results downloaded to implementation/vmemkv/benchmark/logs/   "
  echo "========================================================"
else
  echo "========================================================"
  echo " Parallel Benchmarks Finished with $failed Errors.       "
  echo "========================================================"
  exit 1
fi
