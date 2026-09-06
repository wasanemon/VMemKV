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
  echo "  --scenario in_memory|ltm|all  Limit which scenario(s) run (default: all)"
  echo "  --value-size SIZE  Limit to one value-size combo within the active scenario(s)"
  echo "                   (e.g. 8B, 1KB, 64KB -- see common/benchmark_matrix.sh)"
  echo "  --background-jobs-probe  After the normal matrix, additionally measure"
  echo "                   reorganize()/checkpoint() (fixed 1KB/10,000,000-record corpus): each"
  echo "                   job's own duration plus the QPS degradation it causes to concurrent"
  echo "                   Insert/Update/Scan on the same instance via"
  echo "                   run_background_jobs_probe.sh and download its JSONL output. This is a"
  echo "                   forced, whole-store operation -- not what happens during ordinary"
  echo "                   operation; see --organic-split-probe for that."
  echo "  --organic-split-probe  After the normal matrix, additionally measure the Insert-QPS"
  echo "                   impact of ShardedT1Index's own automatic per-shard background"
  echo "                   splitting under sustained write load (the maintenance path that"
  echo "                   actually runs during ordinary operation) via"
  echo "                   run_organic_split_probe.sh and download its JSONL output."
  echo "  --skip-matrix    Skip the main Google Benchmark-registered CRUD/Scan/YCSB-E matrix"
  echo "                   entirely (and its results download) -- provision/build the instance"
  echo "                   and run only the *-probe flags passed alongside this one."
  echo "                   For a standalone probe-only run when the matrix has already been"
  echo "                   measured separately and only a probe (e.g. --background-jobs-probe)"
  echo "                   is still needed."
  echo "  --without-rivals  Drop RocksDB/RocksDB-BlobDB/LMDB from the benchmark filter, running"
  echo "                   VMemKV variants only. Use for regression-check runs after a"
  echo "                   VMemKV-internal-only code change, where the rivals' numbers are"
  echo "                   unaffected and re-measuring them is pure wasted AWS time/cost -- merge"
  echo "                   the prior full run's rival-only results back in afterward."
  exit 0
}

