#!/usr/bin/env bash

# Source of truth for benchmark filters and result paths.
# Keep the data here so both local and AWS runners stay in sync without
# duplicating filter strings.

# VMemKV variant set per scenario. The read-policy ablation pins (ReadRandom/ReadSeq)
# only diverge from the trifecta under memory pressure; in_memory runs measure them
# identical everywhere, so inmem matrices run the base three variants only (their
# per-variant masters are never built either). LTM keeps the full set.
vmemkv_matrix::vmemkv_variant_filter() {
  case "${1:-}" in
    in_memory) printf '%s\n' 'Baseline|Bloom|Bloom-T1InlineValue' ;;
    *) printf '%s\n' 'Baseline|Bloom|Bloom-T1InlineValue|Bloom-T1InlineValue-ReadRandom|Bloom-T1InlineValue-ReadSeq' ;;
  esac
}

vmemkv_matrix::scenario_filter() {
  # vmemkv_only ("true"/"1"): drops the four rival backends
  # (RocksDB/RocksDB-BlobDB/LMDB/LeanStore) from
  # the filter -- their numbers are unaffected by a VMemKV-internal-only code change, so
  # re-measuring them is pure wasted AWS time/cost for a regression-check run. Their most recent
  # full-matrix numbers (from a run with this left off) are meant to be merged back in afterward
  # rather than re-measured every time; see merge_partial_results.py (--keep-prefix Store=VMemKV/).
  local scenario_key="${1:-}"
  local vmemkv_only="${2:-}"
  local variants
  variants="$(vmemkv_matrix::vmemkv_variant_filter "$scenario_key")"
  if [[ "$vmemkv_only" == "true" || "$vmemkv_only" == "1" ]]; then
    printf '(^Store=VMemKV/Variant=(%s)/)\n' "$variants"
  else
    printf '(^Store=VMemKV/Variant=(%s)/|^Store=RocksDB/|^Store=RocksDB-BlobDB/|^Store=LMDB/|^Store=LeanStore/)\n' "$variants"
  fi
}

vmemkv_matrix::scenario_filter_no_leanstore() {
  # Same as scenario_filter() without LeanStore. LeanStore's BTreeVI key/value lengths are u16,
  # so the 64KB corpus (65536-byte values) is unstorable there; benchmark_filter_for_case()
  # substitutes this for 64KB cases (RE2 has no negative lookahead, hence the explicit
  # alternation instead of an exclusion pattern).
  local scenario_key="${1:-}"
  local vmemkv_only="${2:-}"
  local variants
  variants="$(vmemkv_matrix::vmemkv_variant_filter "$scenario_key")"
  if [[ "$vmemkv_only" == "true" || "$vmemkv_only" == "1" ]]; then
    printf '(^Store=VMemKV/Variant=(%s)/)\n' "$variants"
  else
    printf '(^Store=VMemKV/Variant=(%s)/|^Store=RocksDB/|^Store=RocksDB-BlobDB/|^Store=LMDB/)\n' "$variants"
  fi
}

# LTM scenario memory parameters: a small *declared* budget (fed to bench_kv as
# VMEMKV_CONTEXT_memory_budget_bytes) combined with a large target_ratio (8.0) yields a
# corpus a few times bigger than the budget. Actually forcing that corpus to spill into
# swap requires constraining the process's real RSS to roughly this budget (e.g. via
# `systemd-run ... -p MemoryHigh=... -p MemoryMax=...` as the AWS runner does); without
# that cgroup wrap, the numbers here only size the corpus consistently across
# environments, they do not by themselves recreate memory pressure.
vmemkv_matrix::ltm_memory_budget_bytes() {
  printf '%s\n' "$((1 * 1024 * 1024 * 1024))"
}

vmemkv_matrix::ltm_swap_budget_bytes() {
  printf '%s\n' "$((1024 * 1024 * 1024 * 1024))"
}

vmemkv_matrix::scenario_env_prefix() {
  case "$1" in
    in_memory) printf '%s\n' "VMEMKV_BENCH_TARGET_RATIO=0.5" ;;
    ltm) printf '%s\n' "VMEMKV_BENCH_LTM=1 VMEMKV_BENCH_TARGET_RATIO=8.0" ;;
    *) return 1 ;;
  esac
}

vmemkv_matrix::scenario_result_path() {
  case "$1" in
    in_memory) printf '%s\n' "/tmp/results_in_memory.json" ;;
    ltm) printf '%s\n' "/tmp/results_ltm.json" ;;
    *) return 1 ;;
  esac
}

