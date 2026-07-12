#!/usr/bin/env bash
# run_bench.sh - Local VMemKV benchmark runner.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMPL_ROOT="$REPO_ROOT/implementation"

# Shared matrix and helpers.
# shellcheck source=common/benchmark_matrix.sh
source "$SCRIPT_DIR/common/benchmark_matrix.sh"
# shellcheck source=common/benchmark_common.sh
source "$SCRIPT_DIR/common/benchmark_common.sh"

# ── Defaults ────────────────────────────────────────────────────────────
ENABLE_ROCKSDB=ON
BUILD_TYPE=Release
BUILD_DIR=build
OUTPUT_FILE=""
AUTO_LOG=true
MIN_TIME="5.0s"
QUICK=false
LARGE_VALUE_FIRST=false

# ── Argument Parsing ──────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-rocksdb)   ENABLE_ROCKSDB=OFF; shift ;;
    --build-type)   BUILD_TYPE="$2";    shift 2 ;;
    --build-dir)    BUILD_DIR="$2";     shift 2 ;;
    --min-time)     MIN_TIME="$2";      shift 2 ;;
    --quick)        MIN_TIME="0.1s"; QUICK=true; shift ;;
    --output)       OUTPUT_FILE="$2";   shift 2 ;;
    --no-log)       AUTO_LOG=false;     shift ;;
    --large-value-first|--large_value-first)
      LARGE_VALUE_FIRST=true
      shift
      ;;
    -h|--help)
      echo "Usage: run_bench.sh [options]"
      echo "  --no-rocksdb      Disable RocksDB build"
      echo "  --build-type <T>  CMake build type (default: Release)"
      echo "  --build-dir <D>   Build directory (default: build)"
      echo "  --min-time <S>    Minimum measurement time (default: 5.0s)"
      echo "  --quick           Quick mode (runs one smoke-test benchmark)"
      echo "  --large-value-first  Run larger-value benchmarks before smaller-value benchmarks"
      echo "  --output <F>      Save raw JSON output to file"
      echo "  --no-log          Disable automatic logging"
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

cd "$REPO_ROOT"

if [[ -t 1 ]]; then
  export VMEMKV_COLOR=1
fi

if [[ "$LARGE_VALUE_FIRST" == "true" ]]; then
  export VMEMKV_BENCH_LARGE_VALUE_FIRST=1
fi

# ── Auto log file setup ──────────────────────────────────────────────────
LOG_DIR="$SCRIPT_DIR/logs"
if [[ "$AUTO_LOG" == "true" ]]; then
  TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
  ROCKSDB_SUFFIX="$([ "$ENABLE_ROCKSDB" == "ON" ] && echo "rocksdb" || echo "no_rocksdb")"
  AUTO_LOG_FILE="$LOG_DIR/${TIMESTAMP}_${ROCKSDB_SUFFIX}.json"
  mkdir -p "$LOG_DIR"
fi

# ── RocksDB Check ────────────────────────────────────────────────────────
if [[ "$ENABLE_ROCKSDB" == "ON" ]]; then
  if ! (pkg-config --exists rocksdb 2>/dev/null || \
        [[ -f /opt/homebrew/lib/librocksdb.dylib ]] || \
        [[ -f /usr/local/lib/librocksdb.so ]] || \
        [[ -f /usr/lib/librocksdb.so ]]); then
    ENABLE_ROCKSDB=OFF
  fi
fi

# ── CMake Configure & Build ──────────────────────────────────────────────
echo "Building benchmark binary..."
vmemkv_build_bench "$IMPL_ROOT" "$BUILD_DIR" "$BUILD_TYPE" "$ENABLE_ROCKSDB" >/dev/null
echo "Build completed."

# ── Run ──────────────────────────────────────────────────────────────────
BENCH_BIN="$BUILD_DIR/benchmark/bench_kv"
TEMP_JSON="/tmp/vmemkv_bench_results.json"
rm -f "$TEMP_JSON"

PROGRESS_STATE="$(mktemp /tmp/vmemkv_local_progress.XXXXXX)"
TEMP_JSON="$(mktemp /tmp/vmemkv_bench_results.XXXXXX.json)"
trap 'rm -f "$PROGRESS_STATE" "$TEMP_JSON"' EXIT

