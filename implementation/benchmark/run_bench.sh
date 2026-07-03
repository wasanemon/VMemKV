#!/usr/bin/env bash
# run_bench.sh — VMemKV ベンチマーク実行スクリプト
#
# bench_kv が実行するベンチマーク:
#   シングルスレッド (nanobench): INSERT / GET(hit,miss) / UPDATE(inplace,append)
#                                 DELETE / SCAN / SCAN(reorg)
#   マルチスレッド (カスタム):    GET, INSERT, MIXED(80R/20W)
#                                 スレッド数: 1, 4, hw_concurrency
#
# 使い方:
#   cd <repo>/VMemKV
#   bash benchmark/run_bench.sh [--no-rocksdb] [--build-type Release]
#
# オプション:
#   --no-rocksdb      RocksDB を無効にしてビルド（未インストール環境向け）
#   --build-type <T>  CMake ビルドタイプ（デフォルト: Release）
#   --build-dir <D>   ビルドディレクトリ（デフォルト: build）
#   --iters <N>       ST基準反復回数（MTは N x thread_count）（デフォルト: 10000）
#   --quick           `--iters 2000` の省略形
#   --output <F>      結果をファイルにも保存（例: results/$(date +%Y%m%d).txt）
#   --no-log          benchmark/logs/ への自動保存を無効化

set -euo pipefail

# ── デフォルト値 ────────────────────────────────────────────────────────────
ENABLE_ROCKSDB=ON
BUILD_TYPE=Release
BUILD_DIR=build
OUTPUT_FILE=""
AUTO_LOG=true
ITERS=10000
QUICK=false

# ── 引数パース ──────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-rocksdb)   ENABLE_ROCKSDB=OFF; shift ;;
    --build-type)   BUILD_TYPE="$2";    shift 2 ;;
    --build-dir)    BUILD_DIR="$2";     shift 2 ;;
    --iters)        ITERS="$2";         shift 2 ;;
    --quick)        ITERS=2000; QUICK=true; shift ;;
    --output)       OUTPUT_FILE="$2";   shift 2 ;;
    --no-log)       AUTO_LOG=false;     shift ;;
    -h|--help)
      sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

# ── スクリプトの場所に関わらず VMemKV ルートに移動 ──────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ── 自動ログファイルパスの決定 ──────────────────────────────────────────────
LOG_DIR="$SCRIPT_DIR/logs"
if [[ "$AUTO_LOG" == "true" ]]; then
  TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
  ROCKSDB_SUFFIX="$([ "$ENABLE_ROCKSDB" == "ON" ] && echo "rocksdb" || echo "no_rocksdb")"
  AUTO_LOG_FILE="$LOG_DIR/${TIMESTAMP}_${ROCKSDB_SUFFIX}.txt"
  mkdir -p "$LOG_DIR"
fi

echo "=== VMemKV Benchmark ==="
echo "  Repo root   : $REPO_ROOT"
echo "  Build dir   : $BUILD_DIR"
echo "  Build type  : $BUILD_TYPE"
echo "  RocksDB     : $ENABLE_ROCKSDB"
echo "  Iterations  : ${ITERS} (ST base, MT scales by thread count)"
[[ -n "$OUTPUT_FILE" ]] && echo "  Output file : $OUTPUT_FILE"
[[ "$AUTO_LOG" == "true" ]] && echo "  Log file    : $AUTO_LOG_FILE"
echo ""

# ── RocksDB の存在確認（ON 指定時のみ） ────────────────────────────────────
if [[ "$ENABLE_ROCKSDB" == "ON" ]]; then
  if ! (pkg-config --exists rocksdb 2>/dev/null || \
        [[ -f /opt/homebrew/lib/librocksdb.dylib ]] || \
        [[ -f /usr/local/lib/librocksdb.so ]] || \
        [[ -f /usr/lib/librocksdb.so ]]); then
    echo "Warning: RocksDB not found. Falling back to --no-rocksdb." >&2
    echo "  Install with: brew install rocksdb  (macOS)"
    echo "                apt install librocksdb-dev  (Debian/Ubuntu)"
    echo ""
    ENABLE_ROCKSDB=OFF
  fi
fi

# ── CMake 設定 ──────────────────────────────────────────────────────────────
echo "--- CMake configure ---"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DENABLE_ROCKSDB="$ENABLE_ROCKSDB" \
  -DENABLE_BENCHMARK=ON \
  2>&1 | grep -E "(--|\[|\bError\b|Found|Warning.*RocksDB)" || true
echo ""

# ── ビルド ──────────────────────────────────────────────────────────────────
echo "--- Build bench_kv ---"
cmake --build "$BUILD_DIR" --target bench_kv --parallel
echo ""

# ── 実行 ────────────────────────────────────────────────────────────────────
BENCH_BIN="$BUILD_DIR/benchmark/bench_kv"
echo "--- Run benchmarks ---"
echo ""

# tee 先を動的に構築（auto log + optional --output）
TEE_TARGETS=()
if [[ "$AUTO_LOG" == "true" ]]; then
  TEE_TARGETS+=("$AUTO_LOG_FILE")
fi
if [[ -n "$OUTPUT_FILE" ]]; then
  mkdir -p "$(dirname "$OUTPUT_FILE")"
  TEE_TARGETS+=("$OUTPUT_FILE")
fi

if [[ ${#TEE_TARGETS[@]} -gt 0 ]]; then
  if [[ "$QUICK" == "true" ]]; then
    "$BENCH_BIN" --iters "$ITERS" --quick 2>&1 | tee "${TEE_TARGETS[@]}" || true
  else
    "$BENCH_BIN" --iters "$ITERS" 2>&1 | tee "${TEE_TARGETS[@]}" || true
  fi
  echo ""
  [[ "$AUTO_LOG" == "true" ]] && echo "Log saved to: $AUTO_LOG_FILE"
  [[ -n "$OUTPUT_FILE" ]]     && echo "Results saved to: $OUTPUT_FILE"
else
  if [[ "$QUICK" == "true" ]]; then
    "$BENCH_BIN" --iters "$ITERS" --quick || true
  else
    "$BENCH_BIN" --iters "$ITERS" || true
  fi
fi