vmemkv_matrix::scenario_order_keys_from_flag() {
  case "$1" in
    A_FIRST|"") printf '%s\n' "in_memory ltm" ;;
    B_FIRST) printf '%s\n' "ltm in_memory" ;;
    *) return 1 ;;
  esac
}

vmemkv_matrix::scenario_value_order_keys_from_flag() {
  local scenario_key="$1"
  local large_value_first="${2:-0}"

  case "$scenario_key" in
    in_memory)
      case "$large_value_first" in
        true|1) printf '%s\n' "1kb 8b" ;;
        false|0|"") printf '%s\n' "8b 1kb" ;;
        *) return 1 ;;
      esac
      ;;
    ltm)
      case "$large_value_first" in
        true|1) printf '%s\n' "64kb 1kb" ;;
        false|0|"") printf '%s\n' "1kb 64kb" ;;
        *) return 1 ;;
      esac
      ;;
    *)
      return 1
      ;;
  esac
}

vmemkv_matrix::value_filter_fragment() {
  case "$1" in
    8b) printf '%s\n' "Value=8B" ;;
    1kb) printf '%s\n' "Value=1KB" ;;
    64kb) printf '%s\n' "Value=64KB" ;;
    *) return 1 ;;
  esac
}

vmemkv_matrix::benchmark_filter_for_case() {
  local scenario_key="$1"
  local value_key="$2"
  local vmemkv_only="${3:-}"
  local leanstore_only="${4:-}"
  local scenario_regex

  if [[ "$leanstore_only" == "true" || "$leanstore_only" == "1" ]]; then
    # LeanStore-only re-measurement (e.g. after a rival-backend change): VMemKV and rival
    # numbers are reused from a prior full-matrix run via merge_partial_results.py (--keep-prefix Store=LeanStore/).
    # LeanStore's BTreeVI key/value lengths are u16, so the 64KB corpus is unstorable there:
    # match nothing (deliberately, so callers fail visibly instead of throwing mid-run).
    if [[ "$value_key" == "64kb" ]]; then
      printf '%s\n' '(^Store=NoSuchStore/)'
      return 0
    fi
    scenario_regex='(^Store=LeanStore/)'
  elif [[ "$value_key" == "64kb" ]]; then
    scenario_regex="$(vmemkv_matrix::scenario_filter_no_leanstore "$scenario_key" "$vmemkv_only")"
  else
    scenario_regex="$(vmemkv_matrix::scenario_filter "$scenario_key" "$vmemkv_only")"
  fi
  printf '%s\n' "(${scenario_regex}).*$(vmemkv_matrix::value_filter_fragment "$value_key")"
}

vmemkv_matrix::scenario_run_filter() {
  local scenario_key="$1"
  local value_order="$2"
  local vmemkv_only="${3:-}"
  local leanstore_only="${4:-}"
  local -a values=()
  local -a case_filters=()
  local value_key
  local joined=""

  read -r -a values <<<"$value_order"
  for value_key in "${values[@]}"; do
    case_filters+=("$(vmemkv_matrix::benchmark_filter_for_case "$scenario_key" "$value_key" "$vmemkv_only" "$leanstore_only")")
  done

  for value_key in "${case_filters[@]}"; do
    if [[ -z "$joined" ]]; then
      joined="$value_key"
    else
      joined+="|$value_key"
    fi
  done
  printf '(%s)\n' "$joined"
}

vmemkv_matrix::headline_filter() {
  # Narrows a base scenario filter ($1) to its max-thread-count cells ($2, the host's
  # hardware thread count): the rival-comparison set error bars are reported for.
  # Benchmark names always end with /real_time/threads:N, so the anchored suffix selects
  # exactly the top of the {1, 4, 16, hw} sweep (YCSB-E registers hw only, always included).
  local base_filter="$1"
  local max_threads="$2"
  printf '(%s).*threads:%s$\n' "$base_filter" "$max_threads"
}

vmemkv_matrix::scenario_effective_filter() {
  local scenario_key="$1"
  local large_value_first="${2:-0}"
  local quick="${3:-0}"
  local vmemkv_only="${4:-}"
  local leanstore_only="${5:-}"

  if [[ "$quick" == "true" || "$quick" == "1" ]]; then
    # Already VMemKV-only by construction (see scenario_quick_filter()'s own hardcoded
    # Store=VMemKV patterns) -- vmemkv_only is a no-op here either way.
    vmemkv_matrix::scenario_quick_filter "$scenario_key"
    return
  fi

  vmemkv_matrix::scenario_run_filter \
    "$scenario_key" \
    "$(vmemkv_matrix::scenario_value_order_keys_from_flag "$scenario_key" "$large_value_first")" \
    "$vmemkv_only" \
    "$leanstore_only"
}

