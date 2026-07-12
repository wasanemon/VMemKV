#!/usr/bin/env bash
# aws_remote_runner.sh - Wrapper script executed on AWS instance to run Google Benchmark.
set -euo pipefail

# Argument Parsing
SCENARIO_RUN_FILTER="$1"
MIN_TIME="$2"
SCENARIO_RESULT_PATH="$3"

# Resolve AWS host hardware and OS metadata dynamically on the instance
export VMEMKV_CONTEXT_kernel_release="$(uname -r)"
export VMEMKV_CONTEXT_cpu_model="$(cat /proc/cpuinfo | grep 'model name' | head -n 1 | cut -d: -f2 | xargs)"
export VMEMKV_CONTEXT_cpu_count="$(nproc --all)"
export VMEMKV_CONTEXT_mem_total_bytes="$(cat /proc/meminfo | grep MemTotal | awk '{print $2 * 1024}')"
export VMEMKV_CONTEXT_swap_total_bytes="$(cat /proc/meminfo | grep SwapTotal | awk '{print $2 * 1024}')"

# Run bench_kv with unbuffered stdout to prevent lag in systemd-run logs
exec /home/ubuntu/faultkv/implementation/build-rel/benchmark/bench_kv \
  --benchmark_min_time="${MIN_TIME}" \
  --benchmark_filter="${SCENARIO_RUN_FILTER}" \
  --benchmark_out="${SCENARIO_RESULT_PATH}" \
  --benchmark_out_format=json
