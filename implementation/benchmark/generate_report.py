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
            mode_points.sort(key=lambda p: p["key_count"])
        reorg_data[scenario_key] = modes
    return reorg_data


def build_checkpoint_throughput_data(report_dir):
    """Reads checkpoint_throughput_in_memory.jsonl / checkpoint_throughput_ltm.jsonl
    (run_checkpoint_throughput_probe.sh, via bench_kv --reorg-probe --mode=t1t2_steady
    --ratio=1.0 --churn-ratio=0.25 -- full corpus size, a high-but-not-maximal churn ratio that
    isolates the marginal per-record durabilization cost from checkpoint()'s fixed per-call setup
    overhead while keeping setup work and checkpoint() I/O proportionally bounded) into
    {scenario_key: {"key_count", "elapsed_sec", "records_per_sec"}}, one entry per combo."""
    data = {}
    for fname in ["checkpoint_throughput_in_memory.jsonl", "checkpoint_throughput_ltm.jsonl"]:
        path = report_dir / fname
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            if not line.strip():
                continue
            rec = json.loads(line)
            scenario_key = SCENARIO_LABELS.get((rec["scenario"], _value_size_label(rec.get("value_size"))))
            if scenario_key is None or rec.get("timed_out"):
                continue
            data[scenario_key] = {
                "key_count": rec["key_count"],
                "elapsed_sec": rec["elapsed_sec"],
                "records_per_sec": rec["key_count"] / rec["elapsed_sec"],
            }
    return data


def _value_size_label(value_size):
    if isinstance(value_size, str):
        return value_size
    return {8: "8B", 1024: "1KB", 65536: "64KB"}.get(value_size, str(value_size))


def build_defrag_scaling_data(report_dir):
    """Reads defrag_scaling_in_memory.jsonl / defrag_scaling_ltm.jsonl (run_defrag_scaling_probe.sh,
    via bench_kv --reorg-probe --mode=defrag/defrag_contention) into two row lists: the
    corpus-size sweep (mode=defrag, churn_ratio=0, all 4 scenario/value_size combos) and the
    concurrent-write contention spot check (mode=defrag_contention, one point per combo).
    defragment() relocates every live record regardless of which ones changed, so its cost
    doesn't depend on churn ratio -- confirmed once via a dedicated sweep, not re-swept/re-plotted
    every round (see run_defrag_scaling_probe.sh's own comment)."""
    corpus_rows = []
    contention_rows = []
    for fname in ["defrag_scaling_in_memory.jsonl", "defrag_scaling_ltm.jsonl"]:
        path = report_dir / fname
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            if not line.strip():
                continue
            rec = json.loads(line)
            scenario_val = f'{rec["scenario"]}_{_value_size_label(rec.get("value_size"))}'
            if rec.get("mode") == "defrag":
                if (rec.get("churn_ratio") or 0) == 0:
                    corpus_rows.append({
                        "scenario_val": scenario_val,
                        "ratio": rec.get("ratio"),
                        "key_count": rec["key_count"],
                        "elapsed_sec": rec["elapsed_sec"],
                        "timed_out": rec["timed_out"],
                    })
            elif rec.get("mode") == "defrag_contention":
                contention_rows.append({
                    "scenario_val": scenario_val,
                    "isolated_write_tps": rec.get("isolated_write_tps"),
                    "concurrent_write_tps": rec.get("concurrent_write_tps"),
                    "defrag_elapsed_sec": rec.get("defrag_elapsed_sec", rec.get("elapsed_sec")),
                    "timed_out": rec["timed_out"],
                    "failure_reason": rec.get("failure_reason"),
                })
    corpus_rows.sort(key=lambda r: (r["scenario_val"], r["ratio"] or 0))
    return corpus_rows, contention_rows