vmemkv_matrix::ltm_priming_filter() {
  # Triggers exactly one Get/Hit/Zipf/threads:1 cell per (store, variant, value size) -- enough
  # to build the one shared master corpus every other scenario needs. Get/Update/Delete/YCSB-E/
  # Scan/Insert's LTM pre-populate all clone from this same (val_size, corpus_size) master (see
  # make_fresh_corpus_checkpoint()'s comment in bench_kv.cpp). Scan does not need a second,
  # separately-populated master: base_mmap is already established on this ordinary shared master
  # for every VMemKV variant (see bench_kv.cpp's Scan registration comment).
  # Used to "prime" this master at full, unconstrained disk speed before a cgroup-wrapped LTM run
  # (see run_bench.sh's --cgroup path): a real per-master populate under that same cgroup was
  # measured at 200-1200s, vs. ~20-30s unconstrained.
  local vmemkv_only="${1:-}"
  local leanstore_only="${2:-}"
  # LeanStore's priming cells, spelled explicitly (BTreeVI u16 length ceiling: no 64KB
  # cells can exist there): the leanstore-only fast path and the full run's second
  # alternative below share this.
  local leanstore_prime='(^Store=LeanStore/).*Op=Get/Mode=Hit/Dist=Zipf/.*Value=(8B|1KB).*threads:1$'
  if [[ "$leanstore_only" == "true" || "$leanstore_only" == "1" ]]; then
    # LeanStore-only re-measurement: only its own masters need priming.
    printf '(%s)\n' "$leanstore_prime"
    return 0
  fi
  local scenario_regex
  # LTM scope: ablation variants still run there, so prime the full variant set.
  scenario_regex="$(vmemkv_matrix::scenario_filter "ltm" "$vmemkv_only")"
  if [[ "$vmemkv_only" == "true" || "$vmemkv_only" == "1" ]]; then
    printf '(%s).*Op=Get/Mode=Hit/Dist=Zipf/.*threads:1$\n' "$scenario_regex"
  else
    # Full runs additionally prime LeanStore's masters -- except its 64KB cells, which cannot
    # exist (BTreeVI u16 length ceiling; see benchmark_filter_for_case()). RE2 has no negative
    # lookahead, so the first alternative drops LeanStore while the second spells its storable
    # value sizes explicitly.
    local no_leanstore_regex
    no_leanstore_regex="$(vmemkv_matrix::scenario_filter_no_leanstore "ltm" "$vmemkv_only")"
    printf '((%s).*Op=Get/Mode=Hit/Dist=Zipf/.*threads:1$|%s)\n' \
      "$no_leanstore_regex" "$leanstore_prime"
  fi
}

vmemkv_matrix::scenario_quick_filter() {
  local scenario_key="$1"

  case "$scenario_key" in
    in_memory)
      # One representative in-memory workload from the tail end of the matrix
      # (Scan is registered last). threads:1 keeps this portable across
      # machines with different core counts (unlike a hardcoded thread count).
      printf '%s\n' '^Store=VMemKV/Variant=Bloom-T1InlineValue/Op=Scan/Dist=Uniform/Value=8B/real_time/threads:1$'
      ;;
    ltm)
      # One representative LTM workload, chosen to reach the scenario boundary quickly.
      # Value labels carry a "(20% 8B)" suffix for non-8B sizes, hence the ".*". The variant
      # here must match one VMemKVImpl::name() (vmemkv_impl.hpp) can actually produce, or this
      # filter silently matches zero benchmarks.
      printf '%s\n' '^Store=VMemKV/Variant=Bloom-T1InlineValue/Op=Get/Mode=Hit/Dist=Zipf/Value=64KB.*threads:1$'
      ;;
    *)
      return 1
      ;;
  esac
}

vmemkv_matrix::read_policy_ablation_filter() {
  # Trifecta on/off ablation: representative read cells only (Scan + Get Hit + YCSB-E),
  # comparing the production config (Bloom-T1InlineValue, i.e. the trifecta) against its two
  # single-policy pins (ReadRandom/ReadSeq). Insert/Update/Delete/Get Miss are trimmed: the
  # read path cannot move them. Scenarios/values are chosen by the caller pairing this with
  # scenario_run_filter-style invocation -- LTM 1KB/64KB is the paper's representative set.
  local value_key="$1"
  local value_regex
  value_regex="$(vmemkv_matrix::value_filter_fragment "$value_key")"
  printf '%s\n' "(^Store=VMemKV/Variant=Bloom-T1InlineValue(-ReadRandom|-ReadSeq)?/Op=(Scan/|Get/Mode=Hit/|YCSB-E/)).*${value_regex}"
}
