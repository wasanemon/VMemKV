#!/usr/bin/env bash
# run_reorg_scaling_probe.sh - Sweeps VMemKVImpl::reorganize()'s wall-clock duration as a
# function of corpus size, separately for T1-only and T1+T2 modes, across all 4
# (scenario, value_size) combinations already used by the main benchmark matrix
# (in_memory/8B, in_memory/1KB, ltm/1KB, ltm/64KB).
#
# Not part of the Google Benchmark-registered matrix: reorganize() is a single, possibly very
# slow blocking call (LTM's cgroup memory constraint can make even a T1-only reorganize run
# "well past a minute" -- see bench_kv.cpp's make_vmemkv_clone_from_t1only_snapshot() comment),
# not a repeatable operation GB's timing-loop model expects. Each data point instead runs
# bench_kv's standalone `--reorg-probe` CLI mode as its own process, wrapped in `timeout` twice:
#   - bench_kv's own internal 60s cap on the reorganize() call itself (see kReorgTimeoutSeconds
#     in bench_kv.cpp) -- this is what actually bounds the interesting measurement.
#   - this script's outer OUTER_TIMEOUT_SECONDS, a generous backstop covering populate too (which
#     the internal cap deliberately excludes), in case populate itself hangs.
# Ratios are swept ascending (25%/50%/75%/100% of the scenario's normal target size); as soon as
# one ratio times out (internally or via the outer backstop) or errors, escalation to larger
# ratios stops for that (combo, mode) pair -- there is no reason to expect a larger corpus to be
# faster. This bounds the worst-case total run time to
# (#combos * #modes * #ratios * OUTER_TIMEOUT_SECONDS), though in practice it is far less.
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
DB_DIR="${3:-/tmp}"
# Optional 4th arg: restrict which combos run. Two forms:
#   "in_memory" / "ltm"          -- every combo for that scenario (both its value sizes)
#   "in_memory:8B" / "ltm:64KB"  -- exactly one (scenario, value_size) combo
# The scenario-only form exists so a caller that must wrap only the ltm half in a memory cgroup
# (see run_bench_aws_c6id.sh's run_scenario(), which does exactly this for the normal benchmark
# matrix) can invoke this script twice -- once unconstrained for in_memory, once
# systemd-run-wrapped for ltm. The exact-combo form exists so a caller that's already scoped to
# one (scenario, value_size) pair -- e.g. one of run_4parallel_bench.sh's 4 instances, which each
# own exactly one such pair -- can run only its own combo instead of duplicating its sibling's.
# Empty (the default) means "all 4 combos", for local testing.
COMBO_FILTER="${4:-}"

OUTER_TIMEOUT_SECONDS=300
RATIOS=(0.25 0.5 0.75 1.0)
ALL_COMBOS=("in_memory:8B" "in_memory:1KB" "ltm:1KB" "ltm:64KB")
COMBOS=()
for combo in "${ALL_COMBOS[@]}"; do
  if [[ -z "$COMBO_FILTER" || "$combo" == "$COMBO_FILTER" || "$combo" == "${COMBO_FILTER}:"* ]]; then
    COMBOS+=("$combo")
  fi
done
MODES=("t1only" "t1t2")

: > "$OUTPUT_PATH"

log() { echo "[reorg-scaling-probe] $*" >&2; }

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  for mode in "${MODES[@]}"; do
    log "=== ${scenario}/${value_size}/${mode} ==="
    for ratio in "${RATIOS[@]}"; do
      stdout_file="$(mktemp)"
      stderr_file="$(mktemp)"
      VMEMKV_DB_DIR="$DB_DIR" timeout "${OUTER_TIMEOUT_SECONDS}s" "$BENCH_KV_BIN" \
        --reorg-probe --scenario="$scenario" --value-size="$value_size" --mode="$mode" --ratio="$ratio" \
        >"$stdout_file" 2>"$stderr_file"
      exit_code=$?

      line="$(tail -n 1 "$stdout_file")"
      if [[ $exit_code -eq 0 && -n "$line" ]]; then
        log "ratio=${ratio}: $line"
        echo "$line" >> "$OUTPUT_PATH"
        rm -f "$stdout_file" "$stderr_file"
        continue
      fi

      if [[ -n "$line" ]] && echo "$line" | python3 -c "import json,sys; json.load(sys.stdin)" >/dev/null 2>&1; then
        # The probe's own internal reorganize()-timeout fired: it still printed a JSON line
        # (timed_out:true) before exiting 124. Trust that over synthesizing our own record.
        log "ratio=${ratio}: internal timeout -- $line"
        echo "$line" >> "$OUTPUT_PATH"
      else
        # The outer backstop killed the process before it could print anything (e.g. populate
        # itself never finished), or it crashed/errored some other way.
        reason="outer_timeout"
        if [[ $exit_code -ne 124 ]]; then
          reason="error_exit_${exit_code}"
        fi
        log "ratio=${ratio}: ${reason} (exit=${exit_code}); stderr: $(tail -n 5 "$stderr_file")"
        printf '{"scenario":"%s","value_size":"%s","mode":"%s","ratio":%s,"key_count":null,"elapsed_sec":%s,"timed_out":true,"failure_reason":"%s"}\n' \
          "$scenario" "$value_size" "$mode" "$ratio" "$OUTER_TIMEOUT_SECONDS" "$reason" >> "$OUTPUT_PATH"
      fi
      rm -f "$stdout_file" "$stderr_file"

      log "ratio=${ratio} did not complete cleanly -- stopping escalation for ${scenario}/${value_size}/${mode}"
      break
    done
  done
done

log "done. Results written to $OUTPUT_PATH"
