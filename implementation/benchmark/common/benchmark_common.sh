#!/usr/bin/env bash

# Shared helper functions for VMemKV benchmark runners.
# Keep this file shell-only and dependency-light so both local and AWS runners
# can source it without pulling in extra runtime requirements.

vmemkv_common_dir() {
  cd "$(dirname "${BASH_SOURCE[0]}")" && pwd
}

vmemkv_repo_root() {
  local script_dir
  script_dir="$(vmemkv_common_dir)"
  cd "$script_dir/../../.." && pwd
}

vmemkv_impl_root() {
  local repo_root
  repo_root="$(vmemkv_repo_root)"
  printf '%s\n' "$repo_root/implementation"
}

vmemkv_git_revision() {
  local repo_root
  repo_root="$(vmemkv_repo_root)"
  git -C "$repo_root" rev-parse --short=12 HEAD
}

vmemkv_git_dirty() {
  local repo_root
  repo_root="$(vmemkv_repo_root)"
  if git -C "$repo_root" status --porcelain --untracked-files=no | awk 'NF { exit 1 }'; then
    printf '%s\n' "0"
  else
    printf '%s\n' "1"
  fi
}

vmemkv_scenario_order_label() {
  local joined=""
  local scenario_key

  for scenario_key in "$@"; do
    if [[ -z "$joined" ]]; then
      joined="$scenario_key"
    else
      joined+="->$scenario_key"
    fi
  done
  printf '%s\n' "$joined"
}

vmemkv_value_order_label() {
  local joined=""
  local value_key

  for value_key in "$@"; do
    if [[ -z "$joined" ]]; then
      case "$value_key" in
        64kb) joined="64KB" ;;
        1kb) joined="1KB" ;;
        *) joined="8B" ;;
      esac
    else
      case "$value_key" in
        64kb) joined+="->64KB" ;;
        1kb) joined+="->1KB" ;;
        *) joined+="->8B" ;;
      esac
    fi
  done
  printf '%s\n' "$joined"
}

vmemkv_target_profile_label() {
  local joined=""
  local scenario_key
  local ratio

  for scenario_key in "$@"; do
    case "$scenario_key" in
      in_memory) ratio="0.5" ;;
      ltm) ratio="2.0" ;;
      *) ratio="?" ;;
    esac

    if [[ -z "$joined" ]]; then
      joined="$scenario_key=$ratio"
    else
      joined+="->$scenario_key=$ratio"
    fi
  done
  printf '%s\n' "$joined"
}

vmemkv_benchmark_memo() {
  local run_kind="$1"
  shift
  local has_ltm=false
  local scenario_key

  for scenario_key in "$@"; do
    if [[ "$scenario_key" == "ltm" ]]; then
      has_ltm=true
      break
    fi
  done

  if [[ "$has_ltm" == "true" && "$run_kind" == aws_* ]]; then
    printf '%s\n' "AWS LTM runs use a 1GiB cgroup budget so corpus sizes stay bounded by the effective memory limit."
  fi
}

vmemkv_mem_total_bytes() {
  awk '/MemTotal:/ { print $2 * 1024; exit }' /proc/meminfo
}

vmemkv_swap_total_bytes() {
  awk '/SwapTotal:/ { print $2 * 1024; exit }' /proc/meminfo
}

vmemkv_cpu_model() {
  awk -F': ' '/model name/ { print $2; exit }' /proc/cpuinfo
}

vmemkv_cgroup_memory_limit_bytes() {
  local cgroup_v2_limit="/sys/fs/cgroup/memory.max"
  local cgroup_v1_limit="/sys/fs/cgroup/memory/memory.limit_in_bytes"
  local limit

  if [[ -r "$cgroup_v2_limit" ]]; then
    limit="$(cat "$cgroup_v2_limit")"
    if [[ "$limit" != "max" ]]; then
      printf '%s\n' "$limit"
      return 0
    fi
  fi

  if [[ -r "$cgroup_v1_limit" ]]; then
    limit="$(cat "$cgroup_v1_limit")"
    # cgroup v1 limit can be a very large number (e.g. 9223372036854771712) when unlimited.
    if [[ -n "$limit" && "$limit" -lt 9000000000000000000 ]]; then
      printf '%s\n' "$limit"
      return 0
    fi
  fi

  printf '%s\n' "-1"
  return 0
}

