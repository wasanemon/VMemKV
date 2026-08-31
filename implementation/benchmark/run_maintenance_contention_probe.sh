#!/usr/bin/env bash
# run_maintenance_contention_probe.sh - Concurrent-write contention spot check for checkpoint()
# and reorganize() (T1-only), via bench_kv's standalone `--reorg-probe` CLI mode (see
# bench_kv.cpp's run_checkpoint_contention()/run_reorg_contention()).
#
# One point per combo per mode (--ratio=1.0, full corpus) -- not a sweep. checkpoint_contention
# pre-churns the corpus (fixed 0.25 ratio, matching run_checkpoint_throughput_probe.sh's
# reasoning) before measuring, since checkpoint() needs a non-trivial tail to durabilize;
# reorg_contention does not (reorganize() cost tracks live key count, not churn).
#
# reorganize() completes in well under a second even at full corpus size (T1-only, in-memory) --
# short enough that the concurrent-write-TPS window may be dominated by thread start/stop
# overhead rather than steady-state throughput. Reported as-is; treat with more skepticism than
# the checkpoint() points, which run for seconds.
#
# Not part of the Google Benchmark-registered matrix, for the same reason as
# run_reorg_scaling_probe.sh: a single, possibly slow blocking call, not a repeatable operation
# GB's timing-loop model expects. Reuses this file's own run_probe_point() helper (via
# common/reorg_probe_common.sh) for two-tier timeout handling.
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
DB_DIR="${3:-/tmp}"
# Optional 4th arg: restrict which combos run (same two forms as run_reorg_scaling_probe.sh's
# COMBO_FILTER). Empty (the default) means "all 4 combos".
COMBO_FILTER="${4:-}"

OUTER_TIMEOUT_SECONDS=300
ALL_COMBOS=("in_memory:8B" "in_memory:1KB" "ltm:1KB" "ltm:64KB")
COMBOS=()
for combo in "${ALL_COMBOS[@]}"; do
  if [[ -z "$COMBO_FILTER" || "$combo" == "$COMBO_FILTER" || "$combo" == "${COMBO_FILTER}:"* ]]; then
    COMBOS+=("$combo")
  fi
done

: > "$OUTPUT_PATH"

log() { echo "[maintenance-contention-probe] $*" >&2; }

# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  log "=== ${scenario}/${value_size}/checkpoint_contention spot check ==="
  run_probe_point "$scenario" "$value_size" "checkpoint_contention" "1.0" "checkpoint" --sweep-tag=contention
done

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  log "=== ${scenario}/${value_size}/reorg_contention spot check ==="
  run_probe_point "$scenario" "$value_size" "reorg_contention" "1.0" "reorg" --sweep-tag=contention
done

log "done. Results written to $OUTPUT_PATH"
