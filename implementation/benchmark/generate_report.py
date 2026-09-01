#!/usr/bin/env python3
"""Generate a benchmark_results/pages/<id>_charts.html report from a benchmark_results/<id>/
directory, following the same Chart.js-based template established by prior reports (e.g.
2026081313_charts.html). Reuses that report's JS scaffolding verbatim (tab switching, chart
configs, plugins) and only regenerates the embedded data blocks (rawData/timelineData/
reorgScalingData), the header/description, and the Winners Matrix summary table.

Usage:
  generate_report.py --report-id 2026081711 --template pages/2026081313_charts.html \
                      --title-suffix "(without-rivals + livelock fixes)" \
                      --description "..." --out pages/2026081711_charts.html
"""

import argparse
import json
import re
from pathlib import Path

STORE_VARIANT_TO_LABEL = {
    ("RocksDB", "RocksDB"): "RocksDB",
    ("LMDB", "LMDB"): "LMDB",
    ("RocksDB-BlobDB", "RocksDB-BlobDB"): "RocksDB-BlobDB",
    ("VMemKV", "Baseline"): "Baseline",
    ("VMemKV", "Bloom"): "+BF",
    ("VMemKV", "Bloom-T1InlineValue"): "+Inline",
    ("VMemKV", "Bloom-T1InlineValue-Prefaulting"): "+Prefault",
}

VARIANT_ORDER = ["RocksDB", "LMDB", "RocksDB-BlobDB", "Baseline", "+BF", "+Inline", "+Prefault"]
RIVAL_STORES = ["RocksDB", "LMDB", "RocksDB-BlobDB"]
COLORS = {
    "RocksDB": "#64748b", "LMDB": "#10b981", "RocksDB-BlobDB": "#a855f7",
    "Baseline": "#94a3b8",
    "+BF": "#f59e0b", "+Inline": "#6366f1", "+Prefault": "#ec4899",
}
WORKLOADS = ["Insert", "Update", "Delete", "Get_Miss", "Get_Hit_Zipf", "Get_Hit_Uniform", "Scan_Zipf", "Scan_Uniform"]
THREADS = [1, 4, 16, 32]

SCENARIO_LABELS = {
    ("in_memory", "8B"): "8B_In-Memory",
    ("in_memory", "1KB"): "1KB_In-Memory",
    ("ltm", "1KB"): "1KB_LTM",
    ("ltm", "64KB"): "64KB_LTM",
}


def parse_bench_name(name):
    parts = name.split("/")
    store = parts[0].split("=", 1)[1]
    variant = parts[1].split("=", 1)[1]
    op = None
    mode = None
    dist = None
    threads = None
    for p in parts[2:]:
        if p.startswith("Op="):
            op = p.split("=", 1)[1]
        elif p.startswith("Mode="):
            mode = p.split("=", 1)[1]
        elif p.startswith("Dist="):
            dist = p.split("=", 1)[1]
        elif p.startswith("threads:"):
            threads = int(p.split(":", 1)[1])
    workload = None
    if op == "Insert":
        workload = "Insert"
    elif op == "Delete":
        workload = "Delete"
    elif op == "Update":
        workload = "Update"
    elif op == "Get":
        if mode == "Miss":
            workload = "Get_Miss"
        elif mode == "Hit" and dist == "Zipf":
            workload = "Get_Hit_Zipf"
        elif mode == "Hit" and dist == "Uniform":
            workload = "Get_Hit_Uniform"
    elif op == "Scan":
        if dist == "Zipf":
            workload = "Scan_Zipf"
        elif dist == "Uniform":
            workload = "Scan_Uniform"
    return store, variant, workload, threads


def build_raw_data(report_dir):
    raw_data = {}
    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        fname = report_dir / f"results_{scenario}_{val_size}.json"
        data = json.loads(fname.read_text())
        op_map = {}
        for b in data["benchmarks"]:
            store, variant, workload, threads = parse_bench_name(b["name"])
            if workload is None or threads not in THREADS:
                continue
            label = STORE_VARIANT_TO_LABEL.get((store, variant))
            if label is None:
                continue
            op_map.setdefault(workload, {}).setdefault(label, [None] * len(THREADS))
            idx = THREADS.index(threads)
            op_map[workload][label][idx] = b.get("items_per_second", 0.0)
        raw_data[scenario_key] = op_map
    return raw_data


def _normalize_value_size(file_val_size):
    # bench_kv writes e.g. "1KB_20__8B_" for the 80/20-mix value-size label -- normalize
    # to the plain "1KB"/"64KB"/"8B" keys used elsewhere (SCENARIO_LABELS).
    if file_val_size.startswith("1KB"):
        return "1KB"
    if file_val_size.startswith("64KB"):
        return "64KB"
    return file_val_size


def build_timeline_data(report_dir):
    timeline_data = {}
    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        variants = {}
        for f in report_dir.glob(f"ycsb_e_timeline_{scenario}_*.json"):
            d = json.loads(f.read_text())
            store = d["store"]
            variant = d["variant"]
            if _normalize_value_size(d["value_size"]) != val_size:
                continue
            base_store = store.split("-", 1)[0] if store.startswith("VMemKV") else store
            label = STORE_VARIANT_TO_LABEL.get((base_store, variant))
            if label is None:
                continue
            variants[label] = d["timeline"]
        timeline_data[scenario_key] = variants
    return timeline_data


