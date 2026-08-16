#!/usr/bin/env bash
# run_churn_scaling_probe.sh - "Churn-Ratio Scaling" experiment: sweeps
# checkpoint_and_defragment()'s wall-clock duration as a function of *churn ratio* (the fraction
# of keys updated since the previous checkpoint), at a *fixed* corpus size -- the direct evidence
# for the reflink+punch redesign's core claim that cost is O(diff since last cycle), not O(N).
# Companion to run_reorg_scaling_probe.sh's "Corpus-Size Invariance across Generations" experiment
# (fixed churn, varying corpus size instead); both share bench_kv.cpp's --mode=t1t2_steady and
# this file's run_probe_point() helper.
#
# Scope, chosen to keep AWS time down (see chat/design notes, 2026-08-16): the dense churn-ratio
# sweep runs at in_memory scale only -- the O(N)-vs-O(diff) distinction is a disk-I/O-*volume*
# argument, not a memory-pressure one, so its shape doesn't need real swap thrashing to show up,
# and in_memory iteration is fast/cheap (no cgroup wait). A single additional low-churn point is
# run at ltm scale (via --ltm-spot-check-only, meant to be invoked separately from run_bench_aws_
# c6id.sh's cgroup-wrapped LTM pass): this is the direct replacement for the old ">27 minutes,
# didn't finish" pathological result (TODO.md item 4) -- a concrete completion time instead.
#
# Each data point reuses run_reorg_scaling_probe.sh's two-tier timeout handling (see
# common/reorg_probe_common.sh's run_probe_point()). Churn ratios are swept ascending; unlike the
# corpus-size sweep, a slow/timed-out point here doesn't imply a larger churn ratio would also be
# slow for a structural reason (the whole point is to see whether elapsed_sec tracks churn_ratio),
# so escalation does NOT stop early -- every point in CHURN_RATIOS always runs.
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [--ltm-spot-check-only]}"
OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [--ltm-spot-check-only]}"
DB_DIR="${3:-/tmp}"
# Optional 4th arg: when "--ltm-spot-check-only", run *only* the single ltm/1KB low-churn spot
# check (meant for a caller that wraps just this in a memory cgroup, mirroring
# run_reorg_scaling_probe.sh's scenario-only COMBO_FILTER form) instead of the full in_memory
# sweep. Default (empty) runs the in_memory sweep only, no ltm point -- the ltm point needs real
# cgroup wrapping to mean anything and shouldn't accidentally run unconstrained.
MODE_FILTER="${4:-}"

OUTER_TIMEOUT_SECONDS=300
CHURN_RATIOS=(0.0 0.01 0.05 0.1 0.25 0.5 1.0)
SWEEP_TAG="churn_scaling"

: > "$OUTPUT_PATH"

log() { echo "[churn-scaling-probe] $*" >&2; }

# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"

if [[ "$MODE_FILTER" != "--ltm-spot-check-only" ]]; then
  log "=== in_memory/1KB/t1t2_steady churn-ratio sweep ==="
  for churn_ratio in "${CHURN_RATIOS[@]}"; do
    # No early stop on timeout here (see header comment): every ratio still runs regardless of
    # run_probe_point()'s return value.
    run_probe_point in_memory 1KB t1t2_steady 1.0 "churn_ratio=${churn_ratio}" \
      --churn-ratio="$churn_ratio" --sweep-tag="$SWEEP_TAG"
  done
fi

if [[ "$MODE_FILTER" == "--ltm-spot-check-only" ]]; then
  # The direct replacement for TODO.md item 4's old ">27 minutes, didn't finish" result -- same
  # combo (ltm/1KB), same real LTM budget (VMEMKV_CONTEXT_memory_budget_bytes/target_ratio=8.0,
  # set by the caller -- see run_bench_aws_c6id.sh), same "defragment() on an already-checkpointed
  # store" scenario, but now with a churn ratio low enough (1%) to be representative of a single
  # background reorg cycle rather than a from-scratch rebuild.
  log "=== ltm/1KB/t1t2_steady spot check (churn_ratio=0.01) ==="
  run_probe_point ltm 1KB t1t2_steady 1.0 "churn_ratio=0.01" \
    --churn-ratio=0.01 --sweep-tag="$SWEEP_TAG"
fi

log "done. Results written to $OUTPUT_PATH"
