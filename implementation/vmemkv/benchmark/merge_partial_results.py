#!/usr/bin/env python3
"""Merge a partial-matrix run's entries back into a prior full-matrix run's numbers, so a
follow-up run never has to re-measure stores its change can't have affected.

Usage:
  merge_partial_results.py --new-dir <dir> --old-dir <dir> --out-dir <dir>
                           [--keep-prefix Store=VMemKV/] [--suffix _vmemkv_only.json]

Matches new-dir's "results_<scenario>[_<value_size>]<suffix>" files against old-dir's
"results_<scenario>[_<value_size>].json" files by that same base name, keeps every
<keep-prefix>* benchmark entry from the new file and every other entry from the old file,
and writes the union under the new file's own context (git revision, instance type, etc.
reflect the run that actually produced the kept numbers).

Typical invocations:
  --without-rivals rerun: --keep-prefix Store=VMemKV/ --suffix _vmemkv_only.json
  --leanstore-only rerun: --keep-prefix Store=LeanStore/ --suffix _leanstore_only.json
"""

import argparse
import json
import sys
from pathlib import Path


def merge_one(new_path: Path, old_path: Path, out_path: Path, keep_prefix: str, keep_label: str) -> None:
    new_data = json.loads(new_path.read_text())
    old_data = json.loads(old_path.read_text())

    new_benchmarks = [b for b in new_data["benchmarks"] if b["name"].startswith(keep_prefix)]
    stray = [b["name"] for b in new_data["benchmarks"] if not b["name"].startswith(keep_prefix)]
    if stray:
        print(f"[WARN] {new_path}: {len(stray)} non-{keep_label} entries found in a partial "
              f"run's own output (kept, but this file may not have been run with the matching "
              f"filter): {stray[:3]}", file=sys.stderr)

    old_other_benchmarks = [b for b in old_data["benchmarks"] if not b["name"].startswith(keep_prefix)]
    if not old_other_benchmarks:
        print(f"[WARN] {old_path}: no non-{keep_label} entries found -- merged output will "
              f"contain the new file's entries alone", file=sys.stderr)

    merged = dict(new_data)
    merged["benchmarks"] = old_other_benchmarks + new_benchmarks
    merged["context"] = dict(new_data["context"])
    merged["context"]["merged_partial_results_from"] = str(old_path)

    out_path.write_text(json.dumps(merged, indent=2) + "\n")
    print(f"[OK] {out_path}: {len(old_other_benchmarks)} kept + {len(new_benchmarks)} {keep_label} "
          f"entries (from {old_path.name} + {new_path.name})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--new-dir", required=True, type=Path,
                        help="Directory containing this run's results_*<suffix> files")
    parser.add_argument("--old-dir", required=True, type=Path,
                        help="Directory containing the prior full-matrix results_*.json files")
    parser.add_argument("--out-dir", required=True, type=Path,
                        help="Directory to write merged results_*.json files into")
    parser.add_argument("--keep-prefix", default="Store=VMemKV/",
                        help="Benchmark name prefix kept from the new run (default: %(default)s)")
    parser.add_argument("--suffix", default="_vmemkv_only.json",
                        help="Filename suffix identifying the new run's files (default: %(default)s)")
    args = parser.parse_args()

    keep_label = args.keep_prefix.removeprefix("Store=").removesuffix("/")
    new_files = sorted(args.new_dir.glob(f"results_*{args.suffix}"))
    if not new_files:
        print(f"[ERROR] No results_*{args.suffix} files found in {args.new_dir}", file=sys.stderr)
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)
    ok = True
    for new_path in new_files:
        base_name = new_path.name.removesuffix(args.suffix)
        old_path = args.old_dir / f"{base_name}.json"
        out_path = args.out_dir / f"{base_name}.json"
        if not old_path.exists():
            print(f"[ERROR] {new_path}: no matching old file at {old_path}", file=sys.stderr)
            ok = False
            continue
        merge_one(new_path, old_path, out_path, args.keep_prefix, keep_label)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