def build_forced_events_data(report_dir):
    """Per (scenario, variant): the actual forced_events list (scheduled_sec, fired_sec, kind,
    elapsed_sec). Fired second can lag scheduled second arbitrarily under sustained write
    contention (cascading is intentionally unguarded -- see kForcedTriggers in bench_kv.cpp)."""
    forced_events_data = {}
    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        variants = {}
        for f in report_dir.glob(f"ycsb_e_timeline_{scenario}_*.json"):
            d = json.loads(f.read_text())
            store = d["store"]
            variant = d["variant"]
            if _normalize_value_size(d["value_size"]) != val_size:
                continue
            base_store = store.split("-", 1)[0] if store.startswith("VMemKV") else store
            label = STORE_VARIANT_TO_LABEL.get((base_store, variant))
            if label is None:
                continue
            variants[label] = d.get("forced_events", [])
        forced_events_data[scenario_key] = variants
    return forced_events_data


def build_reorg_scaling_data(report_dir):
    reorg_data = {}
    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        fname = report_dir / f"reorg_scaling_{scenario}_{val_size}.jsonl"
        if not fname.exists():
            continue
        modes = {}
        for line in fname.read_text().splitlines():
            if not line.strip():
                continue
            rec = json.loads(line)
            mode = rec["mode"]
            modes.setdefault(mode, []).append({
                "key_count": rec["key_count"],
                "ratio": rec.get("ratio"),
                "elapsed_sec": rec["elapsed_sec"],
                "timed_out": rec["timed_out"],
            })
        for mode_points in modes.values():
            # key_count is null for an outer_timeout record (run_probe_point()'s synthesized
            # failure fallback, reorg_probe_common.sh) -- setup never even reported a corpus size.
            # Sorts last: it represents "went further than the largest point that did complete."
            mode_points.sort(key=lambda p: (p["key_count"] is None, p["key_count"]))
        reorg_data[scenario_key] = modes
    return reorg_data


def _iter_fixed_files_by_scenario_field(report_dir, filenames):
    """Yields (scenario_key, rec) for every JSONL line in `filenames` (relative to report_dir)
    whose own "scenario"/"value_size" fields resolve to a known SCENARIO_LABELS entry."""
    for fname in filenames:
        path = report_dir / fname
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            if not line.strip():
                continue
            rec = json.loads(line)
            scenario_key = SCENARIO_LABELS.get((rec["scenario"], _value_size_label(rec.get("value_size"))))
            if scenario_key is not None:
                yield scenario_key, rec


def _iter_per_scenario_files(report_dir, filename_fmt):
    """Yields (scenario_key, rec) for every JSONL line in report_dir / filename_fmt.format(scenario=,
    val_size=), one file per SCENARIO_LABELS entry."""
    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        path = report_dir / filename_fmt.format(scenario=scenario, val_size=val_size)
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            if not line.strip():
                continue
            yield scenario_key, json.loads(line)


def _build_throughput_data(records, accept):
    """Reduces a (scenario_key, rec) stream to {scenario_key: {"key_count", "elapsed_sec",
    "records_per_sec"}}, keeping the last accepted record per scenario_key. `accept(rec)` decides
    which records count."""
    data = {}
    for scenario_key, rec in records:
        if not accept(rec):
            continue
        data[scenario_key] = {
            "key_count": rec["key_count"],
            "elapsed_sec": rec["elapsed_sec"],
            "records_per_sec": rec["key_count"] / rec["elapsed_sec"],
        }
    return data


def build_checkpoint_throughput_data(report_dir):
    """Reads checkpoint_throughput_in_memory.jsonl / checkpoint_throughput_ltm.jsonl
    (run_checkpoint_throughput_probe.sh, via bench_kv --reorg-probe --mode=t1t2_steady
    --ratio=1.0 --churn-ratio=0.25 -- full corpus size, a high-but-not-maximal churn ratio that
    isolates the marginal per-record durabilization cost from checkpoint()'s fixed per-call setup
    overhead while keeping setup work and checkpoint() I/O proportionally bounded) into
    {scenario_key: {"key_count", "elapsed_sec", "records_per_sec"}}, one entry per combo."""
    return _build_throughput_data(
        _iter_fixed_files_by_scenario_field(
            report_dir, ["checkpoint_throughput_in_memory.jsonl", "checkpoint_throughput_ltm.jsonl"]
        ),
        accept=lambda rec: not rec.get("timed_out"),
    )


def build_reorg_throughput_data(report_dir):
    """Reads reorg_scaling_<scenario>_<value_size>.jsonl (run_reorg_scaling_probe.sh, mode=t1only,
    ratio=1.0 -- the full-corpus point of the existing T1-only corpus-size sweep) into
    {scenario_key: {"key_count", "elapsed_sec", "records_per_sec"}}, one entry per combo.
    reorganize() is T1-only (never touches T2) and rebuilds the whole structure every call --
    cost tracks corpus size, not churn, so this reuses the sweep's own ratio=1.0 point rather
    than a separate probe."""
    return _build_throughput_data(
        _iter_per_scenario_files(report_dir, "reorg_scaling_{scenario}_{val_size}.jsonl"),
        accept=lambda rec: rec.get("mode") == "t1only" and (rec.get("ratio") or 0) == 1 and not rec.get("timed_out"),
    )


