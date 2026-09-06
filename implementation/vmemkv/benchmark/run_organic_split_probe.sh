#!/usr/bin/env bash
# run_organic_split_probe.sh - Insert-QPS impact of ShardedT1Index's own automatic per-shard
# background splitting under sustained write load. Complements run_background_jobs_probe.sh, which
# measures a forced, whole-store reorganize()/checkpoint() call (every shard synchronously merged
# in one call) -- not what happens during ordinary operation, where each shard is split
# independently and incrementally by the background worker pool as it individually crosses its own
# threshold. This probe starts from an empty store and inserts continuously (fixed 1KB values,
# monotonically increasing keys), detecting each organic split as it completes (via
# get_statistics().t1_split_count) and comparing Insert QPS just before it to Insert QPS during it
# -- see bench_kv.cpp's run_organic_split_probe() for the actual measurement.
#
# Usage: run_organic_split_probe.sh <bench_kv_binary> <output_jsonl_path> [db_dir] [scenario_filter]
#   scenario_filter: "in_memory" or "ltm" (default: both). Matches run_bench_aws_c6id.sh's
#   run_remote_probe() calling convention (in_memory unconstrained, ltm cgroup-wrapped by the
#   caller via systemd-run -- this script applies no memory limit itself).
set -uo pipefail  # deliberately not -e: probe/timeout exit codes are inspected explicitly below

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common/reorg_probe_common.sh
source "$SCRIPT_DIR/common/reorg_probe_common.sh"

PROBE_LOG_PREFIX="organic-split-probe"
BENCH_KV_BIN="${1:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [scenario_filter]}"
OUTPUT_PATH="${2:?usage: $0 <bench_kv_binary> <output_jsonl_path> [db_dir] [scenario_filter]}"
DB_DIR="${3:-/tmp}"
SCENARIO_FILTER="${4:-}"

: > "$OUTPUT_PATH"

# Generous: the probe itself runs a fixed ~90s (bench_kv.cpp's kOrganicSplitProbeDurationSec) of
# sustained inserts, so there's real headroom needed beyond that alone.
OUTER_TIMEOUT_SECONDS=300

SCENARIOS=("in_memory" "ltm")
if [[ -n "$SCENARIO_FILTER" ]]; then
  # Matches run_remote_probe()'s convention of passing "in_memory"/"ltm" (see its own comment).
  SCENARIOS=("${SCENARIO_FILTER%%:*}")
fi

for scenario in "${SCENARIOS[@]}"; do
  log "=== ${scenario} ==="
  run_probe_point "$scenario" "1KB" "organic_split_probe" "1.0" "${scenario}/organic_split"
done

log "done. Results written to $OUTPUT_PATH"
