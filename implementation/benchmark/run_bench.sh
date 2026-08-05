#!/usr/bin/env bash
# run_bench.sh - Local VMemKV benchmark runner.
#
# Shares its scenario filters, env-var construction, and per-scenario invocation
# (common/run_scenario.sh) with run_bench_aws_c6id.sh, so a local run and an AWS
# run differ only in how run_scenario.sh gets launched (in-process here vs. over
# SSH, optionally inside a cgroup, there) -- not in what gets measured.

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
ENABLE_LMDB=ON
BUILD_TYPE=Release
BUILD_DIR=build
OUTPUT_FILE=""
AUTO_LOG=true
MIN_TIME="5.0s"
QUICK=false
LARGE_VALUE_FIRST=false
SCENARIO_LIMIT="in_memory"
ORDER="A_FIRST"
LTM_MEMORY_BUDGET_BYTES="$(vmemkv_matrix::ltm_memory_budget_bytes)"
USE_CGROUP=false

# ── Argument Parsing ──────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-rocksdb)   ENABLE_ROCKSDB=OFF; shift ;;
    --no-lmdb)      ENABLE_LMDB=OFF;    shift ;;
    --build-type)   BUILD_TYPE="$2";    shift 2 ;;
    --build-dir)    BUILD_DIR="$2";     shift 2 ;;
    --min-time)     MIN_TIME="$2";      shift 2 ;;
    --quick)        MIN_TIME="0.1s"; QUICK=true; shift ;;
    --output)       OUTPUT_FILE="$2";   shift 2 ;;
    --no-log)       AUTO_LOG=false;     shift ;;
    --scenario)     SCENARIO_LIMIT="$2"; shift 2 ;;
    --ltm-first)    ORDER="B_FIRST";    shift ;;
    --ltm-memory-budget-bytes) LTM_MEMORY_BUDGET_BYTES="$2"; shift 2 ;;
    --cgroup)       USE_CGROUP=true;    shift ;;
    --large-value-first|--large_value-first)
      LARGE_VALUE_FIRST=true
      shift
      ;;
    -h|--help)
      echo "Usage: run_bench.sh [options]"
      echo "  --scenario <S>    in_memory | ltm | all (default: in_memory)"
      echo "  --ltm-first       With --scenario all, run LTM before in-memory"
      echo "  --no-rocksdb      Disable RocksDB build"
      echo "  --no-lmdb         Disable LMDB build"
      echo "  --build-type <T>  CMake build type (default: Release)"
      echo "  --build-dir <D>   Build directory (default: build)"
      echo "  --min-time <S>    Minimum measurement time (default: 5.0s)"
      echo "  --quick           Quick mode (runs one smoke-test benchmark)"
      echo "  --large-value-first  Run larger-value benchmarks before smaller-value benchmarks"
      echo "  --ltm-memory-budget-bytes <N>  Declared LTM memory budget fed to bench_kv"
      echo "                    (default: same 1GiB the AWS runner uses; combined with"
      echo "                    the fixed LTM target_ratio this sizes the LTM corpus the"
      echo "                    same way regardless of this machine's real RAM)"
      echo "  --cgroup          Best-effort: wrap the LTM scenario in 'systemd-run --user'"
      echo "                    with the same MemoryHigh/MemoryMax as the AWS runner, to"
      echo "                    also reproduce real memory pressure (not just corpus"
      echo "                    sizing). Requires a systemd user session with working"
      echo "                    cgroup delegation -- silently has no effect otherwise, so"
      echo "                    don't rely on it to prove swap behavior without checking."
      echo "  --output <F>      Save raw JSON output to file"
      echo "  --no-log          Disable automatic logging"
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

case "$SCENARIO_LIMIT" in
  in_memory|ltm|all) ;;
  *) echo "Invalid --scenario: $SCENARIO_LIMIT (expected in_memory|ltm|all)" >&2; exit 1 ;;
esac

cd "$REPO_ROOT"

if [[ -t 1 ]]; then
  export VMEMKV_COLOR=1
fi

if [[ "$LARGE_VALUE_FIRST" == "true" ]]; then
  export VMEMKV_BENCH_LARGE_VALUE_FIRST=1
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

# ── LMDB Check ───────────────────────────────────────────────────────────
if [[ "$ENABLE_LMDB" == "ON" ]]; then
  if ! (pkg-config --exists lmdb 2>/dev/null || \
        [[ -f /opt/homebrew/lib/liblmdb.dylib ]] || \
        [[ -f /usr/local/lib/liblmdb.so ]] || \
        [[ -f /usr/lib/x86_64-linux-gnu/liblmdb.so ]] || \
        [[ -f /usr/lib/liblmdb.so ]]); then
    ENABLE_LMDB=OFF
  fi
fi

# ── CMake Configure & Build ──────────────────────────────────────────────
echo "Building benchmark binary..."
vmemkv_build_bench "$IMPL_ROOT" "$BUILD_DIR" "$BUILD_TYPE" "$ENABLE_ROCKSDB" -DENABLE_LMDB="$ENABLE_LMDB" >/dev/null
echo "Build completed."