def build_maintenance_contention_data(report_dir):
    """Reads maintenance_contention_in_memory.jsonl / maintenance_contention_ltm.jsonl
    (run_maintenance_contention_probe.sh, modes checkpoint_contention/reorg_contention) into
    {scenario_val: {"checkpoint": {...}, "reorganize": {...}}} (isolated_write_tps/
    concurrent_write_tps/elapsed_sec/timed_out per op) -- see render_maintenance_contention_html().
    reorganize()
    (T1-only) often completes in well under a millisecond even at full corpus size -- its
    concurrent_write_tps figure is a mean over many repeated reorganize() calls spanning at least
    a second (bench_kv's run_reorg_contention() passes min_wall_seconds=1.0 to
    run_contention_probe() for this reason), not a single call's window, so it isn't noise despite
    the tiny per-call duration -- flagged inline in the rendered table so the duration figure
    itself isn't mistaken for that window."""
    data = {}
    for fname in ["maintenance_contention_in_memory.jsonl", "maintenance_contention_ltm.jsonl"]:
        path = report_dir / fname
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            if not line.strip():
                continue
            rec = json.loads(line)
            scenario_val = f'{rec["scenario"]}_{_value_size_label(rec.get("value_size"))}'
            mode = rec.get("mode")
            if mode == "checkpoint_contention":
                op, elapsed_key = "checkpoint", "checkpoint_elapsed_sec"
            elif mode == "reorg_contention":
                op, elapsed_key = "reorganize", "reorg_elapsed_sec"
            else:
                continue
            data.setdefault(scenario_val, {})[op] = {
                "isolated_write_tps": rec.get("isolated_write_tps"),
                "concurrent_write_tps": rec.get("concurrent_write_tps"),
                "elapsed_sec": rec.get(elapsed_key, rec.get("elapsed_sec")),
                "timed_out": rec.get("timed_out"),
                "failure_reason": rec.get("failure_reason"),
            }
    return data


def _value_size_label(value_size):
    if isinstance(value_size, str):
        return value_size
    return {8: "8B", 1024: "1KB", 65536: "64KB"}.get(value_size, str(value_size))


def compute_winners_matrix(raw_data):
    rows = []
    for workload in WORKLOADS:
        cells = []
        for scenario_key in ["8B_In-Memory", "1KB_In-Memory", "1KB_LTM", "64KB_LTM"]:
            op_data = raw_data.get(scenario_key, {}).get(workload)
            if not op_data:
                cells.append(None)
                continue
            idx32 = THREADS.index(32)
            best_vmemkv = None
            best_vmemkv_label = None
            best_rival = None
            best_rival_label = None
            for label, values in op_data.items():
                v = values[idx32]
                if v is None:
                    continue
                if label in RIVAL_STORES:
                    if best_rival is None or v > best_rival:
                        best_rival, best_rival_label = v, label
                else:
                    if best_vmemkv is None or v > best_vmemkv:
                        best_vmemkv, best_vmemkv_label = v, label
            if best_vmemkv is None or best_rival is None or best_rival == 0:
                cells.append(None)
                continue
            ratio = best_vmemkv / best_rival
            cells.append({
                "ratio": ratio,
                "vmemkv_label": best_vmemkv_label,
                "rival_label": best_rival_label,
            })
        rows.append((workload, cells))
    return rows


def _badge_for_ratio(ratio):
    """Matches the 5-tier scheme established by prior reports (e.g. 2026081313): a ±5% band
    around 1.0x is noise-level ("EVEN", gray); beyond that a weak (pale) or strong (solid,
    with emoji) WIN/LOSE badge depending on whether the deviation exceeds 15%."""
    if ratio <= 0.85:
        return ("❌ LOSE", "bg-rose-600 text-white border-transparent shadow-sm")
    if ratio < 0.95:
        return ("LOSE", "bg-rose-50 text-rose-700 border-rose-200")
    if ratio <= 1.05:
        return ("≈ EVEN", "bg-slate-100 text-slate-600 border-slate-200")
    if ratio < 1.15:
        return ("WIN", "bg-emerald-50 text-emerald-700 border-emerald-200")
    return ("✅ WIN", "bg-emerald-600 text-white border-transparent shadow-sm")


def render_winners_matrix_html(rows):
    workload_display = {
        "Insert": "Insert", "Update": "Update (Zipf)", "Delete": "Delete",
        "Get_Miss": "Get (Miss)", "Get_Hit_Zipf": "Get (Hit, Zipf)", "Get_Hit_Uniform": "Get (Hit, Uniform)",
        "Scan_Zipf": "Scan (Zipf)", "Scan_Uniform": "Scan (Uniform)",
    }
    out = []
    for workload, cells in rows:
        out.append(f'<tr class="hover:bg-indigo-50/10 transition-colors"><td class="py-3.5 px-4 font-bold text-slate-800 bg-slate-50/20">{workload_display[workload]}</td>')
        for cell in cells:
            if cell is None:
                out.append('<td class="py-3.5 px-4 text-slate-300 text-xs">n/a</td>')
                continue
            ratio = cell["ratio"]
            label_text, badge_class = _badge_for_ratio(ratio)
            out.append(f'''<td class="py-3.5 px-4 transition-colors">
  <div class="flex flex-col gap-0.5">
    <span class="inline-flex items-center px-1.5 py-0.5 rounded text-[10px] border {badge_class} font-bold  w-fit">
      {label_text} ({ratio:.2f}x)
    </span>
    <span class="text-[11px] text-slate-500 font-medium">vmemkv ({cell["vmemkv_label"]})</span>
    <span class="text-[10px] text-slate-400">vs {cell["rival_label"]}</span>
  </div>
</td>''')
        out.append("</tr>")
    return "\n".join(out)


def _badge_for_checkpoint_headroom(ratio):
    """ratio = Insert throughput / checkpoint() throughput. Inverted sense from
    _badge_for_ratio(): here a HIGH ratio is bad (checkpoint can't keep up with the write rate
    generating its churn), a LOW ratio is good (checkpoint has headroom)."""
    if ratio >= 1.1:
        return ("❌ Falls behind", "bg-rose-600 text-white border-transparent shadow-sm")
    if ratio >= 0.9:
        return ("≈ At capacity", "bg-amber-50 text-amber-700 border-amber-200")
    return ("✅ Keeps up", "bg-emerald-50 text-emerald-700 border-emerald-200")