SCENARIO_LIMIT="all"
VALUE_SIZE_LIMIT=""
BACKGROUND_JOBS_PROBE=false
ORGANIC_SPLIT_PROBE=false
SKIP_MATRIX=false
WITHOUT_RIVALS=false

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
    --background-jobs-probe)
      BACKGROUND_JOBS_PROBE=true
      shift
      ;;
    --organic-split-probe)
      ORGANIC_SPLIT_PROBE=true
      shift
      ;;
    --skip-matrix)
      SKIP_MATRIX=true
      shift
      ;;
    --without-rivals)
      WITHOUT_RIVALS=true
      shift
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
# i4i: current-generation storage-optimized family (3rd-gen Nitro SSD local NVMe), switched from
# c6id.8xlarge on 2026-07-30 after c6id spot capacity in this region dried up (placement scores of
# 1/10 across all 3 AZs). i4i.8xlarge matches c6id.8xlarge's 32 vCPUs (keeps the existing
# 1/4/16/32-thread concurrency sweep unchanged) and is a more direct fit for this project's actual
# claim (how well OS-managed paging/swap performs against fast local NVMe) than a compute-
# optimized family where NVMe is a secondary feature. At switch time it was *also* both more
# available (placement score 3/10 vs 1/10, all 3 AZs) and cheaper on spot (~$0.60-0.64/hr vs
# ~$0.74-0.75/hr) than c6id.8xlarge, despite a higher on-demand list price ($3.22 vs $2.05/hr) --
# re-check both `get-spot-placement-scores` and `describe-spot-price-history` before assuming
# either holds, they shift over time.
INSTANCE_TYPE="i4i.8xlarge"
# Pin the spot request to whichever AZ currently has the best odds, rather than leaving subnet
# selection to run-instances' own (uncontrollable, for a single non-Fleet request) default.
# `aws ec2 get-spot-placement-scores --instance-types i4i.8xlarge --target-capacity 1
# --region-names ap-northeast-1 --single-availability-zone` scores all 3 AZs equally (3/10) as of
# 2026-08-17, so among AZs with equal interruption odds, pick by `describe-spot-price-history`
# instead: ap-northeast-1d ran ~5-15% cheaper than -1a and ~16% cheaper than -1c over the trailing
# 7 days (~$0.55-0.56/hr vs ~$0.58-0.65/hr vs ~$0.64-0.68/hr). Re-run both commands before assuming
# either still holds; they shift over time.
PREFERRED_AZ="ap-northeast-1d"
MIN_TIME="5.0s"
LTM_MEMORY_BUDGET_BYTES="$(vmemkv_matrix::ltm_memory_budget_bytes)"
LTM_SWAP_BUDGET_BYTES="$(vmemkv_matrix::ltm_swap_budget_bytes)"
KEY_NAME="vmemkv-i4i-key-$$"
PEM_FILE="/tmp/${KEY_NAME}.pem"
SG_NAME="vmemkv-i4i-sg-$$"
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
  ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "bash -lc 'cd ~/faultkv/vmemkv && ${env_prefix:+${env_prefix} }${runtime_prefix:+${runtime_prefix} }./build-rel/benchmark/bench_kv --benchmark_list_tests --benchmark_filter=\"${benchmark_filter}\" 2>/dev/null | awk \"NF { c++ } END { print c + 0 }\"'" | tail -n 1
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
      nohup "$SCRIPT_DIR/aws/aws_clean.sh" "$KEY_NAME" >"$cleanup_log" 2>&1 < /dev/null &
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

  echo "Locating default subnet in preferred AZ ($PREFERRED_AZ)..."
  SUBNET_ID=$(aws ec2 describe-subnets \
    --filters "Name=vpc-id,Values=$VPC_ID" "Name=availability-zone,Values=$PREFERRED_AZ" \
    --query "Subnets[0].SubnetId" \
    --output text)
  if [[ -z "$SUBNET_ID" || "$SUBNET_ID" == "None" ]]; then
    echo "[ERROR] No default subnet found in $PREFERRED_AZ (VPC $VPC_ID)." >&2
    exit 1
  fi
  echo "Using subnet $SUBNET_ID in $PREFERRED_AZ"
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
    --subnet-id "$SUBNET_ID" \
    --instance-market-options "MarketType=spot,SpotOptions={SpotInstanceType=one-time,InstanceInterruptionBehavior=terminate}" \
    --user-data "$user_data_script" \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=VMemKV-i4i-Bench},{Key=VMemKV-Temp-Spot,Value=$KEY_NAME}]" \
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
  # ServerAliveInterval/CountMax: the LTM priming step's ssh command blocks for many minutes
  # while the remote side silently builds large corpora on NVMe -- no output flows back over the
  # SSH channel for most of that time. Observed repeatedly: the local side of that connection
  # dies (and this script exits) after ~16-18 minutes regardless of whether the remote instance
  # or work is actually still healthy (confirmed via `aws ec2 describe-spot-instance-requests`
  # showing the instance was still "active"/"fulfilled" when this happened) -- consistent with a
  # NAT/firewall silently dropping a connection it sees as idle (WSL2's own NAT is a candidate),
  # not an AWS-side problem. Periodic keepalives keep the channel visibly active even when the
  # remote command itself has nothing to print for a while.
  SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $PEM_FILE -o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=6"
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
    sudo apt-get update -y >/tmp/setup.log 2>&1 && sudo apt-get install -y build-essential cmake ninja-build libgoogle-perftools-dev librocksdb-dev liblmdb-dev jq rsync >>/tmp/setup.log 2>&1
  "
}