vmemkv_context_env_array() {
  local -n env_ref="$1"
  shift
  local run_kind="$1"
  local build_type="$2"
  local build_dir="$3"
  local enable_rocksdb="$4"
  local scenario_order="$5"
  local benchmark_flags="$6"
  local memory_budget_source="${7:-}"
  local memory_budget_bytes="${8:-}"
  local instance_type="${9:-}"
  local aws_region="${10:-}"
  local memo="${11:-}"
  local memory_high_bytes="${12:-}"
  local memory_max_bytes="${13:-}"
  local memory_swap_max_bytes="${14:-}"
  local swap_storage_media="${15:-}"
  local cgroup_limit=""

  env_ref=()
  env_ref+=("VMEMKV_CONTEXT_git_revision=$(vmemkv_git_revision)")
  env_ref+=("VMEMKV_CONTEXT_git_dirty=$(vmemkv_git_dirty)")
  env_ref+=("VMEMKV_CONTEXT_run_kind=$run_kind")
  env_ref+=("VMEMKV_CONTEXT_build_type=$build_type")
  env_ref+=("VMEMKV_CONTEXT_build_dir=$build_dir")
  env_ref+=("VMEMKV_CONTEXT_enable_rocksdb=$enable_rocksdb")
  env_ref+=("VMEMKV_CONTEXT_scenario_order=$scenario_order")
  env_ref+=("VMEMKV_CONTEXT_flags=$benchmark_flags")
  if [[ -n "$memory_budget_source" ]]; then
    env_ref+=("VMEMKV_CONTEXT_memory_budget_source=$memory_budget_source")
  fi
  if [[ -n "$memory_budget_bytes" ]]; then
    env_ref+=("VMEMKV_CONTEXT_memory_budget_bytes=$memory_budget_bytes")
  fi
  if [[ -n "$memory_high_bytes" ]]; then
    env_ref+=("VMEMKV_CONTEXT_memory_high_bytes=$memory_high_bytes")
  fi
  if [[ -n "$memory_max_bytes" ]]; then
    env_ref+=("VMEMKV_CONTEXT_memory_max_bytes=$memory_max_bytes")
  fi
  if [[ -n "$memory_swap_max_bytes" ]]; then
    env_ref+=("VMEMKV_CONTEXT_memory_swap_max_bytes=$memory_swap_max_bytes")
  fi
  if [[ -n "$swap_storage_media" ]]; then
    env_ref+=("VMEMKV_CONTEXT_swap_storage_media=$swap_storage_media")
  fi
  env_ref+=("VMEMKV_CONTEXT_kernel_release=$(uname -r)")
  env_ref+=("VMEMKV_CONTEXT_cpu_model=$(vmemkv_cpu_model)")
  env_ref+=("VMEMKV_CONTEXT_cpu_count=$(nproc --all)")
  env_ref+=("VMEMKV_CONTEXT_mem_total_bytes=$(vmemkv_mem_total_bytes)")
  cgroup_limit="$(vmemkv_cgroup_memory_limit_bytes 2>/dev/null || true)"
  env_ref+=("VMEMKV_CONTEXT_cgroup_memory_limit_bytes=$cgroup_limit")
  env_ref+=("VMEMKV_CONTEXT_swap_total_bytes=$(vmemkv_swap_total_bytes)")
  if [[ -n "$instance_type" ]]; then
    env_ref+=("VMEMKV_CONTEXT_instance_type=$instance_type")
  fi
  if [[ -n "$aws_region" ]]; then
    env_ref+=("VMEMKV_CONTEXT_aws_region=$aws_region")
  fi
  if [[ -n "$memo" ]]; then
    env_ref+=("VMEMKV_CONTEXT_memo=$memo")
  fi
}

vmemkv_context_env_prefix() {
  local -a env_pairs=()
  local -a quoted=()
  local pair

  vmemkv_context_env_array env_pairs "$@"
  for pair in "${env_pairs[@]}"; do
    printf -v pair '%q' "$pair"
    quoted+=("$pair")
  done
  printf '%s ' "${quoted[@]}"
}

vmemkv_build_bench() {
  local source_dir="$1"
  local build_dir="$2"
  local build_type="$3"
  local enable_rocksdb="$4"
  shift 4

  local -a configure_args=(
    -S "$source_dir"
    -B "$build_dir"
    -DCMAKE_BUILD_TYPE="$build_type"
    -DENABLE_ROCKSDB="$enable_rocksdb"
    -DENABLE_BENCHMARK=ON
  )
  if (($# > 0)); then
    configure_args+=("$@")
  fi

  cmake "${configure_args[@]}"
  cmake --build "$build_dir" --target bench_kv --parallel
}
