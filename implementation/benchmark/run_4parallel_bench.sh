#!/usr/bin/env bash
# run_4parallel_bench.sh - Wrapper to run 4 parallel AWS spot benchmarks.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
  
  # Run the orchestrator script in the background
  "$SCRIPT_DIR/run_bench_aws_c6id.sh" \
    --scenario "$scenario" \
    --value-size "$val_size" \
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