prepare_remote_storage() {
  echo "Locating and mounting local NVMe SSD..."
  ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "
    # Excludes whichever nvme*n1 device actually backs / or /boot (checked by mountpoint, not by
    # assuming it's always nvme0n1) -- on i4i instances the root EBS volume and the local
    # instance-store NVMe can enumerate in either order, and a fixed index picked the root volume
    # on at least one observed launch, corrupting /boot via a bind-mount and failing later with
    # \"not enough free space for a swapfile\" once /boot's small filesystem filled up.
    ROOT_DEVS=\$(lsblk -no PKNAME,MOUNTPOINT | awk '\$2 == \"/\" || \$2 == \"/boot\" || \$2 == \"/boot/efi\" {print \$1}' | sort -u)
    DEV=\"\"
    for CAND in \$(lsblk -dno NAME | grep -E '^nvme[0-9]+n1'); do
      if ! echo \"\$ROOT_DEVS\" | grep -qx \"\$CAND\"; then
        DEV=\"/dev/\$CAND\"
        break
      fi
    done
    if [ -z \"\$DEV\" ]; then
      echo \"[ERROR] Local NVMe SSD not found! A physical local NVMe is strictly required to run LTM / Swap benchmarks.\" >&2
      exit 1
    fi

    sudo mkdir -p /mnt/nvme
    MOUNT_TARGET=\$(findmnt -rn -S \"\$DEV\" -o TARGET 2>/dev/null | head -n 1 || true)
    if [ -z \"\$MOUNT_TARGET\" ]; then
      MOUNT_TARGET=\$(grep -E \"^[^ ]*nvme[1-9]n1\" /proc/mounts | awk '{print \$2}' | head -n 1 || true)
    fi

    if [ -n \"\$MOUNT_TARGET\" ]; then
      echo \"NVMe SSD \$DEV is already mounted at \$MOUNT_TARGET\"
      if [ \"\$MOUNT_TARGET\" != \"/mnt/nvme\" ]; then
        sudo mount --bind \"\$MOUNT_TARGET\" /mnt/nvme || true
        echo \"Bind-mounted \$MOUNT_TARGET to /mnt/nvme\"
      fi
    else
      if ! sudo blkid \"\$DEV\" >/dev/null 2>&1; then
        echo \"Formatting NVMe SSD \$DEV as ext4...\"
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

    # Transparent Huge Pages: VMemKV's T2 is a large MAP_PRIVATE region that gets randomly
    # accessed and, under the LTM scenario, swapped -- exactly the pattern THP hurts most (COW
    # amplification to a full 2MB on any write, khugepaged scan/lock overhead, synchronous
    # compaction stalls under the memory pressure this scenario deliberately creates, and 2MB-
    # granularity swap I/O that drags cold data in with hot data). VMemKV never opts in via
    # MADV_HUGEPAGE anywhere, so there is no upside to THP being enabled for it; only ever
    # observed locally under WSL2 (which happens to default to madvise, i.e. this was never an
    # issue there) -- this AMI's actual default was unverified until now. 'madvise' rather than
    # 'never': functionally equivalent for VMemKV (which never advises for it) while remaining
    # the standard, less invasive system-wide choice.
    THP_ENABLED_PATH=/sys/kernel/mm/transparent_hugepage/enabled
    THP_DEFRAG_PATH=/sys/kernel/mm/transparent_hugepage/defrag
    if [ -f \"\$THP_ENABLED_PATH\" ]; then
      CURRENT_THP=\$(grep -o '\\[[a-z]*\\]' \"\$THP_ENABLED_PATH\" | tr -d '[]')
      echo \"Transparent Huge Pages: currently '\$CURRENT_THP' (raw: \$(cat \"\$THP_ENABLED_PATH\"))\"
      if [ \"\$CURRENT_THP\" != \"madvise\" ] && [ \"\$CURRENT_THP\" != \"never\" ]; then
        echo \"  -> not madvise/never; setting to madvise (VMemKV never opts in via MADV_HUGEPAGE, so this is effectively 'never' for it).\"
        echo madvise | sudo tee \"\$THP_ENABLED_PATH\" >/dev/null
      fi
      if [ -f \"\$THP_DEFRAG_PATH\" ]; then
        CURRENT_DEFRAG=\$(grep -o '\\[[a-z+]*\\]' \"\$THP_DEFRAG_PATH\" | tr -d '[]')
        if [ \"\$CURRENT_DEFRAG\" != \"madvise\" ] && [ \"\$CURRENT_DEFRAG\" != \"never\" ]; then
          echo madvise | sudo tee \"\$THP_DEFRAG_PATH\" >/dev/null
        fi
      fi
      echo \"Transparent Huge Pages: now '\$(grep -o '\\[[a-z]*\\]' \"\$THP_ENABLED_PATH\" | tr -d '[]')'\"
    else
      echo \"Transparent Huge Pages: sysfs knob not present on this kernel, skipping.\"
    fi
  "
}

sync_repo_to_remote() {
  echo "Syncing codebase..."
  git -C "$REPO_ROOT" ls-files | rsync -az -e "ssh $SSH_OPTS" --files-from=- "$REPO_ROOT/" "ubuntu@$PUBLIC_IP:~/faultkv/"
}

build_remote_benchmark() {
  echo "Building bench_kv on AWS instance..."
  ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "
    cd ~/faultkv/vmemkv &&
    cmake -S . -B build-rel -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -march=native' \
      -DENABLE_ROCKSDB=ON \
      -DENABLE_LMDB=ON \
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
      memo="$(vmemkv_benchmark_memo "aws_i4i" "ltm")"
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
    "aws_i4i" \
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
    scenario_run_filter="$(vmemkv_matrix::benchmark_filter_for_case "$scenario_key" "${VALUE_SIZE_LIMIT,,}" "$WITHOUT_RIVALS")"
  else
    scenario_run_filter="$(vmemkv_matrix::scenario_effective_filter "$scenario_key" "$LARGE_VALUE_FIRST" "$QUICK" "$WITHOUT_RIVALS")"
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

  local skip_cleanup_env_prefix=""
  if [[ "$scenario_key" == "ltm" ]]; then
    # Both the priming pass below and the cgroup-wrapped measurement pass after it run with
    # VMEMKV_BENCH_SKIP_CLEANUP=1 (see its comment in bench_kv.cpp), so bench_kv's own automatic
    # sweeps don't fight a deliberate multi-store, multi-master-generation priming pass. That
    # means this script owns the "start from a clean slate" step neither of those invocations
    # will do on their own -- do it once, here, unconditionally, exactly like bench_kv's own
    # startup sweep normally would (same "bench_" prefix, same directory: /mnt/nvme).
    ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "find /mnt/nvme -maxdepth 1 -name 'bench_*' -exec rm -rf {} +" \
      >>"$scenario_stdout_log" 2>>"$scenario_stderr_log"

    # Prime every shared master corpus this run will need at full, unconstrained NVMe speed
    # *before* entering the cgroup below. Without this, a real per-master populate under this
    # exact cgroup was measured locally at 200-1200s (vs. ~20-30s unconstrained) -- paying that
    # cost here means the cgroup-wrapped measurement pass only ever needs to clone, not rebuild,
    # those masters. See vmemkv_matrix::ltm_priming_filter()'s comment: one Get/Hit/Zipf cell per
    # (store, value size) is enough, since Scan now clones from that same shared master too (no
    # separate Scan-only master to prime).
    local priming_filter priming_cmd priming_cmd_quoted
    priming_filter="$(vmemkv_matrix::ltm_priming_filter "$WITHOUT_RIVALS")"
    # VMEMKV_CONTEXT_memory_budget_bytes must be set here too, not just on the measurement pass
    # below (scenario_context_prefix) -- bench_kv's detect_machine_memory_bytes() falls back to
    # the host's raw /proc/meminfo MemTotal whenever this is unset, and priming runs unconstrained
    # (no cgroup, so no memory.max to fall back to either). Without it, the "small declared budget
    # x large target_ratio" corpus-sizing scheme (see vmemkv_matrix::ltm_memory_budget_bytes()'s
    # comment: 1GB budget x 8.0 ratio should be an ~8GB corpus) instead sizes against this
    # instance's full ~62GB RAM, producing a ~530GB corpus -- large enough to reliably OOM-kill
    # bench_kv partway through populate. Observed directly: every LTM priming attempt without this
    # fix died to the OOM killer, never a script or SSH problem.
    priming_cmd="
cd /home/ubuntu/faultkv/vmemkv &&
VMEMKV_CONTEXT_memory_budget_bytes=$LTM_MEMORY_BUDGET_BYTES ${scenario_runtime_env_prefix:+${scenario_runtime_env_prefix} }VMEMKV_DB_DIR=/mnt/nvme ${scenario_env_prefix:+${scenario_env_prefix} } VMEMKV_BENCH_SKIP_CLEANUP=1 \
./benchmark/common/run_scenario.sh \
  './build-rel/benchmark/bench_kv' \
  '$priming_filter' \
  '$MIN_TIME' \
  '/mnt/nvme/ltm_priming_discard.json'
    "
    printf -v priming_cmd_quoted '%q' "$priming_cmd"
    echo "[runner] priming LTM master corpora (unconstrained) for scenario=$scenario_key ..."
    ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "bash -lc ${priming_cmd_quoted}" \
      >>"$scenario_stdout_log" 2>>"$scenario_stderr_log"
    echo "[runner] priming complete for scenario=$scenario_key"

    # The measurement pass below reuses the masters priming just built -- it must not let
    # bench_kv's own startup cleanup_stale_benchmark_files() wipe them the instant it starts.
    skip_cleanup_env_prefix="VMEMKV_BENCH_SKIP_CLEANUP=1"
  fi

  remote_cmd="
cd /home/ubuntu/faultkv/vmemkv &&
${scenario_context_prefix} ${scenario_runtime_env_prefix:+${scenario_runtime_env_prefix} }VMEMKV_DB_DIR=/mnt/nvme ${scenario_env_prefix:+${scenario_env_prefix} }${large_value_first_env:+${large_value_first_env} }${skip_cleanup_env_prefix:+${skip_cleanup_env_prefix} } \
${ycsb_populate_env_prefix:+${ycsb_populate_env_prefix} }\
./benchmark/common/run_scenario.sh \
  './build-rel/benchmark/bench_kv' \
  '$scenario_run_filter' \
  '$MIN_TIME' \
  '$scenario_result_path'
  "
  printf -v remote_cmd_quoted '%q' "$remote_cmd"

  echo "Scenario $scenario_key stdout log: $scenario_stdout_log"
  echo "Scenario $scenario_key stderr log: $scenario_stderr_log"
  echo "[runner] start scenario=$scenario_key quick=$QUICK"

  local ssh_status
  set +e
  if [[ "$scenario_key" == "ltm" ]]; then
    # Each (Store, Variant) identity gets drop_caches + its OWN systemd-run cgroup scope for the
    # measurement pass below, instead of one shared scope for the whole scenario_run_filter.
    # Sharing one cgroup scope across multiple stores/variants was found to make whichever one is
    # measured LATER in that scope's lifetime look artificially faster than it should -- the
    # cgroup's own memory-reclaim heuristics seem to "warm up" over the scope's life in a way that
    # carries across different stores' corpus files, not just within one store's own corpus. A
    # drop_caches right before entering the cgroup (kept below, and necessary regardless -- see the
    # RocksDB Checkpoint-hardlink note in that step) does NOT fix this by itself: it only clears
    # page cache, not the cgroup's reclaim-heuristic state, which develops fresh after page-cache
    # tenants are entered. Confirmed empirically: a 1KB/Zipf/threads:32 comparison that showed a
    # new ablation LOSING to RocksDB/LMDB when all three were measured in one shared scope showed
    # it WINNING once each got its own isolated scope. Costs more wall-clock (each identity
    # re-clones its own corpus from the already-primed master, on top of paying its own
    # drop_caches), but is the only way to get trustworthy cross-store relative numbers here.
    # Discover the exact benchmark names scenario_run_filter matches, then group them by
    # (Store, Variant) identity and rebuild each group's filter as an anchored alternation of its
    # own exact names -- deliberately NOT a re-derived Value=/Op= regex, so this stays correct
    # under every shape scenario_run_filter can take above (plain (scenario,value), --quick,
    # --scenario/--value-size limits, and YCSB_ONLY, which has no Value= constraint at all).
    echo "[runner] discovering per-store isolation groups for scenario=$scenario_key ..."
    local all_bench_names
    all_bench_names=$(ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" \
      "bash -lc 'cd ~/faultkv/vmemkv && ${scenario_env_prefix:+${scenario_env_prefix} }./build-rel/benchmark/bench_kv --benchmark_list_tests --benchmark_filter=\"${scenario_run_filter}\" 2>/dev/null'")

    local -a identities=()
    while IFS= read -r identity_line; do
      [[ -z "$identity_line" ]] && continue
      identities+=("$identity_line")
    done < <(printf '%s\n' "$all_bench_names" | sed -nE 's#^(Store=[^/]+/Variant=[^/]+)/.*#\1#p' | sort -u)

    if [[ "${#identities[@]}" -eq 0 ]]; then
      echo "[ERROR] No benchmarks matched scenario=$scenario_key filter -- nothing to isolate/run." >&2
      ssh_status=1
    else
      echo "[runner] ${#identities[@]} isolation groups: ${identities[*]}"

      local -a group_result_paths=()
      local group_idx=0 identity group_names group_filter group_result_path group_remote_cmd group_remote_cmd_quoted group_status
      ssh_status=0
      for identity in "${identities[@]}"; do
        group_idx=$((group_idx + 1))
        group_names="$(printf '%s\n' "$all_bench_names" | grep -F "${identity}/" | sed -E 's/[][(){}.^$*+?\\|]/\\&/g' | paste -sd'|' -)"
        group_filter="^(${group_names})\$"
        group_result_path="/tmp/vmemkv_ltm_group_${group_idx}_${KEY_NAME}.json"
        group_result_paths+=("$group_result_path")

        echo "[runner] LTM isolated pass ${group_idx}/${#identities[@]}: ${identity}"
        # See the comment above this loop for why this is per-identity, not once for the whole
        # scenario: RocksDB's Checkpoint-based clone hardlinks SST/blob files instead of copying
        # them, and cgroup v2 charges page-cache pages to whichever cgroup first faulted them in --
        # so without a fresh drop_caches here, a cgroup-constrained pass could read the SAME
        # already-warm pages an earlier pass (or the unconstrained priming pass) just wrote, with
        # no new charge against MemoryHigh, meaning RocksDB never experiences the intended 8x
        # memory oversubscription at all (confirmed directly:
        # implementation/docs/benchmark/20260805_ltm_get_hit_profiling.md).
        ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "sync && echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null" \
          >>"$scenario_stdout_log" 2>>"$scenario_stderr_log"

        group_remote_cmd="
cd /home/ubuntu/faultkv/vmemkv &&
${scenario_context_prefix} ${scenario_runtime_env_prefix:+${scenario_runtime_env_prefix} }VMEMKV_DB_DIR=/mnt/nvme ${scenario_env_prefix:+${scenario_env_prefix} }${large_value_first_env:+${large_value_first_env} }VMEMKV_BENCH_SKIP_CLEANUP=1 \
${ycsb_populate_env_prefix:+${ycsb_populate_env_prefix} }\
./benchmark/common/run_scenario.sh \
  './build-rel/benchmark/bench_kv' \
  '$group_filter' \
  '$MIN_TIME' \
  '$group_result_path'
        "
        printf -v group_remote_cmd_quoted '%q' "$group_remote_cmd"

        {
          ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" \
            "sudo systemd-run --wait --pipe --quiet -p MemoryAccounting=yes -p MemoryHigh=${LTM_MEMORY_BUDGET_BYTES} -p MemoryMax=$((LTM_MEMORY_BUDGET_BYTES * 2)) -p MemorySwapMax=${LTM_SWAP_BUDGET_BYTES} -- bash -lc ${group_remote_cmd_quoted}" \
            2> >(tee -a "$scenario_stderr_log" >&2)
        } | tee -a "$scenario_stdout_log"
        group_status=${PIPESTATUS[0]}
        if [[ "$group_status" -ne 0 ]]; then
          echo "[ERROR] LTM isolated pass for ${identity} failed with exit code $group_status" >&2
          ssh_status="$group_status"
          break
        fi
      done

      if [[ "$ssh_status" -eq 0 ]]; then
        # Merge every isolated pass's standalone Google Benchmark JSON into one document at
        # scenario_result_path, exactly as if it had all come from a single run: same "context"
        # (taken from the first pass), "benchmarks" arrays concatenated in isolation-pass order.
        local merge_paths_quoted="" p
        for p in "${group_result_paths[@]}"; do
          merge_paths_quoted+=" $(printf '%q' "$p")"
        done
        ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" \
          "jq -s '{context: .[0].context, benchmarks: (map(.benchmarks) | add)}'${merge_paths_quoted} > $(printf '%q' "$scenario_result_path")" \
          >>"$scenario_stdout_log" 2>>"$scenario_stderr_log"
        ssh_status=$?
      fi
    fi
  else
    {
      ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" \
        "bash -lc ${remote_cmd_quoted}" \
        2> >(tee -a "$scenario_stderr_log" >&2)
    } | tee -a "$scenario_stdout_log"
    ssh_status=${PIPESTATUS[0]}
  fi
  set -e
  echo "[runner] end scenario=$scenario_key status=$ssh_status"

  if [[ "$ssh_status" -ne 0 ]]; then
    echo "[ERROR] Scenario $scenario_key failed with exit code $ssh_status" >&2
    echo "[ERROR] Scenario $scenario_key stdout log: $scenario_stdout_log" >&2
    echo "[ERROR] Scenario $scenario_key stderr log: $scenario_stderr_log" >&2
    return "$ssh_status"
  fi
}

# Runs one "additive extra measurement" probe script against the already-provisioned instance.
# BACKGROUND_JOBS_PROBE below uses this orchestration -- up to two invocations (in_memory
# unconstrained, ltm systemd-run-wrapped with the same MemoryHigh/MemoryMax/MemorySwapMax as
# run_scenario()'s own ltm wrap), respecting SCENARIO_LIMIT/VALUE_SIZE_LIMIT exactly like the
# matrix above.
#
# Args: $1 = probe script name (e.g. run_background_jobs_probe.sh)
#       $2 = output basename (e.g. background_jobs -> background_jobs_in_memory.jsonl)
#       $3 = log-file prefix (e.g. vmemkv_background_jobs_probe)
#       $4 = human-readable label for [runner]/[WARN] messages (e.g. background-jobs-probe)
#
# A failure here is logged as [WARN], not [ERROR], and does not fail the whole run: unlike the
# matrix above, this is a supplementary measurement, not the main deliverable.
run_remote_probe() {
  local probe_script="$1"
  local output_basename="$2"
  local log_prefix="$3"
  local label="$4"

  local inmem_dst_name="${output_basename}_in_memory.jsonl"
  local ltm_dst_name="${output_basename}_ltm.jsonl"
  if [[ -n "$VALUE_SIZE_LIMIT" ]]; then
    inmem_dst_name="${output_basename}_in_memory_${VALUE_SIZE_LIMIT}.jsonl"
    ltm_dst_name="${output_basename}_ltm_${VALUE_SIZE_LIMIT}.jsonl"
  fi

  local probe_failed=0

  if [[ "$SCENARIO_LIMIT" == "in_memory" || "$SCENARIO_LIMIT" == "all" ]]; then
    local inmem_combo_filter="in_memory"
    if [[ -n "$VALUE_SIZE_LIMIT" ]]; then
      inmem_combo_filter="in_memory:${VALUE_SIZE_LIMIT}"
    fi
    local inmem_stdout_log="/tmp/${log_prefix}_inmem_${KEY_NAME}.stdout.log"
    local inmem_stderr_log="/tmp/${log_prefix}_inmem_${KEY_NAME}.stderr.log"
    : >"$inmem_stdout_log"
    : >"$inmem_stderr_log"
    local inmem_remote_cmd="
cd /home/ubuntu/faultkv/vmemkv &&
./benchmark/${probe_script} './build-rel/benchmark/bench_kv' '/mnt/nvme/${output_basename}_in_memory.jsonl' '/mnt/nvme' '$inmem_combo_filter'
    "
    local inmem_remote_cmd_quoted
    printf -v inmem_remote_cmd_quoted '%q' "$inmem_remote_cmd"
    echo "[runner] start ${label} scenario=in_memory combo_filter=$inmem_combo_filter"
    set +e
    { ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" "bash -lc ${inmem_remote_cmd_quoted}" \
        2> >(tee -a "$inmem_stderr_log" >&2); } | tee -a "$inmem_stdout_log"
    local inmem_status=${PIPESTATUS[0]}
    set -e
    echo "[runner] end ${label} scenario=in_memory status=$inmem_status"
    if [[ "$inmem_status" -ne 0 ]]; then
      # [WARN], deliberately not [ERROR]: this run's own on-error logging (and any external
      # watcher pattern-matching "[ERROR]" to decide the whole run failed, see this session's
      # AWS-monitoring convention) must not treat a supplementary-measurement hiccup as fatal
      # when the main benchmark matrix already succeeded.
      echo "[WARN] ${label} (in_memory) failed with exit code $inmem_status -- logs: $inmem_stdout_log $inmem_stderr_log" >&2
      probe_failed=1
    fi
    scp $SSH_OPTS "ubuntu@$PUBLIC_IP:/mnt/nvme/${output_basename}_in_memory.jsonl" "${RESULTS_DIR}/${inmem_dst_name}" || true
  fi

  if [[ "$SCENARIO_LIMIT" == "ltm" || "$SCENARIO_LIMIT" == "all" ]]; then
    local ltm_combo_filter="ltm"
    if [[ -n "$VALUE_SIZE_LIMIT" ]]; then
      ltm_combo_filter="ltm:${VALUE_SIZE_LIMIT}"
    fi
    local ltm_stdout_log="/tmp/${log_prefix}_ltm_${KEY_NAME}.stdout.log"
    local ltm_stderr_log="/tmp/${log_prefix}_ltm_${KEY_NAME}.stderr.log"
    : >"$ltm_stdout_log"
    : >"$ltm_stderr_log"
    # VMEMKV_CONTEXT_memory_budget_bytes must be set explicitly here, not left to
    # detect_machine_memory_bytes()'s cgroup-file fallback: that would read this systemd-run scope's
    # MemoryMax (2x LTM_MEMORY_BUDGET_BYTES, see below), not the declared budget itself, mis-sizing
    # every corpus by 2x -- same reasoning as run_scenario()'s priming/measurement passes above.
    local ltm_remote_cmd="
cd /home/ubuntu/faultkv/vmemkv &&
VMEMKV_CONTEXT_memory_budget_bytes=$LTM_MEMORY_BUDGET_BYTES \
./benchmark/${probe_script} './build-rel/benchmark/bench_kv' '/mnt/nvme/${output_basename}_ltm.jsonl' '/mnt/nvme' '$ltm_combo_filter'
    "
    local ltm_remote_cmd_quoted
    printf -v ltm_remote_cmd_quoted '%q' "$ltm_remote_cmd"
    echo "[runner] start ${label} scenario=ltm combo_filter=$ltm_combo_filter"
    set +e
    { ssh $SSH_OPTS "ubuntu@$PUBLIC_IP" \
        "sudo systemd-run --wait --pipe --quiet -p MemoryAccounting=yes -p MemoryHigh=${LTM_MEMORY_BUDGET_BYTES} -p MemoryMax=$((LTM_MEMORY_BUDGET_BYTES * 2)) -p MemorySwapMax=${LTM_SWAP_BUDGET_BYTES} -- bash -lc ${ltm_remote_cmd_quoted}" \
        2> >(tee -a "$ltm_stderr_log" >&2); } | tee -a "$ltm_stdout_log"
    local ltm_status=${PIPESTATUS[0]}
    set -e
    echo "[runner] end ${label} scenario=ltm status=$ltm_status"
    if [[ "$ltm_status" -ne 0 ]]; then
      # [WARN], not [ERROR] -- same reasoning as the in_memory branch above.
      echo "[WARN] ${label} (ltm) failed with exit code $ltm_status -- logs: $ltm_stdout_log $ltm_stderr_log" >&2
      probe_failed=1
    fi
    scp $SSH_OPTS "ubuntu@$PUBLIC_IP:/mnt/nvme/${output_basename}_ltm.jsonl" "${RESULTS_DIR}/${ltm_dst_name}" || true
  fi

  if [[ "$probe_failed" -ne 0 ]]; then
    echo "[WARN] ${label} had failures -- main benchmark matrix results above are still valid" >&2
  fi
}


inmem_filter="${inmem_filter:-}"
ltm_filter="${ltm_filter:-}"
if [[ "${YCSB_ONLY:-0}" == "1" ]]; then
  inmem_filter="${inmem_filter:-Store=(VMemKV|RocksDB)/Variant=(Baseline|Bloom-T1InlineValue|RocksDB)/Op=YCSB-E/Dist=Zipf}"
  ltm_filter="${ltm_filter:-Store=(VMemKV|RocksDB)/Variant=(Baseline|Bloom-T1InlineValue|RocksDB)/Op=YCSB-E/Dist=Zipf}"
  MIN_TIME="${MIN_TIME:-30s}"
elif [[ -n "$VALUE_SIZE_LIMIT" ]]; then
  inmem_filter="${inmem_filter:-$(vmemkv_matrix::benchmark_filter_for_case in_memory "${VALUE_SIZE_LIMIT,,}" "$WITHOUT_RIVALS")}"
  ltm_filter="${ltm_filter:-$(vmemkv_matrix::benchmark_filter_for_case ltm "${VALUE_SIZE_LIMIT,,}" "$WITHOUT_RIVALS")}"
else
  inmem_filter="${inmem_filter:-$(vmemkv_matrix::scenario_effective_filter in_memory "$LARGE_VALUE_FIRST" "$QUICK" "$WITHOUT_RIVALS")}"
  ltm_filter="${ltm_filter:-$(vmemkv_matrix::scenario_effective_filter ltm "$LARGE_VALUE_FIRST" "$QUICK" "$WITHOUT_RIVALS")}"
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

if [[ "$SKIP_MATRIX" != "true" ]]; then
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
fi

# ── Retrieve Results & Save ───────────────────────────────────────────────
RESULTS_DIR="${SCRIPT_DIR}/logs"
mkdir -p "$RESULTS_DIR"

if [[ "$SKIP_MATRIX" != "true" ]]; then
  echo "Downloading results..."

  # With --without-rivals, the downloaded JSON only covers VMemKV variants; tag the filename so it
  # is never mistaken for (or silently overwritten by/onto) a full-matrix result, and so a later
  # merge step (see vmemkv_matrix::scenario_filter()'s comment re: merge_vmemkv_only_results.py) can
  # find both halves unambiguously.
  without_rivals_suffix=""
  if [[ "$WITHOUT_RIVALS" == "true" ]]; then
    without_rivals_suffix="_vmemkv_only"
  fi

  if [[ "$SCENARIO_LIMIT" == "in_memory" || "$SCENARIO_LIMIT" == "all" ]]; then
    dst_name="results_in_memory${without_rivals_suffix}.json"
    if [[ -n "$VALUE_SIZE_LIMIT" ]]; then
      dst_name="results_in_memory_${VALUE_SIZE_LIMIT}${without_rivals_suffix}.json"
    fi
    scp $SSH_OPTS "ubuntu@$PUBLIC_IP:$(vmemkv_matrix::scenario_result_path in_memory)" "${RESULTS_DIR}/${dst_name}"
  fi

  if [[ "$SCENARIO_LIMIT" == "ltm" || "$SCENARIO_LIMIT" == "all" ]]; then
    dst_name="results_ltm${without_rivals_suffix}.json"
    if [[ -n "$VALUE_SIZE_LIMIT" ]]; then
      dst_name="results_ltm_${VALUE_SIZE_LIMIT}${without_rivals_suffix}.json"
    fi
    scp $SSH_OPTS "ubuntu@$PUBLIC_IP:$(vmemkv_matrix::scenario_result_path ltm)" "${RESULTS_DIR}/${dst_name}"
  fi

  # Retrieve YCSB-E timeline results if they exist
  echo "Downloading YCSB-E timeline logs..."
  scp $SSH_OPTS "ubuntu@$PUBLIC_IP:/tmp/ycsb_e_timeline_*.json" "${RESULTS_DIR}/" || true
fi

if [[ "$BACKGROUND_JOBS_PROBE" == "true" ]]; then
  # Additive extra measurement (reorganize()/checkpoint() own duration + Insert/Update/Scan QPS
  # degradation while each runs, fixed 1KB/10,000,000-record corpus) on top of the normal matrix
  # just run above -- not part of the Google Benchmark-registered matrix itself, see
  # run_background_jobs_probe.sh's own comment for why. Supersedes the three retired probes
  # (reorg-scaling, checkpoint-throughput, maintenance-contention). Up to two invocations, exactly
  # mirroring run_scenario()'s ltm-only cgroup wrap: unconstrained for in_memory, systemd-run-
  # wrapped (same MemoryHigh/MemoryMax/MemorySwapMax as the real ltm scenario) for ltm -- mixing
  # both under one wrap would starve in_memory of memory it was never meant to be constrained by.
  # Ignores VALUE_SIZE_LIMIT (this probe's corpus is always 1KB, fixed) -- run_5parallel_bench.sh's
  # dedicated 5th instance passes no --value-size, so this always covers both scenarios here.
  # A failure here is logged but does not fail the whole run -- unlike the matrix above, this is a
  # supplementary measurement, not the main deliverable.
  run_remote_probe "run_background_jobs_probe.sh" "background_jobs" "vmemkv_background_jobs_probe" "background-jobs-probe"
fi

if [[ "$ORGANIC_SPLIT_PROBE" == "true" ]]; then
  # Additive extra measurement (Insert-QPS impact of ShardedT1Index's own automatic per-shard
  # background splitting under sustained write load) on top of the normal matrix just run above --
  # complements background-jobs-probe above (which measures a forced, whole-store call, not what
  # happens during ordinary operation) rather than replacing it. Same ltm-cgroup-wrap/
  # VALUE_SIZE_LIMIT-ignoring/non-fatal-failure conventions as that call, see its own comment.
  run_remote_probe "run_organic_split_probe.sh" "organic_split" "vmemkv_organic_split_probe" "organic-split-probe"
fi
