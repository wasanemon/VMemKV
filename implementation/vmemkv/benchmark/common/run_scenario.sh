#!/usr/bin/env bash
# run_scenario.sh - Host-agnostic wrapper that runs a single Google Benchmark scenario.
#
# Used identically by run_bench.sh (invoked directly, in-process) and
# run_bench_aws_c6id.sh (invoked over SSH on the remote instance) so both paths
# exercise the exact same benchmark invocation -- only how this script itself gets
# launched (locally vs. over SSH, optionally inside a systemd-run cgroup) differs.
set -euo pipefail

# Argument Parsing
BENCH_KV_BIN="$1"
SCENARIO_RUN_FILTER="$2"
MIN_TIME="$3"
SCENARIO_RESULT_PATH="$4"

# Resolve host hardware and OS metadata dynamically wherever this runs (local dev
# machine or AWS instance).
export VMEMKV_CONTEXT_kernel_release="$(uname -r)"
export VMEMKV_CONTEXT_cpu_model="$(grep 'model name' /proc/cpuinfo | head -n 1 | cut -d: -f2 | xargs)"
export VMEMKV_CONTEXT_cpu_count="$(nproc --all)"
export VMEMKV_CONTEXT_mem_total_bytes="$(awk '/MemTotal:/ { print $2 * 1024; exit }' /proc/meminfo)"
export VMEMKV_CONTEXT_swap_total_bytes="$(awk '/SwapTotal:/ { print $2 * 1024; exit }' /proc/meminfo)"

# Run bench_kv with unbuffered stdout to prevent lag in systemd-run/SSH pipes.
exec "$BENCH_KV_BIN" \
  --benchmark_min_time="${MIN_TIME}" \
  --benchmark_filter="${SCENARIO_RUN_FILTER}" \
  --benchmark_out="${SCENARIO_RESULT_PATH}" \
  --benchmark_out_format=json
