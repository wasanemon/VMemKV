#!/usr/bin/env bash
# run_reorg_scaling_probe.sh - Sweeps reorganize()/checkpoint()'s wall-clock duration as a
# function of corpus size (--mode=t1only/t1t2), across all 4 (scenario, value_size) combinations
# already used by the main benchmark matrix (in_memory/8B, in_memory/1KB, ltm/1KB, ltm/64KB).
#
# Also runs a "Corpus-Size Invariance" sweep (--mode=t1t2_steady, see bench_kv.cpp's reorg_probe
# namespace comment for what that mode measures), scoped to ltm:1KB only. Reuses one corpus across
# all 4 ratio points (bulk_load()ing just the incremental delta each time -- see run_steady()'s
# own comment in bench_kv.cpp) rather than rebuilding independently at each point. Companion to
# run_churn_scaling_probe.sh, which sweeps --churn-ratio instead of --ratio against the same mode.
#
# Not part of the Google Benchmark-registered matrix: reorganize()/checkpoint() is a single,
# possibly very slow blocking call, not a repeatable operation GB's timing-loop model expects.
# Each data point instead runs bench_kv's standalone `--reorg-probe` CLI mode as its own process,
# wrapped in `timeout` twice:
#   - bench_kv's own internal cap on the reorganize()/checkpoint() call itself (see
#     kReorgTimeoutSeconds in bench_kv.cpp) -- this is what actually bounds the interesting
#     measurement.
#   - this script's outer OUTER_TIMEOUT_SECONDS, a generous backstop covering setup too (which
#     the internal cap deliberately excludes), in case populate/checkpoint/churn itself hangs.
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

# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  for mode in "${MODES[@]}"; do
    log "=== ${scenario}/${value_size}/${mode} ==="
    for ratio in "${RATIOS[@]}"; do
      if ! run_probe_point "$scenario" "$value_size" "$mode" "$ratio" "ratio=${ratio}"; then
        log "ratio=${ratio} did not complete cleanly -- stopping escalation for ${scenario}/${value_size}/${mode}"
        break
      fi
    done
  done
done

# "Corpus-Size Invariance across Generations" -- see the file header comment above. Scoped to
# ltm:1KB only, and deliberately run *after* the t1only/t1t2 sweep above so a mid-sweep failure in
# the (unrelated) bootstrap measurements doesn't cost this one its own results.
STEADY_CHURN_RATIO=0.01
STEADY_COMBO="ltm:1KB"
if [[ -z "$COMBO_FILTER" || "$COMBO_FILTER" == "ltm" || "$COMBO_FILTER" == "$STEADY_COMBO" ]]; then
  scenario="${STEADY_COMBO%%:*}"
  value_size="${STEADY_COMBO##*:}"
  log "=== ${scenario}/${value_size}/t1t2_steady (churn_ratio=${STEADY_CHURN_RATIO}) ==="
  for ratio in "${RATIOS[@]}"; do
    if ! run_probe_point "$scenario" "$value_size" "t1t2_steady" "$ratio" "ratio=${ratio}" \
      --churn-ratio="$STEADY_CHURN_RATIO" --sweep-tag=corpus_scaling; then
      log "ratio=${ratio} did not complete cleanly -- stopping escalation for ${scenario}/${value_size}/t1t2_steady"
      break
    fi
  done
fi

log "done. Results written to $OUTPUT_PATH"
