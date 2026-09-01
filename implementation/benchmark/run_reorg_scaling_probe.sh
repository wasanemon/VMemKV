#!/usr/bin/env bash
# run_reorg_scaling_probe.sh - Sweeps reorganize()'s wall-clock duration as a function of corpus
# size (--mode=t1only), across all 4 (scenario, value_size) combinations already used by the main
# benchmark matrix (in_memory/8B, in_memory/1KB, ltm/1KB, ltm/64KB).
#
# checkpoint()'s cost is a separate question (see run_checkpoint_throughput_probe.sh): it only
# durabilizes the tail since the last cycle, so its cost tracks churn, not corpus size, and the
# operationally relevant number is steady-state throughput vs. Insert's rate -- not a bootstrap
# duration swept by corpus size like reorganize()'s below.
#
# Not part of the Google Benchmark-registered matrix: reorganize() is a single, possibly very slow
# blocking call, not a repeatable operation GB's timing-loop model expects. Each data point
# instead runs bench_kv's standalone `--reorg-probe` CLI mode as its own process, wrapped in
# `timeout` twice:
#   - bench_kv's own internal cap on the reorganize() call itself (see kReorgTimeoutSecondsDefault
#     in bench_kv.cpp, overridable via VMEMKV_BENCH_REORG_TIMEOUT_SECONDS) -- this is what
#     actually bounds the interesting measurement.
#   - this script's outer OUTER_TIMEOUT_SECONDS, a generous backstop covering setup too (which
#     the internal cap deliberately excludes), in case populate itself hangs.
# Ratios are swept ascending (25%/50%/75%/100% of the scenario's normal target size); as soon as
# one ratio times out (internally or via the outer backstop) or errors, escalation to larger
# ratios stops for that combo -- there is no reason to expect a larger corpus to be faster. This
# bounds the worst-case total run time to (#combos * #ratios * OUTER_TIMEOUT_SECONDS), though in
# practice it is far less.
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"
init_probe_driver "reorg-scaling-probe" "$@"

OUTER_TIMEOUT_SECONDS=300
RATIOS=(0.25 0.5 0.75 1.0)

for combo in "${COMBOS[@]}"; do
  scenario="${combo%%:*}"
  value_size="${combo##*:}"
  log "=== ${scenario}/${value_size}/t1only ==="
  for ratio in "${RATIOS[@]}"; do
    if ! run_probe_point "$scenario" "$value_size" t1only "$ratio" "ratio=${ratio}"; then
      log "ratio=${ratio} did not complete cleanly -- stopping escalation for ${scenario}/${value_size}/t1only"
      break
    fi
  done
done

log "done. Results written to $OUTPUT_PATH"
