#!/usr/bin/env bash
# reorg_probe_common.sh - Shared driver logic for bench_kv's standalone `--reorg-probe` CLI mode,
# used by run_background_jobs_probe.sh and run_organic_split_probe.sh. See each script's header
# comment for its own experiment; this file holds the per-data-point execution/timeout-handling
# and driver-setup boilerplate they share.

# Shared driver-script setup for the three scripts above: parses the common <bench_kv_bin>
# <output_jsonl_path> [db_dir] [combo_filter] positional-arg convention, builds $COMBOS from
# $COMBO_FILTER, truncates $OUTPUT_PATH, and sets $PROBE_LOG_PREFIX (consumed by log() below).
# Sets (global, not local -- callers need these after this function returns): BENCH_KV_BIN,
# OUTPUT_PATH, DB_DIR, COMBO_FILTER, ALL_COMBOS, COMBOS, PROBE_LOG_PREFIX.
#
# Must be called *after* sourcing this file -- SCRIPT_DIR resolution and the `source` line itself
# can't live inside this function, since a script needs SCRIPT_DIR to find this file before it can
# call anything defined in it. Each driver script therefore still resolves its own SCRIPT_DIR and
# sources this file directly; only the setup after that point is shared.
#
# Args: $1 = log prefix (e.g. "reorg-scaling-probe"), $2.. = the calling script's own $1..$4
# (bench_kv_bin, output_jsonl_path, [db_dir], [combo_filter]).
init_probe_driver() {
  PROBE_LOG_PREFIX="$1"
  shift
  BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
  OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
  DB_DIR="${3:-/tmp}"
  # Optional 4th arg: restrict which combos run. Two forms:
  #   "in_memory" / "ltm"          -- every combo for that scenario (both its value sizes)
  #   "in_memory:8B" / "ltm:64KB"  -- exactly one (scenario, value_size) combo
  # The scenario-only form exists so a caller that must wrap only the ltm half in a memory cgroup
  # (see run_bench_aws_c6id.sh's run_scenario(), which does exactly this for the normal benchmark
  # matrix) can invoke a driver script twice -- once unconstrained for in_memory, once
  # systemd-run-wrapped for ltm. The exact-combo form exists so a caller that's already scoped to
  # one (scenario, value_size) pair -- e.g. one of run_5parallel_bench.sh's 4 CRUD-matrix instances, which each
  # own exactly one such pair -- can run only its own combo instead of duplicating its sibling's.
  # Empty (the default) means "all 4 combos", for local testing.
  COMBO_FILTER="${4:-}"

  ALL_COMBOS=("in_memory:8B" "in_memory:1KB" "ltm:1KB" "ltm:64KB")
  COMBOS=()
  for combo in "${ALL_COMBOS[@]}"; do
    if [[ -z "$COMBO_FILTER" || "$combo" == "$COMBO_FILTER" || "$combo" == "${COMBO_FILTER}:"* ]]; then
      COMBOS+=("$combo")
    fi
  done

  : > "$OUTPUT_PATH"
}

log() { echo "[${PROBE_LOG_PREFIX}] $*" >&2; }

# Runs one --reorg-probe data point, applies two-tier timeout handling, and appends its JSONL
# result line to $OUTPUT_PATH:
#   - bench_kv's own internal kReorgTimeoutSecondsDefault cap on the reorganize()/checkpoint()
#     call itself (overridable via VMEMKV_BENCH_REORG_TIMEOUT_SECONDS, see bench_kv.cpp) -- this
#     is what actually bounds the interesting measurement.
#   - this function's outer OUTER_TIMEOUT_SECONDS, a generous backstop covering setup too (which
#     the internal cap deliberately excludes), in case populate/checkpoint/churn itself hangs.
# Expects $BENCH_KV_BIN, $DB_DIR, and $OUTPUT_PATH (set by init_probe_driver() above) and
# $OUTER_TIMEOUT_SECONDS (a driver-specific constant the caller sets itself) to already be set.
# Extra args (e.g. --churn-ratio=.../--sweep-tag=...) are passed through as $6+. Returns 0 if the
# point completed cleanly (caller should keep escalating), 1 otherwise (caller should stop
# escalating for this sweep).
run_probe_point() {
  local scenario="$1" value_size="$2" mode="$3" ratio="$4" label="$5"
  shift 5
  local stdout_file stderr_file exit_code line reason
  stdout_file="$(mktemp)"
  stderr_file="$(mktemp)"
  VMEMKV_DB_DIR="$DB_DIR" timeout "${OUTER_TIMEOUT_SECONDS}s" "$BENCH_KV_BIN" \
    --reorg-probe --scenario="$scenario" --value-size="$value_size" --mode="$mode" --ratio="$ratio" "$@" \
    >"$stdout_file" 2>"$stderr_file"
  exit_code=$?

  line="$(tail -n 1 "$stdout_file")"
  if [[ $exit_code -eq 0 && -n "$line" ]]; then
    log "${label}: $line"
    echo "$line" >> "$OUTPUT_PATH"
    rm -f "$stdout_file" "$stderr_file"
    return 0
  fi

  if [[ -n "$line" ]] && echo "$line" | python3 -c "import json,sys; json.load(sys.stdin)" >/dev/null 2>&1; then
    # The probe's own internal reorganize()/checkpoint()-timeout fired: it still printed a JSON
    # line (timed_out:true) before exiting 124. Trust that over synthesizing our own record.
    log "${label}: internal timeout -- $line"
    echo "$line" >> "$OUTPUT_PATH"
  else
    # The outer backstop killed the process before it could print anything (e.g. setup itself
    # never finished), or it crashed/errored some other way.
    reason="outer_timeout"
    if [[ $exit_code -ne 124 ]]; then
      reason="error_exit_${exit_code}"
    fi
    log "${label}: ${reason} (exit=${exit_code}); stderr: $(tail -n 5 "$stderr_file")"
    printf '{"scenario":"%s","value_size":"%s","mode":"%s","ratio":%s,"key_count":null,"elapsed_sec":%s,"timed_out":true,"failure_reason":"%s"}\n' \
      "$scenario" "$value_size" "$mode" "$ratio" "$OUTER_TIMEOUT_SECONDS" "$reason" >> "$OUTPUT_PATH"
  fi
  rm -f "$stdout_file" "$stderr_file"
  return 1
}
