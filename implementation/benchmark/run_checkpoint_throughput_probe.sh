#!/usr/bin/env bash
# run_checkpoint_throughput_probe.sh - Measures checkpoint()'s steady-state throughput
# (records/sec), one point per (scenario, value_size) combo: --mode=t1t2_steady --ratio=1.0
# --churn-ratio=0.25 (full corpus size, 25% of it touched since the last cycle) against a corpus
# that already has one checkpointed generation.
#
# A high (but not maximal) churn ratio is deliberate, not arbitrary: checkpoint()'s cost has a
# large fixed per-call setup component that dominates at low churn (at in_memory/1KB, churn=0.01
# takes ~1.8s for 80k touched records vs. churn=0.25's ~3.6s for 2M -- 0.25 already isolates the
# marginal per-record durabilization cost well past that fixed-overhead-dominated region).
# Pushing the churn ratio higher costs proportionally more corpus-wide setup work and checkpoint()
# I/O, which risks the outer/internal timeouts on the largest combos (in_memory/8B's 20M-key
# corpus, ltm's memory-constrained scenarios, and large-value ltm/64KB records).
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

# Larger than run_reorg_scaling_probe.sh/run_defrag_scaling_probe.sh's 300s: applying churn to a
# large corpus is itself setup work independent of checkpoint()'s own cost, and needs more
# headroom than a single reorganize()/defragment() call does.
OUTER_TIMEOUT_SECONDS=600
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
  log "=== ${scenario}/${value_size}/t1t2_steady (churn_ratio=0.25) ==="
  run_probe_point "$scenario" "$value_size" t1t2_steady 1.0 "checkpoint_throughput" \
    --churn-ratio=0.25 --sweep-tag=checkpoint_throughput
done

log "done. Results written to $OUTPUT_PATH"
