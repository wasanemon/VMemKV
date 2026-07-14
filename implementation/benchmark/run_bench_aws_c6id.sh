#!/usr/bin/env bash
# run_bench_aws_c6id.sh - AWS benchmark orchestrator for VMemKV.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Shared matrix and helpers.
# shellcheck source=common/benchmark_matrix.sh
source "$SCRIPT_DIR/common/benchmark_matrix.sh"
# shellcheck source=common/benchmark_common.sh
source "$SCRIPT_DIR/common/benchmark_common.sh"

# ── Parse Command Line Arguments ──────────────────────────────────────────
ORDER="A_FIRST"
LARGE_VALUE_FIRST=false
QUICK=false

show_help() {
  echo "Usage: $0 [options]"
  echo "Options:"
  echo "  -h, --help       Show this help message and exit"
  echo "  --ltm-first      Run Scenario B (Larger-than-Memory) before Scenario A (In-Memory)"
  echo "  --large-value-first  Run larger-value benchmarks before smaller-value benchmarks"
  echo "  --quick          Run one workload per scenario with a short min_time"
  exit 0
}

SCENARIO_LIMIT="all"
VALUE_SIZE_LIMIT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      show_help
      ;;
    --ltm-first)
      ORDER="B_FIRST"
      shift
      ;;
    --large-value-first|--large_value-first)
      LARGE_VALUE_FIRST=true
      shift
      ;;
    --quick)
      QUICK=true
      MIN_TIME="0.1s"
      shift
      ;;
    --scenario)
      SCENARIO_LIMIT="$2"
      shift 2
      ;;
    --value-size)
      VALUE_SIZE_LIMIT="$2"
      shift 2
      ;;
    *)
      echo "[ERROR] Unknown option: $1" >&2
      echo "Use -h or --help to see available options." >&2
      exit 1
      ;;
  esac
done

# Preserve the original terminal fds before redirecting everything through the
# timestamping pipe. Signal handlers must not depend on that pipe because it
# can be broken precisely when Ctrl+C is delivered.
exec 3>&1 4>&2

if [[ -t 1 ]]; then
  export VMEMKV_COLOR=1
fi

if [[ "$LARGE_VALUE_FIRST" == "true" ]]; then
  export VMEMKV_BENCH_LARGE_VALUE_FIRST=1
fi

# ── Configuration Parameters ──────────────────────────────────────────────
AWS_REGION="ap-northeast-1"
INSTANCE_TYPE="c6id.8xlarge"
MIN_TIME="5.0s"
LTM_MEMORY_BUDGET_BYTES=$((1 * 1024 * 1024 * 1024))
LTM_SWAP_BUDGET_BYTES=$((1024 * 1024 * 1024 * 1024))
KEY_NAME="vmemkv-c6id-key-$$"
PEM_FILE="/tmp/${KEY_NAME}.pem"
SG_NAME="vmemkv-c6id-sg-$$"
PORT=22
SIGNAL_LOG="/tmp/vmemkv_signal_${KEY_NAME}.log"
PROGRESS_STATE="/tmp/vmemkv_progress_state_${KEY_NAME}.txt"
exec > >(awk -v state_file="$PROGRESS_STATE" -f "$SCRIPT_DIR/common/benchmark_progress.awk") 2>&1