def _render_vs_insert_table_html(throughput_data, raw_data, op_column_header):
    if not throughput_data:
        return ""
    idx32 = THREADS.index(32)
    out = ['<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">',
           '<thead><tr class="border-b border-slate-200 bg-slate-50/50">',
           '<th class="py-2 px-3 font-bold text-slate-700">Scenario/Value</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Insert (32 threads)</th>',
           f'<th class="py-2 px-3 font-bold text-slate-700">{op_column_header}</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Verdict</th>',
           "</tr></thead><tbody class=\"divide-y divide-slate-100\">"]
    for scenario_key in ["8B_In-Memory", "1KB_In-Memory", "1KB_LTM", "64KB_LTM"]:
        entry = throughput_data.get(scenario_key)
        insert_series = raw_data.get(scenario_key, {}).get("Insert", {}).get("+Inline")
        if not entry or not insert_series or insert_series[idx32] is None:
            continue
        insert_rate = insert_series[idx32]
        op_rate = entry["records_per_sec"]
        ratio = insert_rate / op_rate if op_rate else float("inf")
        label_text, badge_class = _badge_for_checkpoint_headroom(ratio)
        out.append(f'<tr><td class="py-2 px-3">{scenario_key}</td>'
                    f'<td class="py-2 px-3">{insert_rate:,.0f}/s</td>'
                    f'<td class="py-2 px-3">{op_rate:,.0f}/s</td>'
                    f'<td class="py-2 px-3"><span class="inline-flex items-center px-1.5 py-0.5 rounded text-[10px] border {badge_class} font-bold w-fit">{label_text} ({ratio:.2f}x)</span></td></tr>')
    out.append("</tbody></table></div>")
    return "\n".join(out)


def render_checkpoint_vs_insert_table_html(checkpoint_throughput_data, raw_data):
    return _render_vs_insert_table_html(checkpoint_throughput_data, raw_data, "checkpoint() steady-state")


def render_reorg_vs_insert_table_html(reorg_throughput_data, raw_data):
    return _render_vs_insert_table_html(reorg_throughput_data, raw_data, "reorganize() full-corpus")


def _badge_for_slowdown(pct):
    """pct: percentage drop in concurrent write TPS vs. isolated (higher = worse). Same 3-tier
    color language as _badge_for_ratio()'s strong tiers, just collapsed to 3 steps since slowdown
    has no "better than isolated" side to distinguish."""
    if pct >= 50:
        return "bg-rose-600 text-white border-transparent shadow-sm"
    if pct >= 20:
        return "bg-amber-50 text-amber-700 border-amber-200"
    return "bg-emerald-50 text-emerald-700 border-emerald-200"


