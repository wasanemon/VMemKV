#!/usr/bin/env bash
# run_defrag_scaling_probe.sh - Sweeps defragment()'s wall-clock duration, via bench_kv's
# standalone `--reorg-probe` CLI mode (see bench_kv.cpp's run_defrag()/run_defrag_contention()).
#
# Two sweeps, both against a corpus that's already been checkpoint()ed once (a realistic
# pre-defragment state):
#   corpus-size sweep (--mode=defrag): churn_ratio fixed at 0, --ratio swept 25%/50%/75%/100%,
#     across all 4 (scenario, value_size) combinations already used by the main benchmark matrix.
#   contention spot check (--mode=defrag_contention): one point per combo (--ratio=1.0) measuring
#     concurrent write throughput with and without a defragment() running at the same time.
#     One point per combo, not a dense sweep -- it already runs two measurement phases per call
#     (isolated baseline + concurrent), so it costs roughly double a single --mode=defrag point.
#
# defragment() relocates every live record regardless of which ones changed, so unlike
# checkpoint() its cost does not depend on churn ratio -- confirmed once via a dedicated
# in_memory/1KB churn-ratio sweep (durations stayed flat across churn_ratio 0-1.0 at a fixed
# corpus size), not re-swept on every round since that finding does not change from run to run.
#
# Not part of the Google Benchmark-registered matrix, for the same reason as
# run_reorg_scaling_probe.sh/run_checkpoint_throughput_probe.sh: a single, possibly very slow
# blocking call, not a repeatable operation GB's timing-loop model expects. Reuses this file's own
# run_probe_point() helper (via common/reorg_probe_common.sh) for two-tier timeout handling.
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
DB_DIR="${3:-/tmp}"
# Optional 4th arg: restrict which combos the corpus-size sweep runs (same two forms as
# run_reorg_scaling_probe.sh's COMBO_FILTER). Empty (the default) means "all 4 combos".
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

: > "$OUTPUT_PATH"

log() { echo "[defrag-scaling-probe] $*" >&2; }

# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  log "=== ${scenario}/${value_size}/defrag corpus-size sweep ==="
  for ratio in "${RATIOS[@]}"; do
    if ! run_probe_point "$scenario" "$value_size" "defrag" "$ratio" "ratio=${ratio}" --sweep-tag=corpus_scaling; then
      log "ratio=${ratio} did not complete cleanly -- stopping escalation for ${scenario}/${value_size}/defrag"
      break
    fi
  done
done

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  log "=== ${scenario}/${value_size}/defrag_contention spot check ==="
  run_probe_point "$scenario" "$value_size" "defrag_contention" "1.0" "contention" --sweep-tag=contention
done

log "done. Results written to $OUTPUT_PATH"
