#!/usr/bin/env bash
# run_bench.sh — Run all FaultKV microbenchmark experiments (macOS & Linux)
#
# Usage:
#   chmod +x run_bench.sh
#   ./run_bench.sh [--data-file /path/to/bench_data.bin] [--size-gb 96]
#
# Requirements:
#   - sudo (for page cache purge/drop)
#   - cmake + C++20 compiler (clang 15+ on macOS, gcc 12+ or clang 15+ on Linux)
#   - jq
#   - ~96 GB free disk space (configurable via --size-gb)
#
# Output: macos/ or linux/ or linux-wsl/ directory with one JSON per experiment

set -euo pipefail

# ── OS detection ──────────────────────────────────────────────────────────────
OS=$(uname)
IS_WSL=false
if [[ "${OS}" == "Darwin" ]]; then
    IS_MACOS=true
    OUT=macos
else
    IS_MACOS=false
    if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null || grep -qi microsoft /proc/version 2>/dev/null; then
        IS_WSL=true
        OUT=linux-wsl
    else
        OUT=linux
    fi
fi

# ── Defaults (can be overridden via CLI args) ─────────────────────────────────
SIZE_GB=96
DATA_FILE=/tmp/bench_data.bin
BENCH=./build/bench

# ── Parse args ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --data-file) DATA_FILE="$2"; shift 2 ;;
        --size-gb)   SIZE_GB="$2";   shift 2 ;;
        --out-dir)   OUT="$2";       shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── Derived constants ─────────────────────────────────────────────────────────
COMMON="--size-gb ${SIZE_GB} --data-file ${DATA_FILE} --ops 1000000 --warmup 50000 --lru-pages 20000 --value-size 4096"
mkdir -p "${OUT}"

if ${IS_MACOS}; then
    echo "[setup] Output directory: ${OUT} (macOS)"
elif ${IS_WSL}; then
    echo "[setup] Output directory: ${OUT} (WSL/Linux)"
else
    echo "[setup] Output directory: ${OUT} (Linux)"
fi

# ── Helper: drop page cache ───────────────────────────────────────────────────
drop_cache() {
    echo "[cache] Dropping page cache..." >&2
    if ${IS_MACOS}; then
        sudo purge
    else
        sudo sync
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    fi
}

# ── Helper: check THP availability (Linux only) ───────────────────────────────
THP_OK=false
if ! ${IS_MACOS}; then
    thp_file=/sys/kernel/mm/transparent_hugepage/enabled
    if [[ -f "${thp_file}" ]]; then
        thp_val=$(cat "${thp_file}")
        if [[ "${thp_val}" != *"[never]"* ]]; then
            echo "[setup] THP enabled: ${thp_val}" >&2
            THP_OK=true
        else
            echo "[warn] THP is set to 'never'. Enable with:" >&2
            echo "       sudo sh -c 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled'" >&2
        fi
    else
        echo "[warn] THP not available on this kernel. --huge-pages experiments will be skipped." >&2
    fi
fi

# ── Build ─────────────────────────────────────────────────────────────────────
echo "[setup] Checking sudo access..."
sudo true

echo "[build] Configuring..."
if ${IS_MACOS}; then
    cmake -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
    NCPU=$(sysctl -n hw.logicalcpu)
else
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX:-g++}" > /dev/null
    NCPU=$(nproc)
fi
echo "[build] Compiling..."
cmake --build build --parallel "${NCPU}"
echo "[build] Done: ${BENCH}"