# ── SSH Security Settings ─────────────────────────────────────────────────
export AWS_DEFAULT_REGION="$AWS_REGION"
MY_IP=$(curl -s https://checkip.amazonaws.com)
if [[ -z "$MY_IP" ]]; then
  echo "[ERROR] Could not determine your local IP address." >&2
  exit 1
fi
echo "Your Local IP: $MY_IP"

# ── Cleanup Hook for Temporary Resources ──────────────────────────────────
AWS_CLEANUP_RAN=false

write_progress_state() {
  local global_total="$1"
  echo "$global_total" >"$PROGRESS_STATE"
}

count_remote_benchmarks() {
  local benchmark_filter="$1"
  local env_prefix="${2:-}"
  local runtime_prefix="${3:-}"
  ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "bash -lc 'cd ~/faultkv/implementation && ${env_prefix:+${env_prefix} }${runtime_prefix:+${runtime_prefix} }./build-rel/benchmark/bench_kv --benchmark_list_tests --benchmark_filter=\"${benchmark_filter}\" 2>/dev/null | awk \"NF { c++ } END { print c + 0 }\"'" | tail -n 1
}

signal_log() {
  local message="$1"
  local line="[signal] $message"
  printf '%s\n' "$line" >>"$SIGNAL_LOG" || true
  printf '%s\n' "$line" >&4 || true
}

run_aws_cleanup() {
  local signal_name="${1:-EXIT}"

  if [[ "$AWS_CLEANUP_RAN" == "false" ]]; then
    AWS_CLEANUP_RAN=true
    signal_log "starting aws_clean.sh signal=$signal_name pid=$$ bashtid=$BASHPID"
    if [[ "$signal_name" == "EXIT" ]]; then
      "$SCRIPT_DIR/aws/aws_clean.sh" "$KEY_NAME" || true
    else
      local cleanup_log="/tmp/vmemkv_aws_clean_${KEY_NAME}.log"
      nohup "$SCRIPT_DIR/aws/aws_clean.sh" >"$cleanup_log" 2>&1 < /dev/null &
      local cleanup_pid=$!
      signal_log "cleanup continued in background pid=$cleanup_pid log=$cleanup_log"
      wait "$cleanup_pid" || true
      signal_log "cleanup helper finished pid=$cleanup_pid signal=$signal_name"
    fi
  fi
}

on_signal() {
  set +e
  local signal_name="$1"
  local exit_code=130
  if [[ "$signal_name" == "TERM" ]]; then
    exit_code=143
  fi
  # Ignore further interrupts while cleanup is running; otherwise Ctrl+C can
  # interrupt the cleanup path itself before resources are released.
  signal_log "caught signal=$signal_name pid=$$ bashtid=$BASHPID exit_code=$exit_code"
  trap '' INT TERM
  trap - EXIT
  run_aws_cleanup "$signal_name"
  exit "$exit_code"
}

trap 'run_aws_cleanup EXIT' EXIT
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM

create_ssh_key_pair() {
  echo "Creating SSH Key Pair..."
  aws ec2 create-key-pair --key-name "$KEY_NAME" --query "KeyMaterial" --output text > "$PEM_FILE"
  chmod 400 "$PEM_FILE"
}

resolve_ubuntu_ami() {
  echo "Locating latest Ubuntu 24.04 LTS AMI..."
  AMI_ID=$(aws ec2 describe-images \
    --owners 099720109477 \
    --filters "Name=name,Values=ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*" \
              "Name=state,Values=available" \
              "Name=architecture,Values=x86_64" \
    --query "reverse(sort_by(Images, &CreationDate))[0].ImageId" \
    --output text)
  echo "Using AMI ID: $AMI_ID"
}

create_security_group() {
  echo "Creating temporary Security Group..."
  VPC_ID=$(aws ec2 describe-vpcs --filters "Name=isDefault,Values=true" --query "Vpcs[0].VpcId" --output text)
  SG_ID=$(aws ec2 create-security-group \
    --group-name "$SG_NAME" \
    --description "VMemKV Temporary Benchmark SG" \
    --vpc-id "$VPC_ID" \
    --query "GroupId" \
    --output text)

  aws ec2 create-tags --resources "$SG_ID" --tags "Key=VMemKV-Temp-Spot,Value=$KEY_NAME" >/dev/null

  echo "Authorizing SSH ingress access from your IP ($MY_IP)..."
  aws ec2 authorize-security-group-ingress \
    --group-id "$SG_ID" \
    --protocol tcp \
    --port "$PORT" \
    --cidr "${MY_IP}/32" >/dev/null
}

launch_spot_instance() {
  echo "Requesting EC2 Spot Instance ($INSTANCE_TYPE)..."
  local user_data_script="#!/bin/bash
nohup bash -c 'sleep 14400 && sudo poweroff' >/dev/null 2>&1 &"

  INSTANCE_JSON=$(aws ec2 run-instances \
    --image-id "$AMI_ID" \
    --instance-type "$INSTANCE_TYPE" \
    --key-name "$KEY_NAME" \
    --security-group-ids "$SG_ID" \
    --instance-market-options "MarketType=spot,SpotOptions={SpotInstanceType=one-time,InstanceInterruptionBehavior=terminate}" \
    --user-data "$user_data_script" \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=VMemKV-c6id-Bench},{Key=VMemKV-Temp-Spot,Value=$KEY_NAME}]" \
    --query "Instances[0]" \
    --output json)

  INSTANCE_ID=$(echo "$INSTANCE_JSON" | jq -r '.InstanceId')
  echo "Launched Spot Instance ID: $INSTANCE_ID"

  echo "Waiting for instance to start running..."
  aws ec2 wait instance-running --instance-ids "$INSTANCE_ID"

  PUBLIC_IP=$(aws ec2 describe-instances \
    --instance-ids "$INSTANCE_ID" \
    --query "Reservations[0].Instances[0].PublicIpAddress" \
    --output text)
  echo "Public IP Address: $PUBLIC_IP"
}

