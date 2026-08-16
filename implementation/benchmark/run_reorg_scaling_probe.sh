#!/usr/bin/env bash
# run_reorg_scaling_probe.sh - Sweeps VMemKVImpl::reorganize()'s wall-clock duration as a
# function of corpus size, separately for T1-only and T1+T2 modes, across all 4
# (scenario, value_size) combinations already used by the main benchmark matrix
# (in_memory/8B, in_memory/1KB, ltm/1KB, ltm/64KB).
#
# Also runs the "Corpus-Size Invariance across Generations" experiment (--mode=t1t2_steady, see
# bench_kv.cpp's reorg_probe namespace comment): unlike t1only/t1t2 above, which always measure a
# first-ever (bootstrap, O(N)) checkpoint, this sweeps the same 4 ratios but measures a *second*
# defragment() call after one churn round, against a corpus that already has a prior generation to
# reflink from -- the O(diff) steady-state path checkpoint_and_defragment() actually optimizes for.
# Scoped to ltm:1KB only (the combo the original O(N)-scaling pathology was found in -- see TODO.md
# item 4's resolution): unlike t1only/t1t2, which want the full comparison matrix, this experiment
# only needs to demonstrate the effect where it matters most. Reuses one corpus across all 4 ratio
# points (bulk_load()ing just the incremental delta each time -- see run_steady()'s own comment in
# bench_kv.cpp) rather than rebuilding independently at each point, so its total populate cost is
# ~100% of a full corpus instead of t1only/t1t2's 250% (25+50+75+100% built independently) -- see
# run_churn_scaling_probe.sh's header for the companion "Churn-Ratio Scaling" experiment, which
# sweeps --churn-ratio instead of --ratio and shares this same t1t2_steady mode.
#
# Not part of the Google Benchmark-registered matrix: reorganize()/defragment() is a single,
# possibly very slow blocking call (LTM's cgroup memory constraint can make even a T1-only
# reorganize run "well past a minute"), not a repeatable operation GB's timing-loop model expects.
# Each data point instead runs
# bench_kv's standalone `--reorg-probe` CLI mode as its own process, wrapped in `timeout` twice:
#   - bench_kv's own internal 60s cap on the reorganize()/defragment() call itself (see
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