# ── Collect system info ───────────────────────────────────────────────────────
SYSINFO_FILE="${OUT}/sysinfo.txt"
echo "[setup] Collecting system info → ${SYSINFO_FILE}"
{
    echo "=== Date ==="
    date
    echo ""
    if ${IS_MACOS}; then
        echo "=== OS ==="
        sw_vers
        echo ""
        echo "=== Hardware ==="
        system_profiler SPHardwareDataType 2>/dev/null | grep -E "Model|Chip|CPU|Memory|Cores"
        echo ""
        echo "=== CPU details ==="
        sysctl -n machdep.cpu.brand_string 2>/dev/null || sysctl -n hw.model
        echo "Logical CPUs: $(sysctl -n hw.logicalcpu)"
        echo "Physical CPUs: $(sysctl -n hw.physicalcpu)"
        echo ""
        echo "=== Memory ==="
        echo "Total: $(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )) GB"
        echo ""
        echo "=== Compiler ==="
        clang++ --version
    else
        echo "=== OS ==="
        cat /etc/os-release 2>/dev/null || uname -a
        echo ""
        echo "=== Kernel ==="
        uname -r
        echo ""
        echo "=== CPU ==="
        lscpu 2>/dev/null || grep -E "model name|cpu cores|siblings" /proc/cpuinfo | sort -u
        echo ""
        echo "=== Memory ==="
        free -h
        echo ""
        echo "=== THP ==="
        cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "N/A"
        echo ""
        echo "=== Compiler ==="
        "${CXX:-g++}" --version
    fi
    echo ""
    echo "=== Storage (data file device) ==="
    df -h "$(dirname "${DATA_FILE}")"
} > "${SYSINFO_FILE}"
cat "${SYSINFO_FILE}"

# ── Create data file if it doesn't exist ─────────────────────────────────────
if [[ ! -f "${DATA_FILE}" ]]; then
    echo "[setup] Creating ${SIZE_GB} GB data file at ${DATA_FILE}..."
    if ${IS_MACOS}; then
        dd if=/dev/urandom of="${DATA_FILE}" bs=1m count=$(( SIZE_GB * 1024 )) status=progress
    else
        dd if=/dev/urandom of="${DATA_FILE}" bs=1M count=$(( SIZE_GB * 1024 )) status=progress
    fi
    echo "[setup] Data file created."
else
    if ${IS_MACOS}; then
        ACTUAL_GB=$(( $(stat -f%z "${DATA_FILE}") / 1024 / 1024 / 1024 ))
    else
        ACTUAL_GB=$(( $(stat -c%s "${DATA_FILE}") / 1024 / 1024 / 1024 ))
    fi
    echo "[setup] Data file exists: ${DATA_FILE} (${ACTUAL_GB} GB)"
fi

# ── Experiment runner ─────────────────────────────────────────────────────────
# Usage: run_exp <output_name> [--cold] <bench args...>
#   --cold: drop page cache before running
run_exp() {
    local name="$1"; shift
    local cold=false
    if [[ "${1:-}" == "--cold" ]]; then
        cold=true; shift
    fi

    local out_file="${OUT}/${name}.json"
    if [[ -f "${out_file}" ]]; then
        echo "[skip] ${name}.json already exists"
        return
    fi

    echo ""
    echo "════════════════════════════════════════════════════════"
    echo "[exp] ${name}"
    echo "      args: $*"

    if ${cold}; then
        drop_cache
    fi

    "${BENCH}" "$@" > "${out_file}"
    local tput_mmap tput_lru
    tput_mmap=$(jq -r '.results.mmap.throughput_ops_per_sec' "${out_file}")
    tput_lru=$(jq -r '.results.pread_lru.throughput_ops_per_sec' "${out_file}")
    printf "[exp] done  mmap=%.0f ops/s  LRU=%.0f ops/s\n" "${tput_mmap}" "${tput_lru}"
}

# ─────────────────────────────────────────────────────────────────────────────
# Experiments
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 1: Baseline (cold, Zipf α=1.0 over full dataset)"
echo "████████████████████████████████████████████████████████████"

run_exp cold --cold \
    ${COMMON} --zipf-alpha 1.0

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 2: Warm baseline (prefault access sequence into page cache)"
echo "████████████████████████████████████████████████████████████"

run_exp warm \
    ${COMMON} --zipf-alpha 1.0 --prefault

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 3: Low locality (cold, Zipf α=0.5 ≈ near-uniform)"
echo "████████████████████████████████████████████████████████████"

run_exp cold_uniform --cold \
    ${COMMON} --zipf-alpha 0.5

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 4: MADV_WILLNEED prefetch (cold, varying lookahead)"
echo "████████████████████████████████████████████████████████████"

run_exp prefetch_8 --cold \
    ${COMMON} --zipf-alpha 1.0 --prefetch-ahead 8

run_exp prefetch_32 --cold \
    ${COMMON} --zipf-alpha 1.0 --prefetch-ahead 32

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 5: Multi-thread (cold)"
echo "████████████████████████████████████████████████████████████"

