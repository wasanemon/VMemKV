#!/usr/bin/env python3
"""Merge a --without-rivals run's VMemKV-only results back into a prior full-matrix run's
rival numbers (RocksDB/RocksDB-BlobDB/LMDB), so a regression-check run never has to
re-measure rivals whose numbers a VMemKV-internal-only change can't have affected.

Usage:
  merge_vmemkv_only_results.py --new-dir <dir with results_*_vmemkv_only.json> \
                                --old-dir <dir with prior full results_*.json> \
                                --out-dir <dir to write merged results_*.json>

Matches new-dir's "results_<scenario>[_<value_size>]_vmemkv_only.json" files against
old-dir's "results_<scenario>[_<value_size>].json" files by that same base name, keeps every
Store=VMemKV/* benchmark entry from the new file and every non-VMemKV entry from the old
file, and writes the union under the new file's own context (git revision, instance type,
etc. reflect the run that actually produced the VMemKV numbers being reported).
"""

import argparse
import json
import sys
from pathlib import Path

VMEMKV_PREFIX = "Store=VMemKV/"
SUFFIX = "_vmemkv_only.json"


def merge_one(new_path: Path, old_path: Path, out_path: Path) -> None:
    new_data = json.loads(new_path.read_text())
    old_data = json.loads(old_path.read_text())

    new_benchmarks = [b for b in new_data["benchmarks"] if b["name"].startswith(VMEMKV_PREFIX)]
    stray = [b["name"] for b in new_data["benchmarks"] if not b["name"].startswith(VMEMKV_PREFIX)]
    if stray:
        print(f"[WARN] {new_path}: {len(stray)} non-VMemKV entries found in a --without-rivals "
              f"run's own output (kept, but this file may not have been run with "
              f"--without-rivals): {stray[:3]}", file=sys.stderr)

    old_rival_benchmarks = [b for b in old_data["benchmarks"] if not b["name"].startswith(VMEMKV_PREFIX)]
    if not old_rival_benchmarks:
        print(f"[WARN] {old_path}: no rival (non-VMemKV) entries found -- merged output will be "
              f"VMemKV-only, same as the new file alone", file=sys.stderr)

    merged = dict(new_data)
    merged["benchmarks"] = new_benchmarks + old_rival_benchmarks
    merged["context"] = dict(new_data["context"])
    merged["context"]["merged_rival_results_from"] = str(old_path)

    out_path.write_text(json.dumps(merged, indent=2) + "\n")
    print(f"[OK] {out_path}: {len(new_benchmarks)} VMemKV + {len(old_rival_benchmarks)} rival "
          f"entries (from {new_path.name} + {old_path.name})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--new-dir", required=True, type=Path,
                         help="Directory containing this run's results_*_vmemkv_only.json files")
    parser.add_argument("--old-dir", required=True, type=Path,
                         help="Directory containing the prior full-matrix results_*.json files")
    parser.add_argument("--out-dir", required=True, type=Path,
                         help="Directory to write merged results_*.json files into")
    args = parser.parse_args()

    new_files = sorted(args.new_dir.glob(f"results_*{SUFFIX}"))
    if not new_files:
        print(f"[ERROR] No results_*{SUFFIX} files found in {args.new_dir}", file=sys.stderr)
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)
    ok = True
    for new_path in new_files:
        base_name = new_path.name[: -len(SUFFIX)]  # strip "_vmemkv_only.json"
        old_path = args.old_dir / f"{base_name}.json"
        out_path = args.out_dir / f"{base_name}.json"
        if not old_path.exists():
            print(f"[ERROR] {new_path}: no matching old file at {old_path}", file=sys.stderr)
            ok = False
            continue
        merge_one(new_path, old_path, out_path)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
