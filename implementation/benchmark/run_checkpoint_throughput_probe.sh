#!/usr/bin/env bash
# run_checkpoint_throughput_probe.sh - Measures checkpoint()'s steady-state throughput
# (records/sec), one point per (scenario, value_size) combo: --mode=t1t2_steady --ratio=1.0
# --churn-ratio=1.0 (full corpus, full churn since the last cycle) against a corpus that already
# has one checkpointed generation.
#
# churn_ratio=1.0 is deliberate, not arbitrary: checkpoint()'s cost has a large fixed per-call
# setup component that dominates at low churn (observed: churn=0.01 took 1.76s for 80k touched
# records, churn=1.0 took 3.64s for 8M -- throughput is very much not constant per record at low
# churn). Using the highest churn ratio isolates the marginal per-record durabilization cost,
# which is what "does checkpoint's steady-state throughput keep up with Insert's rate" needs.
#
# No sweep, no escalation -- unlike run_reorg_scaling_probe.sh/run_defrag_scaling_probe.sh, this
# is exactly one data point per combo. Reuses run_probe_point()'s two-tier timeout handling (see
# common/reorg_probe_common.sh).
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [combo_filter]}"
DB_DIR="${3:-/tmp}"
# Optional 4th arg: same two forms as run_reorg_scaling_probe.sh's COMBO_FILTER
# ("in_memory"/"ltm" or "in_memory:8B"/"ltm:64KB"). Empty (the default) means "all 4 combos".
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

log() { echo "[checkpoint-throughput-probe] $*" >&2; }

# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  log "=== ${scenario}/${value_size}/t1t2_steady (churn_ratio=1.0) ==="
  run_probe_point "$scenario" "$value_size" t1t2_steady 1.0 "checkpoint_throughput" \
    --churn-ratio=1.0 --sweep-tag=checkpoint_throughput
done

log "done. Results written to $OUTPUT_PATH"