run_exp threads8 --cold \
    ${COMMON} --zipf-alpha 1.0 --threads 8

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 6: Scan workload (MADV_WILLNEED batch + batch read)"
echo "████████████████████████████████████████████████████████████"

run_exp scan32 --cold \
    ${COMMON} --zipf-alpha 1.0 --scan-width 32

run_exp threads8_scan32 --cold \
    ${COMMON} --zipf-alpha 1.0 --threads 8 --scan-width 32

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 7: Hot set size sweep"
echo "           zipf-n controls how many unique pages = hot set size"
echo "           lru-pages=20000 → LRU buffer = 80 MB"
echo "████████████████████████████████████████████████████████████"

# hot set 80 MB  (20000 pages × 4 KB = 80 MB — fits in LRU buffer)
run_exp hotset_80mb --cold \
    ${COMMON} --zipf-alpha 1.0 --zipf-n 20000

# hot set 800 MB (200000 pages × 4 KB = 800 MB — 10× LRU buffer)
run_exp hotset_800mb --cold \
    ${COMMON} --zipf-alpha 1.0 --zipf-n 200000

# hot set 8 GB (Linux only; skipped on macOS as not previously measured)
if ! ${IS_MACOS}; then
    run_exp hotset_8gb --cold \
        ${COMMON} --zipf-alpha 1.0 --zipf-n 2000000
fi

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 8: Zipf α sweep (cold, full dataset, α=0.5/1.0/1.5/2.0)"
echo "          Measures how LRU competitiveness shifts with locality"
echo "████████████████████████████████████████████████████████████"

# α=0.5: near-uniform, LRU hit rate ~3% → pread miss dominates
run_exp alpha_050 --cold \
    ${COMMON} --zipf-alpha 0.5

# α=1.0: power-law standard (same workload as 'cold', regenerated for sweep)
run_exp alpha_100 --cold \
    ${COMMON} --zipf-alpha 1.0

# α=1.5: concentrated hot set, LRU hit rate ~85%
run_exp alpha_150 --cold \
    ${COMMON} --zipf-alpha 1.5

# α=2.0: very hot few pages, LRU hit rate ~99%
run_exp alpha_200 --cold \
    ${COMMON} --zipf-alpha 2.0

echo ""
echo "████████████████████████████████████████████████████████████"
echo " Phase 9: Linux THP variants (--huge-pages, Linux only)"
echo "          Requires THP enabled (madvise or always mode)"
echo "████████████████████████████████████████████████████████████"

if ! ${IS_MACOS}; then
    if ${THP_OK}; then
        # warm + THP: key comparison — can THP close the L3-cache gap?
        run_exp warm_thp \
            ${COMMON} --zipf-alpha 1.0 --prefault --huge-pages

        # cold + THP
        run_exp cold_thp --cold \
            ${COMMON} --zipf-alpha 1.0 --huge-pages

        # hot set 80 MB + THP: macOS LRU won 1.7×; THP may flip this
        run_exp hotset_80mb_thp --cold \
            ${COMMON} --zipf-alpha 1.0 --zipf-n 20000 --huge-pages

        # hot set 800 MB + THP
        run_exp hotset_800mb_thp --cold \
            ${COMMON} --zipf-alpha 1.0 --zipf-n 200000 --huge-pages
    else
        echo "[skip] THP experiments skipped (THP not available or disabled)"
        echo "       To enable: sudo sh -c 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled'"
        echo "       Then re-run this script."
    fi
else
    echo "[skip] THP experiments not available on macOS"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════"
echo " All experiments complete. Results in: ${OUT}/"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Quick summary:"
printf "%-22s  %14s  %14s\n" "Experiment" "mmap ops/s" "LRU ops/s"
printf "%-22s  %14s  %14s\n" "──────────────────────" "──────────────" "──────────────"
for f in "${OUT}"/*.json; do
    name=$(basename "${f}" .json)
    mmap=$(jq -r '.results.mmap.throughput_ops_per_sec'      "${f}" 2>/dev/null || echo "-")
    lru=$(jq  -r '.results.pread_lru.throughput_ops_per_sec' "${f}" 2>/dev/null || echo "-")
    printf "%-22s  %14.0f  %14.0f\n" "${name}" "${mmap}" "${lru}"
done
