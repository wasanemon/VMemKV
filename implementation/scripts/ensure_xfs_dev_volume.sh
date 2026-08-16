#!/usr/bin/env bash
# Ensures a reflink-capable (XFS, reflink=1) loopback filesystem is mounted for local dev/test use.
# VMemKV's checkpoint_and_defragment() requires reflink support (ioctl(FICLONE)) -- see TODO.md
# item 5 -- which ext4 does not provide, so on an ext4-rooted dev machine (e.g. WSL2) this script
# provisions a small loopback XFS volume instead of requiring the whole machine to run XFS.
#
# Idempotent: safe to run on every build. Exits 0 immediately if the target mount point is already
# a mounted XFS filesystem. The one-time mkfs/mount step needs root and an interactive password
# (this script cannot supply one non-interactively) -- run it yourself once in a real terminal, or
# grant NOPASSWD sudo for this exact script if you want CI/automation to call it unattended:
#   <username> ALL=(root) NOPASSWD: /path/to/ensure_xfs_dev_volume.sh
#
# Usage: ensure_xfs_dev_volume.sh [mount_point] [image_size]
#   mount_point default: /mnt/vmemkv-xfs
#   image_size  default: 20G
set -euo pipefail

MOUNT_POINT="${1:-/mnt/vmemkv-xfs}"
IMAGE_SIZE="${2:-20G}"
IMAGE_PATH="/var/lib/vmemkv-xfs-dev.img"

is_mounted_xfs() {
  mountpoint -q "$MOUNT_POINT" 2>/dev/null && \
    [[ "$(findmnt -no FSTYPE "$MOUNT_POINT" 2>/dev/null)" == "xfs" ]]
}

if is_mounted_xfs; then
  echo "[ensure_xfs_dev_volume] $MOUNT_POINT already mounted as XFS -- nothing to do."
  exit 0
fi

echo "[ensure_xfs_dev_volume] $MOUNT_POINT is not a mounted XFS filesystem. Provisioning..."

if ! command -v mkfs.xfs >/dev/null 2>&1; then
  echo "[ensure_xfs_dev_volume] installing xfsprogs..."
  sudo apt-get update -y
  sudo apt-get install -y xfsprogs
fi

if [[ ! -f "$IMAGE_PATH" ]]; then
  echo "[ensure_xfs_dev_volume] creating ${IMAGE_SIZE} backing image at $IMAGE_PATH..."
  sudo fallocate -l "$IMAGE_SIZE" "$IMAGE_PATH"
  sudo mkfs.xfs -m reflink=1 "$IMAGE_PATH"
fi

sudo mkdir -p "$MOUNT_POINT"
sudo mount -o loop "$IMAGE_PATH" "$MOUNT_POINT"
sudo chown "$(id -u):$(id -g)" "$MOUNT_POINT"

if ! is_mounted_xfs; then
  echo "[ensure_xfs_dev_volume] ERROR: mount succeeded but $MOUNT_POINT does not report as XFS." >&2
  exit 1
fi

echo "[ensure_xfs_dev_volume] $MOUNT_POINT ready (reflink=1)."
xfs_info "$MOUNT_POINT" | grep -i reflink || true