def render_defrag_scaling_html(corpus_rows, contention_rows):
    if not corpus_rows and not contention_rows:
        return ""
    out = []
    if corpus_rows:
        out.append('<h4 class="text-xs font-bold text-slate-700 uppercase tracking-wide">Corpus-Size Sweep (churn=0)</h4>')
        out.append('<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">'
                    '<thead><tr class="border-b border-slate-200 bg-slate-50/50">'
                    '<th class="py-2 px-3 font-bold text-slate-700">Scenario/Value</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Corpus Ratio</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Corpus (keys)</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">defragment()</th>'
                    '</tr></thead><tbody class="divide-y divide-slate-100">')
        for r in corpus_rows:
            status = f'<span class="text-rose-600 font-semibold">≥{r["elapsed_sec"]:.0f}s (timeout)</span>' if r["timed_out"] else f'{r["elapsed_sec"]:.2f}s'
            ratio_label = f'{r["ratio"]:.0%}' if r["ratio"] is not None else "n/a"
            out.append(f'<tr><td class="py-2 px-3">{r["scenario_val"]}</td><td class="py-2 px-3">{ratio_label}</td>'
                        f'<td class="py-2 px-3">{r["key_count"]:,}</td><td class="py-2 px-3">{status}</td></tr>')
        out.append("</tbody></table></div>")
    if contention_rows:
        out.append('<h4 class="text-xs font-bold text-slate-700 uppercase tracking-wide mt-4">Concurrent-Write Contention Spot Check (32 writer threads, ratio=1.0)</h4>')
        out.append('<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">'
                    '<thead><tr class="border-b border-slate-200 bg-slate-50/50">'
                    '<th class="py-2 px-3 font-bold text-slate-700">Scenario/Value</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Isolated Write TPS</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Concurrent Write TPS</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">Slowdown</th>'
                    '<th class="py-2 px-3 font-bold text-slate-700">defragment() duration</th>'
                    '</tr></thead><tbody class="divide-y divide-slate-100">')
        for r in contention_rows:
            if r["failure_reason"]:
                out.append(f'<tr><td class="py-2 px-3">{r["scenario_val"]}</td>'
                            f'<td class="py-2 px-3 text-slate-300" colspan="3">n/a</td>'
                            f'<td class="py-2 px-3"><span class="text-rose-600 font-semibold">did not complete ({r["failure_reason"]})</span></td></tr>')
                continue
            iso = r["isolated_write_tps"]
            conc = r["concurrent_write_tps"]
            slowdown = f'{(1 - conc / iso) * 100:.0f}%' if iso else "n/a"
            defrag_status = f'<span class="text-rose-600 font-semibold">≥{r["defrag_elapsed_sec"]:.0f}s (timeout)</span>' if r["timed_out"] else f'{r["defrag_elapsed_sec"]:.2f}s'
            out.append(f'<tr><td class="py-2 px-3">{r["scenario_val"]}</td>'
                        f'<td class="py-2 px-3">{iso:,.0f}/s</td><td class="py-2 px-3">{conc:,.0f}/s</td>'
                        f'<td class="py-2 px-3">{slowdown}</td><td class="py-2 px-3">{defrag_status}</td></tr>')
        out.append("</tbody></table></div>")
    return "\n".join(out)


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


