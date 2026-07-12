#!/usr/bin/env bash

# Source of truth for benchmark filters and result paths.
# Keep the data here so both local and AWS runners stay in sync without
# duplicating filter strings.

vmemkv_matrix::scenario_filter() {
  case "$1" in
    in_memory)
      printf '%s\n' '(^Store=VMemKV/Variant=Baseline/|^Store=VMemKV/Variant=Bloom-Simd/|^Store=VMemKV/Variant=Bloom-Simd-T1InlineValue/|^Store=VMemKV/Variant=Bloom-Simd-T1InlineValue-Prefaulting/|^Store=RocksDB/Variant=RocksDB/)'
      ;;
    ltm)
      printf '%s\n' '(^Store=VMemKV/Variant=Simd/|^Store=VMemKV/Variant=Bloom-Simd/|^Store=VMemKV/Variant=Bloom-Simd-Hints/|^Store=VMemKV/Variant=Bloom-Simd-Hints-T1InlineValue/|^Store=VMemKV/Variant=Bloom-Simd-Hints-T1InlineValue-Prefaulting/|^Store=RocksDB/Variant=RocksDB/)'
      ;;
    *)
      return 1
      ;;
  esac
}

vmemkv_matrix::scenario_env_prefix() {
  case "$1" in
    in_memory) printf '%s\n' "VMEMKV_BENCH_TARGET_RATIO=0.5" ;;
    ltm) printf '%s\n' "VMEMKV_BENCH_LTM=1 VMEMKV_BENCH_TARGET_RATIO=2.0" ;;
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
      printf '%s\n' "8b"
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

vmemkv_matrix::scenario_json_filename() {
  case "$1" in
    in_memory) printf '%s\n' "results_in_memory.json" ;;
    ltm) printf '%s\n' "results_ltm.json" ;;
    *) return 1 ;;
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
  local scenario_regex
  local value_regex

  scenario_regex="$(vmemkv_matrix::scenario_filter "$scenario_key")"
  value_regex="$(vmemkv_matrix::value_filter_fragment "$value_key")"
  printf '%s\n' "(${scenario_regex}).*${value_regex}"
}

vmemkv_matrix::scenario_run_filter() {
  local scenario_key="$1"
  local value_order="$2"
  local -a values=()
  local -a case_filters=()
  local value_key
  local joined=""

  read -r -a values <<<"$value_order"
  for value_key in "${values[@]}"; do
    case_filters+=("$(vmemkv_matrix::benchmark_filter_for_case "$scenario_key" "$value_key")")
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

vmemkv_matrix::scenario_effective_filter() {
  local scenario_key="$1"
  local large_value_first="${2:-0}"
  local quick="${3:-0}"

  if [[ "$quick" == "true" || "$quick" == "1" ]]; then
    vmemkv_matrix::scenario_quick_filter "$scenario_key"
    return
  fi

  vmemkv_matrix::scenario_run_filter \
    "$scenario_key" \
    "$(vmemkv_matrix::scenario_value_order_keys_from_flag "$scenario_key" "$large_value_first")"
}

vmemkv_matrix::local_quick_filter() {
  # Smoke-test a single unique benchmark to keep `--quick` fast and
  # avoid name collisions across VMemKV variants.
  printf '%s\n' '^Store=VMemKV/Variant=Baseline/Op=Get/Mode=Hit/Dist=Zipf/Value=8B/real_time/threads:1$'
}

vmemkv_matrix::scenario_quick_filter() {
  local scenario_key="$1"

  case "$scenario_key" in
    in_memory)
      # One representative in-memory workload, chosen to match the previous
      # long-running end-of-scenario region with a single 32-thread case.
      printf '%s\n' '^Store=VMemKV/Variant=Bloom-Simd/Op=ScanReorg/Dist=Uniform/Value=8B/real_time/threads:32$'
      ;;
    ltm)
      # One representative LTM workload, chosen to reach the scenario boundary quickly.
      printf '%s\n' '^Store=VMemKV/Variant=Bloom-Simd-Hints-T1InlineValue/Op=Get/Mode=Hit/Dist=Zipf/Value=64KB/real_time/threads:1$'
      ;;
    *)
      return 1
      ;;
  esac
}