wait_for_ssh_ready() {
  echo "Waiting for SSH to become available..."
  SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $PEM_FILE -o ConnectTimeout=10"
  for i in {1..30}; do
    if ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "echo 'SSH Ready'" >/dev/null 2>&1; then
      echo "Connection established successfully."
      return 0
    fi
    sleep 4
  done
  echo "[ERROR] SSH did not become ready in time." >&2
  return 1
}

install_remote_dependencies() {
  echo "Preparing package locks & Installing runtime dependencies..."
  ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "
    for i in {1..30}; do
      if ! sudo fuser /var/lib/dpkg/lock-frontend /var/lib/apt/lists/lock /var/lib/dpkg/lock >/dev/null 2>&1; then
        break
      fi
      sleep 4
    done
    sudo apt-get update -y >/tmp/setup.log 2>&1 && sudo apt-get install -y build-essential cmake ninja-build libgoogle-perftools-dev librocksdb-dev jq rsync >>/tmp/setup.log 2>&1
  "
}

prepare_remote_storage() {
  echo "Locating and mounting local NVMe SSD..."
  ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "
    DEV=\$(lsblk -dno NAME | grep -E '^nvme[1-9]n1' | awk '{print \"/dev/\"\$1}' | head -n 1)
    if [ -z \"\$DEV\" ]; then
      echo \"[ERROR] Local NVMe SSD not found! A physical local NVMe is strictly required to run LTM / Swap benchmarks.\" >&2
      exit 1
    fi

    sudo mkdir -p /mnt/nvme
    MOUNT_TARGET=\$(findmnt -rn -S \"\$DEV\" -o TARGET 2>/dev/null | head -n 1 || true)
    if [ -z \"\$MOUNT_TARGET\" ]; then
      MOUNT_TARGET=\$(grep -E "^[^ ]*nvme[1-9]n1" /proc/mounts | awk '{print \$2}' | head -n 1 || true)
    fi

    if [ -n \"\$MOUNT_TARGET\" ]; then
      echo \"NVMe SSD \$DEV is already mounted at \$MOUNT_TARGET\"
      if [ \"\$MOUNT_TARGET\" != \"/mnt/nvme\" ]; then
        sudo mount --bind \"\$MOUNT_TARGET\" /mnt/nvme || true
        echo \"Bind-mounted \$MOUNT_TARGET to /mnt/nvme\"
      fi
    else
      if ! sudo blkid \"\$DEV\" >/dev/null 2>&1; then
        echo \"Formatting NVMe SSD \$DEV...\"
        sudo mkfs.ext4 -F \"\$DEV\" >/dev/null 2>&1
      else
        echo \"NVMe SSD \$DEV already has a filesystem; mounting without reformatting...\"
      fi
      sudo mount \"\$DEV\" /mnt/nvme || true
    fi
    if ! findmnt -rn /mnt/nvme >/dev/null 2>&1; then
      echo \"[ERROR] Failed to mount NVMe SSD to /mnt/nvme\" >&2
      exit 1
    fi
    sudo chown -R ubuntu:ubuntu /mnt/nvme
    echo \"Mounted NVMe SSD to /mnt/nvme\"

    SWAPFILE_SIZE_GB=\${AWS_SWAPFILE_SIZE_GB:-32}
    SWAPFILE_SIZE_BYTES=\$((SWAPFILE_SIZE_GB * 1024 * 1024 * 1024))
    AVAILABLE_BYTES=\$(df -B1 --output=avail /mnt/nvme | tail -n 1 | tr -d ' ')
    RESERVED_BYTES=\$((2 * 1024 * 1024 * 1024))
    MAX_SWAP_BYTES=\$((AVAILABLE_BYTES > RESERVED_BYTES ? AVAILABLE_BYTES - RESERVED_BYTES : 0))
    if [ \"\$MAX_SWAP_BYTES\" -le 0 ]; then
      echo \"[ERROR] NVMe filesystem does not have enough free space for a swapfile.\" >&2
      exit 1
    fi
    if [ \"\$SWAPFILE_SIZE_BYTES\" -gt \"\$MAX_SWAP_BYTES\" ]; then
      SWAPFILE_SIZE_BYTES=\"\$MAX_SWAP_BYTES\"
    fi
    echo \"Creating swap file of \$SWAPFILE_SIZE_BYTES bytes on NVMe SSD to prevent OOM Killer...\"
    sudo rm -f /mnt/nvme/swapfile
    sudo fallocate -l \"\$SWAPFILE_SIZE_BYTES\" /mnt/nvme/swapfile
    sudo chmod 600 /mnt/nvme/swapfile
    sudo mkswap /mnt/nvme/swapfile >/dev/null 2>&1
    sudo swapon /mnt/nvme/swapfile
    sudo sysctl -w vm.swappiness=100
    echo \"Swap file created and enabled successfully.\"
  "
}

sync_repo_to_remote() {
  echo "Syncing codebase..."
  git -C "$REPO_ROOT" ls-files | rsync -az -e "ssh $SSH_OPTS" --files-from=- "$REPO_ROOT/" "ubuntu@$PUBLIC_IP:~/faultkv/"
}

build_remote_benchmark() {
  echo "Building bench_kv on AWS instance..."
  ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "
    cd ~/faultkv/implementation &&
    cmake -S . -B build-rel -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -march=native' \
      -DENABLE_ROCKSDB=ON \
      -DENABLE_BENCHMARK=ON &&
    cmake --build build-rel --target bench_kv --clean-first -j\"\$(nproc)\"
  "
  echo "AWS build completed successfully."
}

create_ssh_key_pair
resolve_ubuntu_ami
create_security_group
launch_spot_instance
wait_for_ssh_ready
install_remote_dependencies
prepare_remote_storage
sync_repo_to_remote
build_remote_benchmark

echo "Counting benchmark instances..."

read -r -a SCENARIO_ORDER_KEYS <<<"$(vmemkv_matrix::scenario_order_keys_from_flag "$ORDER")"

SCENARIO_ORDER_LABEL="$(vmemkv_scenario_order_label "${SCENARIO_ORDER_KEYS[@]}")"
RUNNER_FLAGS="ltm_first=$([[ "$ORDER" == "B_FIRST" ]] && printf '1' || printf '0');large_value_first=$([[ "$LARGE_VALUE_FIRST" == "true" ]] && printf '1' || printf '0')"
if [[ "$QUICK" == "true" ]]; then
  RUNNER_FLAGS+=";quick=1"
fi

scenario_context_env_prefix() {
  local scenario_key="$1"
  local memory_budget_source
  local memory_budget_bytes
  local memo
  local memory_swap_max_bytes
  local swap_storage_media=""

  case "$scenario_key" in
    ltm)
      memory_budget_source="cgroup"
      memory_budget_bytes="$LTM_MEMORY_BUDGET_BYTES"
      memory_swap_max_bytes="$LTM_SWAP_BUDGET_BYTES"
      swap_storage_media="Local NVMe SSD"
      memo="$(vmemkv_benchmark_memo "aws_c6id" "ltm")"
      ;;
    in_memory)
      memory_budget_source="host"
      memory_budget_bytes=""
      memory_swap_max_bytes=""
      memo="In-memory runs execute without an enclosing cgroup."
      ;;
    *)
      return 1
      ;;
  esac

  vmemkv_context_env_prefix \
    "aws_c6id" \
    "Release" \
    "build-rel" \
    "ON" \
    "$SCENARIO_ORDER_LABEL" \
    "$RUNNER_FLAGS" \
    "$memory_budget_source" \
    "$memory_budget_bytes" \
    "$INSTANCE_TYPE" \
    "$AWS_REGION" \
    "$memo" \
    "" \
    "" \
    "$memory_swap_max_bytes" \
    "$swap_storage_media"
}

declare -A SCENARIO_TOTALS=(
  [in_memory]=0
  [ltm]=0
)

run_scenario() {
  local scenario_key="$1"
  local scenario_env_prefix
  local scenario_context_prefix
  local scenario_runtime_env_prefix=""
  local ycsb_populate_env_prefix=""
  local scenario_result_path
  local scenario_stdout_log
  local scenario_stderr_log
  local scenario_total
  local scenario_run_filter
  local scenario_ycsb_only_filter=""
  local remote_cmd
  local remote_cmd_quoted

  read -r -a scenario_value_order_keys <<<"$(vmemkv_matrix::scenario_value_order_keys_from_flag "$scenario_key" "$LARGE_VALUE_FIRST")"
  if [[ "${YCSB_ONLY:-0}" == "1" ]]; then
    if [[ "$scenario_key" == "in_memory" ]]; then
      scenario_ycsb_only_filter="$inmem_filter"
    else
      scenario_ycsb_only_filter="$ltm_filter"
    fi
    scenario_run_filter="$scenario_ycsb_only_filter"
  elif [[ -n "$VALUE_SIZE_LIMIT" ]]; then
    scenario_run_filter="$(vmemkv_matrix::benchmark_filter_for_case "$scenario_key" "${VALUE_SIZE_LIMIT,,}")"
  else
    scenario_run_filter="$(vmemkv_matrix::scenario_effective_filter "$scenario_key" "$LARGE_VALUE_FIRST" "$QUICK")"
  fi
  scenario_env_prefix="$(vmemkv_matrix::scenario_env_prefix "$scenario_key")"
  scenario_context_prefix="$(scenario_context_env_prefix "$scenario_key")"
  scenario_result_path="$(vmemkv_matrix::scenario_result_path "$scenario_key")"
  scenario_stdout_log="/tmp/vmemkv_${scenario_key}_${KEY_NAME}.stdout.log"
  scenario_stderr_log="/tmp/vmemkv_${scenario_key}_${KEY_NAME}.stderr.log"
  scenario_total="${SCENARIO_TOTALS[$scenario_key]}"

  : >"$scenario_stdout_log"
  : >"$scenario_stderr_log"

  if [[ "$scenario_key" == "in_memory" ]]; then
    scenario_runtime_env_prefix="VMEMKV_BENCH_FORCE_HOST_MEMORY=1"
  fi

  if [[ -n "${YCSB_E_POPULATE:-}" ]]; then
    ycsb_populate_env_prefix="YCSB_E_POPULATE=${YCSB_E_POPULATE}"
  fi



  local large_value_first_env=""
  if [[ "$LARGE_VALUE_FIRST" == "true" ]]; then
    large_value_first_env="VMEMKV_BENCH_LARGE_VALUE_FIRST=1"
  fi

  remote_cmd="
cd /home/ubuntu/faultkv/implementation &&
${scenario_context_prefix} ${scenario_runtime_env_prefix:+${scenario_runtime_env_prefix} }VMEMKV_DB_DIR=/mnt/nvme ${scenario_env_prefix:+${scenario_env_prefix} }${large_value_first_env:+${large_value_first_env} } \
${ycsb_populate_env_prefix:+${ycsb_populate_env_prefix} }\
./benchmark/common/aws_remote_runner.sh \
  '$scenario_run_filter' \
  '$MIN_TIME' \
  '$scenario_result_path'
  "
  printf -v remote_cmd_quoted '%q' "$remote_cmd"

  echo "Scenario $scenario_key stdout log: $scenario_stdout_log"
  echo "Scenario $scenario_key stderr log: $scenario_stderr_log"
  echo "[runner] start scenario=$scenario_key quick=$QUICK"

  set +e
  if [[ "$scenario_key" == "ltm" ]]; then
    {
      ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" \
        "sudo systemd-run --wait --pipe --quiet -p MemoryAccounting=yes -p MemoryHigh=${LTM_MEMORY_BUDGET_BYTES} -p MemoryMax=$((LTM_MEMORY_BUDGET_BYTES * 2)) -p MemorySwapMax=${LTM_SWAP_BUDGET_BYTES} -- bash -lc ${remote_cmd_quoted}" \
        2> >(tee -a "$scenario_stderr_log" >&2)
    } | tee -a "$scenario_stdout_log"
  else
    {
      ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" \
        "bash -lc ${remote_cmd_quoted}" \
        2> >(tee -a "$scenario_stderr_log" >&2)
    } | tee -a "$scenario_stdout_log"
  fi
  local ssh_status=${PIPESTATUS[0]}
  set -e
  echo "[runner] end scenario=$scenario_key status=$ssh_status"

  if [[ "$ssh_status" -ne 0 ]]; then
    echo "[ERROR] Scenario $scenario_key failed with exit code $ssh_status" >&2
    echo "[ERROR] Scenario $scenario_key stdout log: $scenario_stdout_log" >&2
    echo "[ERROR] Scenario $scenario_key stderr log: $scenario_stderr_log" >&2
    return "$ssh_status"
  fi
}



inmem_filter=""
ltm_filter=""
if [[ "${YCSB_ONLY:-0}" == "1" ]]; then
  inmem_filter="Store=(VMemKV|RocksDB)/Variant=(Baseline|Bloom-Simd-T1InlineValue-Prefaulting|RocksDB)/Op=YCSB-E/Dist=Zipf"
  ltm_filter="Store=(VMemKV|RocksDB)/Variant=(Baseline|Bloom-Simd-T1InlineValue-Prefaulting|RocksDB)/Op=YCSB-E/Dist=Zipf"
  MIN_TIME="30s"
elif [[ -n "$VALUE_SIZE_LIMIT" ]]; then
  inmem_filter="$(vmemkv_matrix::benchmark_filter_for_case in_memory "${VALUE_SIZE_LIMIT,,}")"
  ltm_filter="$(vmemkv_matrix::benchmark_filter_for_case ltm "${VALUE_SIZE_LIMIT,,}")"
else
  inmem_filter="$(vmemkv_matrix::scenario_effective_filter in_memory "$LARGE_VALUE_FIRST" "$QUICK")"
  ltm_filter="$(vmemkv_matrix::scenario_effective_filter ltm "$LARGE_VALUE_FIRST" "$QUICK")"
fi

echo "Counting remote benchmarks concurrently..."
if [[ "$SCENARIO_LIMIT" == "in_memory" || "$SCENARIO_LIMIT" == "all" ]]; then
  count_remote_benchmarks "$inmem_filter" "$(vmemkv_matrix::scenario_env_prefix in_memory)" "$([[ "$LARGE_VALUE_FIRST" == "true" ]] && printf 'VMEMKV_BENCH_LARGE_VALUE_FIRST=1')" > "/tmp/vmemkv_inmem_count_${KEY_NAME}.txt" &
  pid1=$!
else
  pid1=""
fi

if [[ "$SCENARIO_LIMIT" == "ltm" || "$SCENARIO_LIMIT" == "all" ]]; then
  count_remote_benchmarks "$ltm_filter" "$(vmemkv_matrix::scenario_env_prefix ltm)" "$([[ "$LARGE_VALUE_FIRST" == "true" ]] && printf 'VMEMKV_BENCH_LARGE_VALUE_FIRST=1')" > "/tmp/vmemkv_ltm_count_${KEY_NAME}.txt" &
  pid2=$!
else
  pid2=""
fi

if [[ -n "$pid1" ]]; then wait "$pid1"; fi
if [[ -n "$pid2" ]]; then wait "$pid2"; fi

total_inmem=0
total_ltm=0
if [[ "$SCENARIO_LIMIT" == "in_memory" || "$SCENARIO_LIMIT" == "all" ]]; then
  total_inmem=$(cat "/tmp/vmemkv_inmem_count_${KEY_NAME}.txt")
  rm -f "/tmp/vmemkv_inmem_count_${KEY_NAME}.txt"
fi
if [[ "$SCENARIO_LIMIT" == "ltm" || "$SCENARIO_LIMIT" == "all" ]]; then
  total_ltm=$(cat "/tmp/vmemkv_ltm_count_${KEY_NAME}.txt")
  rm -f "/tmp/vmemkv_ltm_count_${KEY_NAME}.txt"
fi

GLOBAL_TOTAL=$((total_inmem + total_ltm))
write_progress_state "$GLOBAL_TOTAL"
echo "Total benchmarks to run: $GLOBAL_TOTAL"

if [[ "$SCENARIO_LIMIT" == "in_memory" ]]; then
  run_scenario in_memory
elif [[ "$SCENARIO_LIMIT" == "ltm" ]]; then
  run_scenario ltm
else
  if [[ "$ORDER" == "B_FIRST" ]]; then
    run_scenario ltm
    run_scenario in_memory
  else
    run_scenario in_memory
    run_scenario ltm
  fi
fi

# ── Retrieve Results & Save ───────────────────────────────────────────────
echo "Downloading results..."
RESULTS_DIR="${REPO_ROOT}/implementation/benchmark/logs"
mkdir -p "$RESULTS_DIR"

if [[ "$SCENARIO_LIMIT" == "in_memory" || "$SCENARIO_LIMIT" == "all" ]]; then
  dst_name="results_in_memory.json"
  if [[ -n "$VALUE_SIZE_LIMIT" ]]; then
    dst_name="results_in_memory_${VALUE_SIZE_LIMIT}.json"
  fi
  scp $SSH_OPTS "ubuntu@$PUBLIC_IP:$(vmemkv_matrix::scenario_result_path in_memory)" "${RESULTS_DIR}/${dst_name}"
fi

if [[ "$SCENARIO_LIMIT" == "ltm" || "$SCENARIO_LIMIT" == "all" ]]; then
  dst_name="results_ltm.json"
  if [[ -n "$VALUE_SIZE_LIMIT" ]]; then
    dst_name="results_ltm_${VALUE_SIZE_LIMIT}.json"
  fi
  scp $SSH_OPTS "ubuntu@$PUBLIC_IP:$(vmemkv_matrix::scenario_result_path ltm)" "${RESULTS_DIR}/${dst_name}"
fi

# Retrieve YCSB-E timeline results if they exist
echo "Downloading YCSB-E timeline logs..."
scp $SSH_OPTS "ubuntu@$PUBLIC_IP:/tmp/ycsb_e_timeline_*.json" "${RESULTS_DIR}/" || true

echo "All tasks finished."