def render_checkpoint_vs_insert_table_html(checkpoint_throughput_data, raw_data):
    if not checkpoint_throughput_data:
        return ""
    idx32 = THREADS.index(32)
    out = ['<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">',
           '<thead><tr class="border-b border-slate-200 bg-slate-50/50">',
           '<th class="py-2 px-3 font-bold text-slate-700">Scenario/Value</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Insert (32 threads)</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">checkpoint() steady-state</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Verdict</th>',
           "</tr></thead><tbody class=\"divide-y divide-slate-100\">"]
    for scenario_key in ["8B_In-Memory", "1KB_In-Memory", "1KB_LTM", "64KB_LTM"]:
        cp = checkpoint_throughput_data.get(scenario_key)
        insert_series = raw_data.get(scenario_key, {}).get("Insert", {}).get("+Inline")
        if not cp or not insert_series or insert_series[idx32] is None:
            continue
        insert_rate = insert_series[idx32]
        cp_rate = cp["records_per_sec"]
        ratio = insert_rate / cp_rate if cp_rate else float("inf")
        label_text, badge_class = _badge_for_checkpoint_headroom(ratio)
        out.append(f'<tr><td class="py-2 px-3">{scenario_key}</td>'
                    f'<td class="py-2 px-3">{insert_rate:,.0f}/s</td>'
                    f'<td class="py-2 px-3">{cp_rate:,.0f}/s</td>'
                    f'<td class="py-2 px-3"><span class="inline-flex items-center px-1.5 py-0.5 rounded text-[10px] border {badge_class} font-bold w-fit">{label_text} ({ratio:.2f}x)</span></td></tr>')
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
    defrag_corpus_rows, defrag_contention_rows = build_defrag_scaling_data(args.report_dir)
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
        "const checkpointThroughputData = " + json.dumps(checkpoint_throughput_data, indent=2) + ";\n    "
    )

    # checkpoint()/defragment() mislabeling: churn-scaling, reorg-scaling and YCSB-E's forced
    # triggers only ever call checkpoint(), never defragment() -- but older report rounds
    # (predating that split) still describe them as defragment()/reflink+punch/O(N)/O(diff). Best
    # effort (not asserted) since exact wording has drifted across rounds; a no-op once fixed.
    html = html.replace(
        "t=10秒・t=25秒予定の<code class=\"bg-slate-100 px-1 rounded text-xs\">defragment()</code>(粗い点線)",
        "t=10秒・t=25秒予定の<code class=\"bg-slate-100 px-1 rounded text-xs\">checkpoint()</code>(粗い点線)",
    )
    html = html.replace("後続のdefragment()トリガー", "後続のcheckpoint()トリガー")
    html = html.replace("t=10s & t=25s (defragment())", "t=10s & t=25s (checkpoint())")
    old_reorg_caption_variants = [
        """<strong class="text-indigo-600">藍色</strong> = T1-only(T2は一切触らない)。<strong class="text-rose-600">赤色</strong> = T1+T2、ただし<strong>ブートストラップ</strong>(直前チェックポイントが存在しない新規コーパスへの初回<code class="bg-slate-100 px-1 rounded text-xs">defragment()</code>。reflink元が無いのでO(N)。現行のreflink+punch実装そのものを、コールド状態で計測した数値であって別実装ではない)。<strong class="text-emerald-600">緑色</strong> = T1+T2 steady(既存世代からのreflink clone + hole punch、O(diff)の本来の定常状態。churn_ratio=0.01でのコーパスサイズ不変性実験、1KB LTMタブのみ)。""",
        """<strong class="text-indigo-600">藍色</strong> = T1-only、<strong class="text-rose-600">赤色</strong> = T1+T2。""",
    ]
    new_reorg_caption = """<strong class="text-indigo-600">藍色</strong> = T1-only(T2は一切触らない)。<strong class="text-rose-600">赤色</strong> = T1+T2、<strong>ブートストラップ</strong>(直前チェックポイントが存在しない新規コーパスへの初回<code class="bg-slate-100 px-1 rounded text-xs">checkpoint()</code>)。<strong class="text-emerald-600">緑色</strong> = T1+T2 steady(既にチェックポイント済みのコーパスへの2回目以降の<code class="bg-slate-100 px-1 rounded text-xs">checkpoint()</code>。churn_ratio=0.01でのコーパスサイズ不変性実験、1KB LTMタブのみ)。"""
    for old_variant in old_reorg_caption_variants:
        html = html.replace(old_variant, new_reorg_caption)

    # checkpoint()'s bootstrap/steady cost no longer has its own chart series -- it only durabilizes
    # the tail since the last cycle (proportional to churn, not corpus size), so a corpus-size sweep
    # was never the right axis for it; see the new Insert-vs-checkpoint-throughput chart/table
    # instead (run_checkpoint_throughput_probe.sh). This chart goes back to being reorganize()-only,
    # T1-only, the same operation it always measured. Best effort (not asserted), a no-op once fixed.
    html = html.replace(
        "Reorganize / Checkpoint Duration vs. Corpus Size (T1-only reorganize() vs T1+T2 checkpoint())",
        "reorganize() Duration vs. Corpus Size (T1-only)",
    )
    html = html.replace(
        """const modeStyle = {
        t1only: { label: 'Reorganize (T1-only)', color: '#6366f1' },
        t1t2:   { label: 'Checkpoint, bootstrap (T1+T2)', color: '#e11d48' },
        t1t2_steady: { label: 'Checkpoint, steady (T1+T2)', color: '#059669' },
      };
      const datasets = ['t1only', 't1t2', 't1t2_steady'].filter(m => rs[m] && rs[m].length).map(m => {""",
        """const modeStyle = {
        t1only: { label: 'Reorganize (T1-only)', color: '#6366f1' },
      };
      const datasets = ['t1only'].filter(m => rs[m] && rs[m].length).map(m => {""",
    )
    old_reorg_caption_simplified_variants = [
        """<strong class="text-indigo-600">藍色</strong> = T1-only(T2は一切触らない)。<strong class="text-rose-600">赤色</strong> = T1+T2、<strong>ブートストラップ</strong>(直前チェックポイントが存在しない新規コーパスへの初回<code class="bg-slate-100 px-1 rounded text-xs">checkpoint()</code>)。<strong class="text-emerald-600">緑色</strong> = T1+T2 steady(既にチェックポイント済みのコーパスへの2回目以降の<code class="bg-slate-100 px-1 rounded text-xs">checkpoint()</code>。churn_ratio=0.01でのコーパスサイズ不変性実験、1KB LTMタブのみ)。""",
    ]
    new_reorg_caption_simplified = """reorganize() は T1(インメモリインデックス)のみを対象とし、T2には一切触れない。"""
    for old_variant in old_reorg_caption_simplified_variants:
        html = html.replace(old_variant, new_reorg_caption_simplified)

    # Older report rounds' already-generated HTML says churn_ratio=1.0 in this caption; the probe
    # measures at churn_ratio=0.25 (see run_checkpoint_throughput_probe.sh). Best effort (not
    # asserted), a no-op once fixed.
    html = html.replace("checkpoint()</code> の定常状態スループット(churn_ratio=1.0での記録数/所要時間",
                         "checkpoint()</code> の定常状態スループット(churn_ratio=0.25での記録数/所要時間")

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
        (f"defrag_scaling_in_memory.jsonl", "Defrag Scaling, In-Memory"),
        (f"defrag_scaling_ltm.jsonl", "Defrag Scaling, LTM"),
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
        section_end = html.index("</section>", heading_idx) + len("</section>")
        return html[:section_start] + section_html.strip("\n") + html[section_end:]

    def remove_section(html, heading):
        heading_idx = html.find(heading)
        if heading_idx == -1:
            return html
        section_start = html.rindex(section_open_marker, 0, heading_idx)
        section_end = html.index("</section>", heading_idx) + len("</section>")
        return html[:section_start] + html[section_end:]

    # Both superseded by the Insert-vs-checkpoint-throughput chart/table below: checkpoint()'s
    # cost tracks churn, not corpus size, so a corpus-size sweep (bootstrap-only, or steady-state
    # scoped to ltm/1KB) was never the right measurement for "does checkpoint keep up".
    html = remove_section(html, "Corpus-Size Invariance across Generations (new experiment)")
    html = remove_section(html, "Churn-Ratio Scaling (new experiment)")

    html = upsert_section(
        html,
        heading="Insert vs. Checkpoint() Throughput (new experiment)",
        icon_bg="bg-indigo-50", icon_text="text-indigo-600", icon_name="gauge",
        title="Insert vs. Checkpoint() Throughput (new experiment)",
        description_html='checkpoint() only durabilizes the tail since the last cycle (cost tracks churn, not corpus size), so the operationally relevant question is whether its steady-state throughput (records/sec, measured at churn_ratio=0.25 to isolate the marginal per-record cost from checkpoint()\'s fixed per-call setup overhead -- see run_checkpoint_throughput_probe.sh) can keep up with the sustained Insert rate generating that churn. Same comparison also plotted per-tab (Insert\'s 1/4/16/32-thread line vs. a flat checkpoint() throughput reference line).',
        table_html=render_checkpoint_vs_insert_table_html(checkpoint_throughput_data, raw_data),
    )
    html = upsert_section(
        html,
        heading="Defragment Scaling & Contention (new experiment)",
        icon_bg="bg-amber-50", icon_text="text-amber-600", icon_name="scissors",
        title="Defragment Scaling & Contention (new experiment)",
        description_html='<code class="bg-slate-100 px-1 rounded">defragment()</code> duration against an already-<code class="bg-slate-100 px-1 rounded">checkpoint()</code>ed corpus (a realistic pre-defragment state): a corpus-size sweep at churn_ratio=0 across all 4 scenario/value-size combos, and a concurrent-write contention spot check (isolated vs. concurrent write throughput while defragment() runs). defragment() relocates every live record regardless of which ones changed, so its cost tracks corpus size, not churn ratio -- confirmed once via a dedicated in_memory/1KB churn-ratio sweep (durations stayed flat across churn_ratio 0-1.0 at a fixed corpus size), not re-swept every round since that finding doesn\'t change from run to run.',
        table_html=render_defrag_scaling_html(defrag_corpus_rows, defrag_contention_rows),
    )

    # Per-tab "Insert vs. Checkpoint() Throughput" chart: one new canvas + section per tab,
    # inserted right after the existing Reorg Scaling Probe section (same sibling-block style),
    # plus the JS chart-config function and its initCharts() wiring. All three insertions are
    # guarded so a re-run against an already-migrated template is a no-op.
    if "function makeCheckpointVsInsertConfig(" not in html:
        checkpoint_config_js = """    function makeCheckpointVsInsertConfig(valSizeKey) {
      const cp = checkpointThroughputData[valSizeKey];
      const insertSeries = rawData[valSizeKey] && rawData[valSizeKey]['Insert'] && rawData[valSizeKey]['Insert']['+Inline'];
      if (!cp || !insertSeries) return null;
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
              label: 'checkpoint() steady-state',
              data: threadLabels.map(() => cp.records_per_sec),
              borderColor: '#059669',
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
        html = html.replace("    function initCharts() {", checkpoint_config_js + "function initCharts() {")

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

    args.out.write_text(html)
    print(f"Wrote {args.out} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