BENCH_BIN="$REPO_ROOT/$BUILD_DIR/benchmark/bench_kv"

# ── Auto log file setup ──────────────────────────────────────────────────
LOG_DIR="$SCRIPT_DIR/logs"
if [[ "$AUTO_LOG" == "true" ]]; then
  TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
  ROCKSDB_SUFFIX="$([ "$ENABLE_ROCKSDB" == "ON" ] && echo "rocksdb" || echo "no_rocksdb")"
  mkdir -p "$LOG_DIR"
fi

read -r -a SCENARIO_ORDER_KEYS <<<"$(vmemkv_matrix::scenario_order_keys_from_flag "$ORDER")"

run_local_scenario() {
  local scenario_key="$1"
  local scenario_env_prefix
  local scenario_runtime_env=""
  local filter
  local temp_json
  local progress_state
  local auto_log_file=""

  scenario_env_prefix="$(vmemkv_matrix::scenario_env_prefix "$scenario_key")"

  if [[ "$scenario_key" == "in_memory" ]]; then
    # Same reasoning as the AWS runner: guarantee the in-memory corpus is sized
    # off real host RAM regardless of any ambient cgroup the shell happens to
    # be running under.
    scenario_runtime_env="VMEMKV_BENCH_FORCE_HOST_MEMORY=1"
  fi

  local memory_budget_source=""
  local memory_budget_bytes=""
  local memo=""
  if [[ "$scenario_key" == "ltm" ]]; then
    memory_budget_source="declared"
    memory_budget_bytes="$LTM_MEMORY_BUDGET_BYTES"
    memo="Local LTM run uses the same declared memory budget as the AWS runner ($LTM_MEMORY_BUDGET_BYTES bytes) to size the corpus consistently; real memory pressure additionally requires --cgroup with working cgroup delegation."
    scenario_runtime_env="VMEMKV_CONTEXT_memory_budget_bytes=$memory_budget_bytes"
  fi

  if [[ "$QUICK" == "true" ]]; then
    filter="$(vmemkv_matrix::scenario_quick_filter "$scenario_key")"
  else
    filter="$(vmemkv_matrix::scenario_effective_filter "$scenario_key" "$LARGE_VALUE_FIRST" false)"
  fi

  local scenario_order_label value_order_keys value_order_label target_profile
  scenario_order_label="$(vmemkv_scenario_order_label "$scenario_key")"
  read -r -a value_order_keys <<<"$(vmemkv_matrix::scenario_value_order_keys_from_flag "$scenario_key" "$LARGE_VALUE_FIRST")"
  value_order_label="$(vmemkv_value_order_label "${value_order_keys[@]}")"
  target_profile="$(vmemkv_target_profile_label "$scenario_key")"

  local benchmark_flags="quick=$([[ "$QUICK" == "true" ]] && printf '1' || printf '0');large_value_first=$([[ "$LARGE_VALUE_FIRST" == "true" ]] && printf '1' || printf '0')"
  declare -a context_env=()
  vmemkv_context_env_array context_env \
    "local" \
    "$BUILD_TYPE" \
    "$BUILD_DIR" \
    "$ENABLE_ROCKSDB" \
    "$scenario_order_label" \
    "$benchmark_flags" \
    "$memory_budget_source" \
    "$memory_budget_bytes" \
    "host" \
    "" \
    "$memo"

  echo "=== Scenario: $scenario_key (target_ratio=$target_profile) ==="

  progress_state="$(mktemp /tmp/vmemkv_local_progress.XXXXXX)"
  temp_json="$(mktemp /tmp/vmemkv_bench_results.XXXXXX.json)"
  # shellcheck disable=SC2064
  trap "rm -f '$progress_state' '$temp_json'" RETURN

  local -a bench_stdout_wrapper=()
  if command -v stdbuf >/dev/null 2>&1; then
    bench_stdout_wrapper=(stdbuf -oL -eL)
  fi

  echo "Counting benchmarks..."
  local total_benchmarks
  total_benchmarks="$(env $scenario_env_prefix ${scenario_runtime_env:+$scenario_runtime_env} "$BENCH_BIN" --benchmark_list_tests --benchmark_filter="${filter}" 2>/dev/null | awk 'NF { c++ } END { print c + 0 }')"
  if [[ "$total_benchmarks" -eq 0 ]]; then
    echo "Failed to match any benchmarks against regex: ${filter}" >&2
    return 1
  fi
  echo "Running ${total_benchmarks} benchmark(s)..."
  echo "Filter: ${filter}"
  echo "$total_benchmarks" >"$progress_state"

  local -a scenario_cmd=("$SCRIPT_DIR/common/run_scenario.sh" "$BENCH_BIN" "$filter" "$MIN_TIME" "$temp_json")
  if [[ "$USE_CGROUP" == "true" && "$scenario_key" == "ltm" ]]; then
    if command -v systemd-run >/dev/null 2>&1; then
      # Both the priming pass below and the cgroup-wrapped measurement pass after it run with
      # VMEMKV_BENCH_SKIP_CLEANUP=1 (see its comment in bench_kv.cpp), so bench_kv's own
      # automatic sweeps don't fight a deliberate multi-store, multi-master-generation priming
      # pass. That means *this* script owns the "start from a clean slate" step neither of those
      # invocations will do on their own -- do it once, here, unconditionally, exactly like
      # bench_kv's own startup sweep normally would (same "bench_" prefix, same directory: this
      # script never sets VMEMKV_DB_DIR, so bench_kv defaults to ".", i.e. $REPO_ROOT after the
      # `cd "$REPO_ROOT"` above).
      find "$REPO_ROOT" -maxdepth 1 -name 'bench_*' -exec rm -rf {} +

      # Prime every shared master corpus this run will need at full, unconstrained disk speed
      # *before* entering the cgroup below. Without this, a real per-master populate under this
      # exact cgroup was measured at 200-1200s (vs. ~20-30s unconstrained) -- paying that cost
      # here means the cgroup-wrapped measurement pass only ever needs to clone, not rebuild,
      # those masters. See vmemkv_matrix::ltm_priming_filter()'s comment for what this
      # single-cell-per-(store,value size) filter does and does not cover.
      echo "Priming LTM master corpora (unconstrained) before entering the cgroup..."
      local priming_filter priming_json
      priming_filter="$(vmemkv_matrix::ltm_priming_filter)"
      priming_json="$(mktemp /tmp/vmemkv_ltm_priming.XXXXXX.json)"
      env $scenario_env_prefix ${scenario_runtime_env:+$scenario_runtime_env} VMEMKV_BENCH_SKIP_CLEANUP=1 \
        "$SCRIPT_DIR/common/run_scenario.sh" "$BENCH_BIN" "$priming_filter" "$MIN_TIME" "$priming_json" \
        >/dev/null
      rm -f "$priming_json"
      echo "Priming complete."

      # The measurement pass below reuses the masters priming just built -- it must not let
      # bench_kv's own startup cleanup_stale_benchmark_files() wipe them the instant it starts.
      scenario_runtime_env="${scenario_runtime_env:+$scenario_runtime_env }VMEMKV_BENCH_SKIP_CLEANUP=1"

      local ltm_swap_budget
      ltm_swap_budget="$(vmemkv_matrix::ltm_swap_budget_bytes)"
      scenario_cmd=(systemd-run --user --scope --quiet \
        -p "MemoryHigh=${LTM_MEMORY_BUDGET_BYTES}" \
        -p "MemoryMax=$((LTM_MEMORY_BUDGET_BYTES * 2))" \
        -p "MemorySwapMax=${ltm_swap_budget}" \
        -- "${scenario_cmd[@]}")
      echo "NOTE: --cgroup requested systemd-run --user with MemoryHigh=${LTM_MEMORY_BUDGET_BYTES}," \
           "MemoryMax=$((LTM_MEMORY_BUDGET_BYTES * 2)). Whether this actually constrains RSS" \
           "depends on this host's cgroup delegation (verify with e.g. 'systemctl --user status');" \
           "in sandboxed/nested containers it is commonly a no-op."
    else
      echo "NOTE: --cgroup requested but systemd-run is not available; running without a memory cgroup." >&2
    fi
  fi

  set +e
  env "${context_env[@]}" $scenario_env_prefix ${scenario_runtime_env:+$scenario_runtime_env} "${bench_stdout_wrapper[@]}" \
    "${scenario_cmd[@]}" 2>&1 \
    | awk -v state_file="$progress_state" -f "$SCRIPT_DIR/common/benchmark_progress.awk"
  local bench_status=${PIPESTATUS[0]}
  set -e

  if [[ "$bench_status" -ne 0 ]]; then
    return "$bench_status"
  fi

  if [[ ! -s "$temp_json" ]]; then
    echo "Benchmark produced no JSON output. Check the benchmark filter: ${filter}" >&2
    return 1
  fi

  if [[ "$AUTO_LOG" == "true" ]]; then
    auto_log_file="$LOG_DIR/${TIMESTAMP}_${ROCKSDB_SUFFIX}_${scenario_key}.json"
    cp "$temp_json" "$auto_log_file"
    echo "JSON saved to: $auto_log_file"
  fi

  if [[ -n "$OUTPUT_FILE" ]]; then
    local scenario_output="$OUTPUT_FILE"
    if [[ "$SCENARIO_LIMIT" == "all" ]]; then
      scenario_output="${OUTPUT_FILE%.json}_${scenario_key}.json"
    fi
    mkdir -p "$(dirname "$scenario_output")"
    cp "$temp_json" "$scenario_output"
    echo "JSON saved to: $scenario_output"
  fi
}

if [[ "$SCENARIO_LIMIT" == "all" ]]; then
  for key in "${SCENARIO_ORDER_KEYS[@]}"; do
    run_local_scenario "$key"
  done
else
  run_local_scenario "$SCENARIO_LIMIT"
fi