def render_maintenance_contention_html(maintenance_contention_data):
    if not maintenance_contention_data:
        return ""
    # Grouped by operation (one mini-table per op) rather than by scenario: the reader's actual
    # question is almost always "how does checkpoint() alone degrade across scenarios", not
    # "what's every op doing for one scenario", so this ordering puts the 4 scenario rows that
    # answer that side by side instead of scattered rows apart across separate scenario blocks.
    out = []
    op_labels = {"checkpoint": "checkpoint()", "reorganize": "reorganize()"}
    for op in ["checkpoint", "reorganize"]:
        rows_for_op = [(sv, maintenance_contention_data[sv][op])
                       for sv in ["in_memory_8B", "in_memory_1KB", "ltm_1KB", "ltm_64KB"]
                       if sv in maintenance_contention_data and op in maintenance_contention_data[sv]]
        if not rows_for_op:
            continue
        out.append(f'<h4 class="text-xs font-bold text-slate-700 uppercase tracking-wide mt-4 first:mt-0">{op_labels[op]}</h4>')
        out.append('<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">'
                    '<thead><tr class="border-b border-slate-200 bg-slate-50/50">'
                    '<th class="py-2 px-3 font-bold text-slate-700">Scenario/Value</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Isolated Write TPS</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Concurrent Write TPS</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Slowdown</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Operation Duration</th>'
                    '</tr></thead><tbody class="divide-y divide-slate-100">')
        for scenario_val, r in rows_for_op:
            if r.get("failure_reason"):
                out.append(f'<tr><td class="py-2 px-3">{scenario_val}</td>'
                            f'<td class="py-2 px-3 text-slate-300" colspan="2">n/a</td>'
                            f'<td class="py-2 px-3"><span class="text-rose-600 font-semibold">did not complete ({r["failure_reason"]})</span></td></tr>')
                continue
            iso = r["isolated_write_tps"]
            conc = r["concurrent_write_tps"]
            status = (f'<span class="text-rose-600 font-semibold">&ge;{r["elapsed_sec"]:.0f}s (timeout)</span>'
                      if r["timed_out"] else f'{r["elapsed_sec"]:.2f}s')
            note = ('  <span class="text-slate-400">(single-call duration -- Isolated/Concurrent TPS above are '
                    'averaged over many repeated calls spanning &ge;1s, not this one call)</span>'
                    if op == "reorganize" and r["elapsed_sec"] < 0.01 else "")
            if iso:
                pct = (1 - conc / iso) * 100
                slowdown_html = (f'<span class="inline-flex items-center px-1.5 py-0.5 rounded text-[10px] border '
                                  f'{_badge_for_slowdown(pct)} font-bold w-fit">{pct:.0f}%</span>')
            else:
                slowdown_html = "n/a"
            out.append(f'<tr><td class="py-2 px-3">{scenario_val}</td>'
                        f'<td class="py-2 px-3">{iso:,.0f}/s</td><td class="py-2 px-3">{conc:,.0f}/s</td>'
                        f'<td class="py-2 px-3">{slowdown_html}</td><td class="py-2 px-3">{status}{note}</td></tr>')
        out.append("</tbody></table></div>")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report-id", required=True)
    parser.add_argument("--report-dir", required=True, type=Path)
    parser.add_argument("--template", required=True, type=Path)
    parser.add_argument("--template-id", required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--title", required=True)
    parser.add_argument("--description-html", required=True)
    parser.add_argument("--env-html", required=True)
    args = parser.parse_args()

    html = args.template.read_text()

    raw_data = build_raw_data(args.report_dir)
    timeline_data = build_timeline_data(args.report_dir)
    forced_events_data = build_forced_events_data(args.report_dir)
    reorg_data = build_reorg_scaling_data(args.report_dir)
    checkpoint_throughput_data = build_checkpoint_throughput_data(args.report_dir)
    reorg_throughput_data = build_reorg_throughput_data(args.report_dir)
    maintenance_contention_data = build_maintenance_contention_data(args.report_dir)
    winners_rows = compute_winners_matrix(raw_data)

    html = html.replace(args.template_id, args.report_id)

    def replace_block(html, start_marker, end_marker, new_js_value_expr):
        start = html.index(start_marker)
        end = html.index(end_marker, start)
        return html[:start] + new_js_value_expr + html[end:]

    html = replace_block(
        html, "const rawData = {", "const timelineData = {",
        "const rawData = " + json.dumps(raw_data, indent=2) + ";\n    "
    )
    html = replace_block(
        html, "const timelineData = {", "const reorgScalingData = {",
        "const timelineData = " + json.dumps(timeline_data, indent=2) + ";\n    " +
        "const forcedEventsData = " + json.dumps(forced_events_data, indent=2) + ";\n    "
    )
    html = replace_block(
        html, "const reorgScalingData = {", "const workloads =",
        "const reorgScalingData = " + json.dumps(reorg_data, indent=2) + ";\n    " +
        "const checkpointThroughputData = " + json.dumps(checkpoint_throughput_data, indent=2) + ";\n    " +
        "const reorgThroughputData = " + json.dumps(reorg_throughput_data, indent=2) + ";\n    "
    )

    # Header title / links / description.
    old_title = f'<title>VMemKV Performance Charts ({args.template_id})</title>'
    new_title = f'<title>{args.title}</title>'
    html = html.replace(old_title, new_title)

    h1_old = f'<h1 class="text-3xl font-bold tracking-tight text-slate-900">Benchmark Results ({args.report_id})</h1>'
    h1_new = f'<h1 class="text-3xl font-bold tracking-tight text-slate-900">{args.title}</h1>'
    html = html.replace(h1_old, h1_new)

    download_links_start = html.index('<div class="flex flex-wrap gap-2">')
    download_links_end = html.index("</div>", download_links_start)
    links = []
    for fname, label in [
        (f"results_in_memory_8B.json", "In-mem 8B"),
        (f"results_in_memory_1KB.json", "In-mem 1KB"),
        (f"results_ltm_1KB.json", "LTM 1KB"),
        (f"results_ltm_64KB.json", "LTM 64KB"),
        (f"reorg_scaling_in_memory_8B.jsonl", "Reorg Scaling, In-Mem 8B"),
        (f"reorg_scaling_in_memory_1KB.jsonl", "Reorg Scaling, In-Mem 1KB"),
        (f"reorg_scaling_ltm_1KB.jsonl", "Reorg Scaling, LTM 1KB"),
        (f"reorg_scaling_ltm_64KB.jsonl", "Reorg Scaling, LTM 64KB"),
        (f"checkpoint_throughput_in_memory.jsonl", "Checkpoint Throughput, In-Memory"),
        (f"checkpoint_throughput_ltm.jsonl", "Checkpoint Throughput, LTM"),
    ]:
        if not (args.report_dir / fname).exists():
            continue
        links.append(
            f'<a href="../{args.report_id}/{fname}" download class="inline-flex items-center gap-1 text-xs text-slate-500 bg-slate-100 rounded px-2.5 py-1.5 hover:bg-slate-200">'
            f'<i data-lucide="download" class="w-3.5 h-3.5"></i> Raw {"JSONL" if fname.endswith("jsonl") else "JSON"} ({label})</a>'
        )
    html = html[:download_links_start] + '<div class="flex flex-wrap gap-2">\n          ' + "\n          ".join(links) + "\n        " + html[download_links_end:]

    desc_start = html.index('<p class="text-slate-500 text-sm max-w-3xl">')
    desc_end = html.index("</p>", desc_start) + len("</p>")
    html = html[:desc_start] + f'<p class="text-slate-500 text-sm max-w-3xl">{args.description_html}</p>' + html[desc_end:]

    # Environment section: replace the <ul> under "Environment" heading.
    env_marker = '<i data-lucide="cpu" class="w-3.5 h-3.5"></i> Environment\n        </h2>\n        <ul class="space-y-1 text-slate-500">'
    env_start = html.index(env_marker) + len(env_marker)
    env_end = html.index("</ul>", env_start)
    html = html[:env_start] + args.env_html + html[env_end:]

    # localStorage memo namespace.
    html = html.replace(f"vmemkv-{args.template_id}-memo-", f"vmemkv-{args.report_id}-memo-")

    # Winners matrix table body.
    tbody_marker = '<tbody class="divide-y divide-slate-100">\n<tr class="hover:bg-indigo-50/10 transition-colors">'
    tbody_start = html.index(tbody_marker)
    tbody_content_start = tbody_start + len('<tbody class="divide-y divide-slate-100">\n')
    tbody_end = html.index("</tbody>", tbody_content_start)
    html = html[:tbody_content_start] + render_winners_matrix_html(winners_rows) + "\n" + html[tbody_end:]

    # Tier 1 / Tier 1+2 reorganize-duration summary grid: dropped as 100% redundant with the
    # per-tab "Reorganize Duration vs. Corpus Size" chart (removed once, so a no-op if the
    # template already lacks it).
    grid_open_marker = '      <div class="grid grid-cols-1 lg:grid-cols-2 gap-8">'
    grid_close_marker = '</section>\n      </div>\n    </div>\n'
    if grid_open_marker in html:
        grid_start = html.index(grid_open_marker)
        grid_close_start = html.index(grid_close_marker, grid_start)
        # Keep the trailing "    </div>\n" (tab-summary's own closing tag); only the grid + its
        # two <section> children are being removed.
        grid_end = grid_close_start + len('</section>\n      </div>\n')
        html = html[:grid_start] + html[grid_end:]

    # Corpus-Size Invariance / Churn-Ratio Scaling summary sections: replace the existing
    # section's table in place if the template already has it (added by a prior report round),
    # otherwise insert a fresh section right after the Workload Winners section closes.
    section_open_marker = '<section class="bg-white rounded-xl shadow-sm border border-slate-100 p-6 space-y-4">'

    def upsert_section(html, heading, icon_bg, icon_text, icon_name, title, description_html, table_html):
        if not table_html:
            return html
        section_html = f'''
      <section class="bg-white rounded-xl shadow-sm border border-slate-100 p-6 space-y-4">
        <div class="flex items-center gap-3">
          <div class="p-2 {icon_bg} {icon_text} rounded-lg">
            <i data-lucide="{icon_name}" class="w-6 h-6"></i>
          </div>
          <div>
            <h3 class="text-base font-bold text-slate-900">{title}</h3>
            <p class="text-xs text-slate-500">{description_html}</p>
          </div>
        </div>
        {table_html}
      </section>
'''
        heading_idx = html.find(heading)
        if heading_idx == -1:
            # Not present yet: insert right after the Workload Winners section closes.
            winners_section_close = html.index("</section>", tbody_content_start) + len("</section>")
            return html[:winners_section_close] + section_html + html[winners_section_close:]
        section_start = html.rindex(section_open_marker, 0, heading_idx)
        # Also consume any whitespace-only text immediately preceding the marker: without this,
        # a re-run's fixed-format replacement (below) leaves the *previous* run's own leading
        # indentation orphaned in place, accumulating a little more on every regeneration instead
        # of converging -- confirmed via a 3x-idempotency check while adding this section.
        ws_start = section_start
        while ws_start > 0 and html[ws_start - 1] in " \t\n":
            ws_start -= 1
        section_end = html.index("</section>", heading_idx) + len("</section>")
        return html[:ws_start] + "\n" + section_html.strip("\n") + html[section_end:]

    html = upsert_section(
        html,
        heading="Insert vs. Checkpoint() Throughput",
        icon_bg="bg-indigo-50", icon_text="text-indigo-600", icon_name="gauge",
        title="Insert vs. Checkpoint() Throughput",
        description_html='checkpoint() only durabilizes the tail since the last cycle (cost tracks churn, not corpus size), so the operationally relevant question is whether its steady-state throughput (records/sec, measured at churn_ratio=0.25 to isolate the marginal per-record cost from checkpoint()\'s fixed per-call setup overhead -- see run_checkpoint_throughput_probe.sh) can keep up with the sustained Insert rate generating that churn. Same comparison also plotted per-tab (Insert\'s 1/4/16/32-thread line vs. a flat checkpoint() throughput reference line).',
        table_html=render_checkpoint_vs_insert_table_html(checkpoint_throughput_data, raw_data),
    )
    html = upsert_section(
        html,
        heading="Insert vs. Reorganize() Throughput",
        icon_bg="bg-violet-50", icon_text="text-violet-600", icon_name="gauge",
        title="Insert vs. Reorganize() Throughput",
        description_html='reorganize() (T1-only, never touches T2) rebuilds the whole T1 structure every call -- cost tracks corpus size, not churn. Full-corpus throughput (records/sec, the T1-only corpus-size sweep\'s own ratio=100% point) compared against the sustained Insert rate that grew that corpus. Same comparison also plotted per-tab. Isolated (no concurrent writers).',
        table_html=render_reorg_vs_insert_table_html(reorg_throughput_data, raw_data),
    )
    html = upsert_section(
        html,
        heading="Maintenance Operations: Concurrent-Write Contention",
        icon_bg="bg-rose-50", icon_text="text-rose-600", icon_name="swords",
        title="Maintenance Operations: Concurrent-Write Contention",
        description_html='Both maintenance operations (checkpoint(), reorganize()) side by side: how much does write throughput degrade while each runs concurrently (32 writer threads, full corpus), and how long does the operation itself take under that contention versus in isolation (see the Insert-vs-throughput tables above for the isolated numbers alone). reorganize()\'s own duration is often under a millisecond even at full corpus size (T1-only, in-memory) -- flagged inline where its concurrent-TPS figure is likely dominated by measurement noise rather than a real effect.',
        table_html=render_maintenance_contention_html(maintenance_contention_data),
    )

    # Per-tab "Insert vs. X() Throughput" chart configs: one new canvas + section per tab, plus
    # the JS chart-config function and its initCharts() wiring. Both insertions below are guarded
    # so a re-run against an already-migrated template is a no-op.
    _VS_INSERT_CONFIG_JS = """    function %(func_name)s(valSizeKey) {
      const %(var)s = %(data_var)s[valSizeKey];
      const insertSeries = rawData[valSizeKey] && rawData[valSizeKey]['Insert'] && rawData[valSizeKey]['Insert']['+Inline'];
      if (!%(var)s || !insertSeries) return null;
      const threadLabels = [1, 4, 16, 32];
      return {
        type: 'line',
        data: {
          labels: threadLabels,
          datasets: [
            {
              label: 'Insert (+Inline)',
              data: insertSeries,
              borderColor: '#6366f1',
              backgroundColor: 'transparent',
              borderWidth: 2,
              tension: 0.2, fill: false,
              pointRadius: 3.5,
            },
            {
              label: '%(series_label)s',
              data: threadLabels.map(() => %(var)s.records_per_sec),
              borderColor: '%(color)s',
              backgroundColor: 'transparent',
              borderWidth: 2,
              borderDash: [6, 4],
              pointRadius: 0,
              fill: false,
            },
          ],
        },
        options: {
          responsive: true, maintainAspectRatio: false,
          animation: { duration: 900, easing: 'easeInOutQuart' },
          plugins: {
            legend: { position:'bottom', labels:{ boxWidth:12, font:{size:12,family:'Inter',weight:'500'}, usePointStyle:true } },
            tooltip: {
              mode: 'index', intersect: false,
              backgroundColor: 'rgba(15,23,42,0.95)',
              titleFont: {size:12,family:'Inter',weight:'bold'},
              bodyFont: {size:11,family:'Inter'},
              padding:10, cornerRadius:8,
              callbacks: {
                title: items => `${items[0].label} threads`,
                label: ctx => ` ${ctx.dataset.label}: ${formatVal(ctx.raw)}/s`
              }
            }
          },
          scales: {
            x: {
              title: { display:true, text:'Threads', font:{size:11,family:'Inter'}, color:'#64748b' },
              grid: { color:'rgba(100,116,139,0.08)' },
              ticks: { font:{size:10,family:'Inter'}, color:'#94a3b8' }
            },
            y: {
              type: 'logarithmic',
              title: { display:true, text:'Throughput (records/sec, log scale)', font:{size:11,family:'Inter'}, color:'#64748b' },
              grid: { color:'rgba(100,116,139,0.08)' },
              ticks: { font:{size:10,family:'Inter'}, color:'#94a3b8', callback: v => formatVal(v) }
            }
          }
        }
      };
    }

    """

    if "function makeCheckpointVsInsertConfig(" not in html:
        checkpoint_config_js = _VS_INSERT_CONFIG_JS % {
            "func_name": "makeCheckpointVsInsertConfig", "var": "cp", "data_var": "checkpointThroughputData",
            "series_label": "checkpoint() steady-state", "color": "#059669",
        }
        html = html.replace("    function initCharts() {", checkpoint_config_js + "function initCharts() {")

    if "function makeReorgVsInsertConfig(" not in html:
        if "    function initCharts() {" not in html:
            raise RuntimeError("initCharts() anchor not found for reorg chart config insertion -- template drifted")
        reorg_config_js = _VS_INSERT_CONFIG_JS % {
            "func_name": "makeReorgVsInsertConfig", "var": "rg", "data_var": "reorgThroughputData",
            "series_label": "reorganize() full-corpus", "color": "#7c3aed",
        }
        html = html.replace("    function initCharts() {", reorg_config_js + "function initCharts() {")

    if "-checkpoint-throughput'" not in html:
        wiring_marker = """        const reorgCanvas = document.getElementById('chart-' + reorgEid);
        if (reorgCanvas) {
          const cfg = makeReorgScalingConfig(valSize);
          if (cfg) new Chart(reorgCanvas, cfg);
        }
      }
    }"""
        wiring_new = """        const reorgCanvas = document.getElementById('chart-' + reorgEid);
        if (reorgCanvas) {
          const cfg = makeReorgScalingConfig(valSize);
          if (cfg) new Chart(reorgCanvas, cfg);
        }
        const cpEid = valSize.toLowerCase().replace(/-/g,'_') + '-checkpoint-throughput';
        const cpCanvas = document.getElementById('chart-' + cpEid);
        if (cpCanvas) {
          const cfg2 = makeCheckpointVsInsertConfig(valSize);
          if (cfg2) new Chart(cpCanvas, cfg2);
        }
      }
    }"""
        if wiring_marker not in html:
            raise RuntimeError("initCharts() reorg-scaling wiring marker not found -- template drifted")
        html = html.replace(wiring_marker, wiring_new)

    if "-reorg-throughput'" not in html:
        reorg_wiring_marker = """        const cpEid = valSize.toLowerCase().replace(/-/g,'_') + '-checkpoint-throughput';
        const cpCanvas = document.getElementById('chart-' + cpEid);
        if (cpCanvas) {
          const cfg2 = makeCheckpointVsInsertConfig(valSize);
          if (cfg2) new Chart(cpCanvas, cfg2);
        }
      }
    }"""
        reorg_wiring_new = """        const cpEid = valSize.toLowerCase().replace(/-/g,'_') + '-checkpoint-throughput';
        const cpCanvas = document.getElementById('chart-' + cpEid);
        if (cpCanvas) {
          const cfg2 = makeCheckpointVsInsertConfig(valSize);
          if (cfg2) new Chart(cpCanvas, cfg2);
        }
        const rgEid = valSize.toLowerCase().replace(/-/g,'_') + '-reorg-throughput';
        const rgCanvas = document.getElementById('chart-' + rgEid);
        if (rgCanvas) {
          const cfg4 = makeReorgVsInsertConfig(valSize);
          if (cfg4) new Chart(rgCanvas, cfg4);
        }
      }
    }"""
        if reorg_wiring_marker not in html:
            raise RuntimeError("initCharts() checkpoint-throughput wiring marker not found -- template drifted")
        html = html.replace(reorg_wiring_marker, reorg_wiring_new)

    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        slug = scenario_key.lower().replace("-", "_")
        anchor = f'id="memo-{slug}-checkpoint-throughput"'
        if anchor in html:
            continue
        reorg_memo_anchor = f'id="memo-{slug}-reorg-scaling"'
        if reorg_memo_anchor not in html:
            continue
        section_html = f'''
      <div class="mt-12 border-t border-slate-200 pt-8 space-y-6">
        <div class="flex items-center gap-3">
          <div class="w-2 h-6 bg-emerald-500 rounded-full"></div>
          <h2 class="text-lg font-bold text-slate-900">Insert Throughput vs. Checkpoint() Steady-State Throughput</h2>
        </div>
        <p class="text-sm text-slate-500">
          <strong class="text-indigo-600">藍色</strong> = Insert スループット(1/4/16/32スレッド)。<strong class="text-emerald-600">緑色破線</strong> = <code class="bg-slate-100 px-1 rounded text-xs">checkpoint()</code> の定常状態スループット(churn_ratio=0.25での記録数/所要時間。スレッド数に依存しない一定値なので水平線)。緑の線が藍色の線を下回る = 書き込み側が生成するchurnにcheckpoint()の処理速度が追いつかない可能性を示す。
        </p>
        <div class="space-y-3">
          <div class="flex items-center justify-between border-b border-slate-100 pb-1.5">
            <h3 class="text-md font-bold text-slate-900">Throughput Comparison</h3>
            <span class="text-[11px] text-slate-400 bg-slate-100 rounded px-2.5 py-0.5">records/sec</span>
          </div>
          <div class="h-80 relative"><canvas id="chart-{slug}-checkpoint-throughput"></canvas></div>
        </div>
        <div class="space-y-1.5">
          <label class="text-[10px] font-semibold uppercase tracking-wider text-slate-400">Local Notes</label>
          <textarea id="memo-{slug}-checkpoint-throughput" oninput="saveMemo('{slug}-checkpoint-throughput', this.value)" placeholder="Checkpoint Throughput 実験データに関するメモを入力..." class="w-full text-xs p-2.5 border border-slate-200 rounded-lg focus:outline-none focus:border-indigo-500 bg-slate-50/30 resize-y h-14"></textarea>
        </div>
      </div>
'''
        anchor_idx = html.index(reorg_memo_anchor)
        close_marker = "\n      </div>\n    </div>\n"
        close_idx = html.index(close_marker, anchor_idx) + len("\n      </div>\n")
        html = html[:close_idx] + section_html.lstrip("\n") + html[close_idx:]

    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        slug = scenario_key.lower().replace("-", "_")
        anchor = f'id="memo-{slug}-reorg-throughput"'
        if anchor in html:
            continue
        checkpoint_memo_anchor = f'id="memo-{slug}-checkpoint-throughput"'
        if checkpoint_memo_anchor not in html:
            continue
        section_html = f'''
      <div class="mt-12 border-t border-slate-200 pt-8 space-y-6">
        <div class="flex items-center gap-3">
          <div class="w-2 h-6 bg-violet-500 rounded-full"></div>
          <h2 class="text-lg font-bold text-slate-900">Insert Throughput vs. Reorganize() Full-Corpus Throughput</h2>
        </div>
        <p class="text-sm text-slate-500">
          <strong class="text-indigo-600">藍色</strong> = Insert スループット(1/4/16/32スレッド)。<strong class="text-violet-600">紫色破線</strong> = <code class="bg-slate-100 px-1 rounded text-xs">reorganize()</code>(T1-only)のフルコーパススループット(T1-only コーパスサイズスイープの ratio=100% 地点。スレッド数に依存しない一定値なので水平線)。単独実行(並行書き込みなし)での比較。
        </p>
        <div class="space-y-3">
          <div class="flex items-center justify-between border-b border-slate-100 pb-1.5">
            <h3 class="text-md font-bold text-slate-900">Throughput Comparison</h3>
            <span class="text-[11px] text-slate-400 bg-slate-100 rounded px-2.5 py-0.5">records/sec</span>
          </div>
          <div class="h-80 relative"><canvas id="chart-{slug}-reorg-throughput"></canvas></div>
        </div>
        <div class="space-y-1.5">
          <label class="text-[10px] font-semibold uppercase tracking-wider text-slate-400">Local Notes</label>
          <textarea id="memo-{slug}-reorg-throughput" oninput="saveMemo('{slug}-reorg-throughput', this.value)" placeholder="Reorganize Throughput 実験データに関するメモを入力..." class="w-full text-xs p-2.5 border border-slate-200 rounded-lg focus:outline-none focus:border-indigo-500 bg-slate-50/30 resize-y h-14"></textarea>
        </div>
      </div>
'''
        anchor_idx = html.index(checkpoint_memo_anchor)
        close_marker = "\n      </div>\n    </div>\n"
        close_idx = html.index(close_marker, anchor_idx) + len("\n      </div>\n")
        html = html[:close_idx] + section_html.lstrip("\n") + html[close_idx:]

    args.out.write_text(html)
    print(f"Wrote {args.out} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
