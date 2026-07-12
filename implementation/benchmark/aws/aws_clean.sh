#!/usr/bin/env bash
# aws_clean.sh - Clean up all temporary AWS resources created by vmemkv benchmark scripts

set -euo pipefail

echo "==========================================================="
echo "   Cleaning up VMemKV Temporary AWS Benchmark Resources"
echo "==========================================================="

# 1. Terminate EC2 instances associated with the benchmark keypair
KEY_PREFIX="vmemkv-c6id-key-"
INSTANCES=$(aws ec2 describe-instances \
  --filters "Name=key-name,Values=${KEY_PREFIX}*" "Name=instance-state-name,Values=pending,running,stopping,stopped" \
  --query "Reservations[*].Instances[*].InstanceId" \
  --output text)

if [ -n "$INSTANCES" ]; then
  echo "Found running benchmark instances: $INSTANCES"
  echo "Terminating instances..."
  aws ec2 terminate-instances --instance-ids $INSTANCES > /dev/null
  echo "Waiting for instances to be terminated..."
  aws ec2 wait instance-terminated --instance-ids $INSTANCES
  echo "Instances terminated successfully."
else
  echo "No active benchmark EC2 instances found."
fi

# 2. Delete temporary security groups
SGS=$(aws ec2 describe-security-groups \
  --filters "Name=group-name,Values=vmemkv-c6id-sg-*" \
  --query "SecurityGroups[*].GroupId" \
  --output text)

if [ -n "$SGS" ]; then
  echo "Found temporary security groups: $SGS"
  for sg in $SGS; do
    echo "Deleting security group $sg..."
    # Retry a few times in case dependencies (network interfaces) take a moment to clear
    for i in {1..5}; do
      if aws ec2 delete-security-group --group-id "$sg" 2>/dev/null; then
        echo "Deleted security group $sg."
        break
      fi
      echo "Waiting for security group dependencies to clear (retry $i/5)..."
      sleep 3
    done
  done
else
  echo "No temporary security groups found."
fi

# 3. Delete key pairs
KEY_PAIRS=$(aws ec2 describe-key-pairs \
  --query "KeyPairs[?starts_with(KeyName, '${KEY_PREFIX}')].KeyName" \
  --output text)

if [ -n "$KEY_PAIRS" ]; then
  echo "Found temporary key pairs: $KEY_PAIRS"
  for kp in $KEY_PAIRS; do
    echo "Deleting AWS Key Pair $kp..."
    aws ec2 delete-key-pair --key-name "$kp"
  done
else
  echo "No temporary AWS key pairs found."
fi

# 4. Clean up local key files in /tmp
echo "Cleaning up local temporary key files..."
rm -f /tmp/vmemkv-c6id-key-*.pem

echo "==========================================================="
echo "   AWS Clean Up Completed Successfully!"
echo "==========================================================="
