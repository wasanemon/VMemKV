#!/usr/bin/env bash
# run_bench.sh — VMemKV Benchmark Execution Script (Google Benchmark version)
#
# Usage:
#   bash benchmark/run_bench.sh [--no-rocksdb] [--build-type Release] [--quick] [--min-time <secs>]
#

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────
ENABLE_ROCKSDB=ON
BUILD_TYPE=Release
BUILD_DIR=build
OUTPUT_FILE=""
AUTO_LOG=true
MIN_TIME="5.0s"
QUICK=false

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
    -h|--help)
      echo "Usage: run_bench.sh [options]"
      echo "  --no-rocksdb      Disable RocksDB build"
      echo "  --build-type <T>  CMake build type (default: Release)"
      echo "  --build-dir <D>   Build directory (default: build)"
      echo "  --min-time <S>    Minimum measurement time (default: 5.0s)"
      echo "  --quick           Quick mode (runs a small subset in ~1 second)"
      echo "  --output <F>      Save output to file"
      echo "  --no-log          Disable automatic logging"
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

# ── Set repository root ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ── Auto log file setup ──────────────────────────────────────────────────
LOG_DIR="$SCRIPT_DIR/logs"
if [[ "$AUTO_LOG" == "true" ]]; then
  TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
  ROCKSDB_SUFFIX="$([ "$ENABLE_ROCKSDB" == "ON" ] && echo "rocksdb" || echo "no_rocksdb")"
  AUTO_LOG_FILE="$LOG_DIR/${TIMESTAMP}_${ROCKSDB_SUFFIX}.md"
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
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DENABLE_ROCKSDB="$ENABLE_ROCKSDB" -DENABLE_BENCHMARK=ON > /dev/null
cmake --build "$BUILD_DIR" --target bench_kv --parallel > /dev/null

# ── Run ──────────────────────────────────────────────────────────────────
BENCH_BIN="$BUILD_DIR/benchmark/bench_kv"
TEMP_JSON="/tmp/vmemkv_bench_results.json"
rm -f "$TEMP_JSON"

# Determine filter
FILTER=".*"
if [[ "$QUICK" == "true" ]]; then
  # Match Baseline/ST/Insert for quick smoke test
  FILTER="Baseline.*/ST/Insert"
fi

# Execute Google Benchmark saving to JSON directly, showing real-time progress on stdout
"$BENCH_BIN" --benchmark_min_time="${MIN_TIME}" --benchmark_filter="${FILTER}" --benchmark_out="${TEMP_JSON}" --benchmark_out_format=json

# Format results to Markdown tables using jq
ST_TABLE=$(echo -e "### Single-Threaded Benchmarks (threads=1)\n| Benchmark | M ops/s | real_time/op (ns) |\n| :--- | :---: | :---: |\n$(jq -r '.benchmarks[] | select(.threads == 1) | [ .name, (if .items_per_second then ((.items_per_second / 10000 | round) / 100) else "N/A" end), (.real_time | round) ] | "| " + (map(tostring) | join(" | ")) + " |"' "$TEMP_JSON")")

MT_TABLE=$(echo -e "### Multi-Threaded Benchmarks (threads > 1)\n| Benchmark | threads | M ops/s | real_time/op (ns) |\n| :--- | :---: | :---: | :---: |\n$(jq -r '.benchmarks[] | select(.threads > 1) | [ .name, .threads, (if .items_per_second then ((.items_per_second / 10000 | round) / 100) else "N/A" end), (.real_time | round) ] | "| " + (map(tostring) | join(" | ")) + " |"' "$TEMP_JSON")")

FORMATTED_RESULTS=$(echo -e "${ST_TABLE}\n\n${MT_TABLE}")

# Output to console
echo ""
echo "=== Benchmark Results Summary (Markdown) ==="
echo "$FORMATTED_RESULTS"

# Save logs if requested
if [[ "$AUTO_LOG" == "true" ]]; then
  echo "$FORMATTED_RESULTS" > "$AUTO_LOG_FILE"
  echo ""
  echo "Log saved to: $AUTO_LOG_FILE"
fi

if [[ -n "$OUTPUT_FILE" ]]; then
  mkdir -p "$(dirname "$OUTPUT_FILE")"
  echo "$FORMATTED_RESULTS" > "$OUTPUT_FILE"
  echo "Results saved to: $OUTPUT_FILE"
fi
