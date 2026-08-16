#!/usr/bin/env bash
# reorg_probe_common.sh - Shared driver logic for bench_kv's standalone `--reorg-probe` CLI mode,
# used by both run_reorg_scaling_probe.sh (corpus-size axis) and run_churn_scaling_probe.sh
# (churn-ratio axis). See either script's header comment for the experiments themselves; this file
# only holds the per-data-point execution/timeout-handling they share.

# Runs one --reorg-probe data point, applies two-tier timeout handling, and appends its JSONL
# result line to $OUTPUT_PATH:
#   - bench_kv's own internal kReorgTimeoutSeconds cap on the reorganize()/defragment() call
#     itself -- this is what actually bounds the interesting measurement.
#   - this function's outer OUTER_TIMEOUT_SECONDS, a generous backstop covering setup too (which
#     the internal cap deliberately excludes), in case populate/checkpoint/churn itself hangs.
# Expects $BENCH_KV_BIN, $DB_DIR, $OUTPUT_PATH, $OUTER_TIMEOUT_SECONDS, and log() to already be
# set/defined by the caller. Extra args (e.g. --churn-ratio=.../--sweep-tag=...) are passed
# through as $6+. Returns 0 if the point completed cleanly (caller should keep escalating), 1
# otherwise (caller should stop escalating for this sweep).
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
    # The probe's own internal reorganize()/defragment()-timeout fired: it still printed a JSON
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