BENCH_STDOUT_WRAPPER=()
if command -v stdbuf >/dev/null 2>&1; then
  BENCH_STDOUT_WRAPPER=(stdbuf -oL -eL)
fi

LOCAL_SCENARIO_KEY="in_memory"
if [[ "${VMEMKV_BENCH_LTM:-0}" == "1" ]]; then
  LOCAL_SCENARIO_KEY="ltm"
fi

read -r -a LOCAL_VALUE_ORDER_KEYS <<<"$(vmemkv_matrix::scenario_value_order_keys_from_flag "$LOCAL_SCENARIO_KEY" "$LARGE_VALUE_FIRST")"

LOCAL_SCENARIO_ORDER_LABEL="$(vmemkv_scenario_order_label "$LOCAL_SCENARIO_KEY")"
LOCAL_VALUE_ORDER_LABEL="$(vmemkv_value_order_label "${LOCAL_VALUE_ORDER_KEYS[@]}")"
LOCAL_TARGET_PROFILE="$(vmemkv_target_profile_label "$LOCAL_SCENARIO_KEY")"
LOCAL_MEMO="$(vmemkv_benchmark_memo "local" "$LOCAL_SCENARIO_KEY")"

LOCAL_BENCHMARK_FLAGS="quick=$([[ "$QUICK" == "true" ]] && printf '1' || printf '0');large_value_first=$([[ "$LARGE_VALUE_FIRST" == "true" ]] && printf '1' || printf '0')"
declare -a LOCAL_CONTEXT_ENV=()
vmemkv_context_env_array LOCAL_CONTEXT_ENV \
  "local" \
  "$BUILD_TYPE" \
  "$BUILD_DIR" \
  "$ENABLE_ROCKSDB" \
  "$LOCAL_SCENARIO_ORDER_LABEL" \
  "$LOCAL_VALUE_ORDER_LABEL" \
  "$LOCAL_BENCHMARK_FLAGS" \
  "$LOCAL_TARGET_PROFILE" \
  "host" \
  "" \
  "" \
  "" \
  "$LOCAL_MEMO"

FILTER=".*"
if [[ "$QUICK" == "true" ]]; then
  FILTER="$(vmemkv_matrix::local_quick_filter)"
else
  FILTER="$(vmemkv_matrix::scenario_effective_filter "$LOCAL_SCENARIO_KEY" "$LARGE_VALUE_FIRST" false)"
fi

echo "Counting benchmarks..."
TOTAL_BENCHMARKS="$("$BENCH_BIN" --benchmark_list_tests --benchmark_filter="${FILTER}" 2>/dev/null | awk 'NF { c++ } END { print c + 0 }')"
if [[ "$TOTAL_BENCHMARKS" -eq 0 ]]; then
  echo "Failed to match any benchmarks against regex: ${FILTER}" >&2
  exit 1
fi
echo "Running ${TOTAL_BENCHMARKS} benchmark(s)..."
echo "Quick filter: ${FILTER}"

echo "$TOTAL_BENCHMARKS" >"$PROGRESS_STATE"

set +e
env "${LOCAL_CONTEXT_ENV[@]}" "${BENCH_STDOUT_WRAPPER[@]}" "$BENCH_BIN" \
  --benchmark_min_time="${MIN_TIME}" \
  --benchmark_filter="${FILTER}" \
  --benchmark_out="${TEMP_JSON}" \
  --benchmark_out_format=json 2>&1 \
  | awk -v state_file="$PROGRESS_STATE" -f "$SCRIPT_DIR/common/benchmark_progress.awk"
BENCH_STATUS=${PIPESTATUS[0]}
set -e

if [[ "$BENCH_STATUS" -ne 0 ]]; then
  exit "$BENCH_STATUS"
fi

if [[ ! -s "$TEMP_JSON" ]]; then
  echo "Benchmark produced no JSON output. Check the benchmark filter: ${FILTER}" >&2
  exit 1
fi

# Save logs if requested
if [[ "$AUTO_LOG" == "true" ]]; then
  cp "$TEMP_JSON" "$AUTO_LOG_FILE"
  echo "JSON saved to: $AUTO_LOG_FILE"
fi

if [[ -n "$OUTPUT_FILE" ]]; then
  mkdir -p "$(dirname "$OUTPUT_FILE")"
  cp "$TEMP_JSON" "$OUTPUT_FILE"
  echo "JSON saved to: $OUTPUT_FILE"
fi
