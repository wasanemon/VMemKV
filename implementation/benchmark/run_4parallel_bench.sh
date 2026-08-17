#!/usr/bin/env bash
# run_4parallel_bench.sh - Wrapper to run 4 parallel AWS spot benchmarks.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --without-rivals: pass straight through to each of the 4 instances (see
# run_bench_aws_c6id.sh --help). Use this for a regression-check run after a VMemKV-internal-only
# change, where RocksDB/RocksDB-BlobDB/LMDB's numbers are unaffected and re-measuring them is pure
# wasted AWS time/cost -- reuse the most recent full-bench run's rival-only numbers instead (see
# merge_vmemkv_only_results.py, referenced from vmemkv_matrix::scenario_filter()'s comment).
# Downloaded results are tagged with a _vmemkv_only suffix so they never collide with a full run's.
WITHOUT_RIVALS_FLAG=""
if [[ "${1:-}" == "--without-rivals" ]]; then
  WITHOUT_RIVALS_FLAG="--without-rivals"
  echo "[without-rivals] Running VMemKV variants only -- reuse the last full-bench run's rival numbers."
fi

echo "========================================================"
echo " Starting 4-Parallel AWS Spot Benchmarks for VMemKV     "
echo "========================================================"

# Define the 4 parallel tasks
# Format: "scenario value_size log_prefix"
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
  
  # Run the orchestrator script in the background. --reorg-scaling-probe and --churn-scaling-probe
  # are both additive: they run after this instance's normal (scenario, value_size) matrix, on the
  # same already-provisioned instance -- no 5th instance needed. --reorg-scaling-probe is scoped
  # to this instance's own combo (these 4 tasks already partition the 4 combos it sweeps).
  # --churn-scaling-probe only actually does anything on the two value_size=1KB tasks (see its own
  # comment in run_bench_aws_c6id.sh for why it's fixed to 1KB regardless of this instance's
  # value_size) -- passing it unconditionally to all 4 is harmless, it just no-ops on the 8B/64KB
  # tasks.
  extra_flags=(--reorg-scaling-probe --churn-scaling-probe)
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

echo "--------------------------------------------------------"
echo "All 4 Spot Instances requested. Waiting for execution..."
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
  echo " All 4 Parallel Benchmarks Finished SUCCESSFULLY!       "
  echo " Results downloaded to implementation/benchmark/logs/   "
  echo "========================================================"
else
  echo "========================================================"
  echo " Parallel Benchmarks Finished with $failed Errors.       "
  echo "========================================================"
  exit 1
fi
