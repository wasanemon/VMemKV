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


def build_timeline_data(report_dir):
    timeline_data = {}
    for (scenario, val_size), scenario_key in SCENARIO_LABELS.items():
        variants = {}
        for f in report_dir.glob(f"ycsb_e_timeline_{scenario}_*.json"):
            d = json.loads(f.read_text())
            store = d["store"]
            variant = d["variant"]
            file_val_size = d["value_size"]
            if file_val_size != val_size:
                continue
            base_store = store.split("-", 1)[0] if store.startswith("VMemKV") else store
            label = STORE_VARIANT_TO_LABEL.get((base_store, variant))
            if label is None:
                continue
            variants[label] = d["timeline"]
        timeline_data[scenario_key] = variants
    return timeline_data


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
                "elapsed_sec": rec["elapsed_sec"],
                "timed_out": rec["timed_out"],
            })
        for mode_points in modes.values():
            mode_points.sort(key=lambda p: p["key_count"])
        reorg_data[scenario_key] = modes
    return reorg_data


def build_churn_scaling_rows(report_dir):
    rows = []
    for fname in sorted(report_dir.glob("churn_scaling_*.jsonl")):
        scenario_val = fname.stem.replace("churn_scaling_", "")
        for line in fname.read_text().splitlines():
            if not line.strip():
                continue
            rec = json.loads(line)
            rows.append({
                "scenario_val": scenario_val,
                "churn_ratio": rec["churn_ratio"],
                "key_count": rec["key_count"],
                "elapsed_sec": rec["elapsed_sec"],
                "timed_out": rec["timed_out"],
            })
    return rows


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
            win = ratio >= 1.0
            badge_class = "bg-emerald-600 text-white border-transparent" if win else "bg-rose-50 text-rose-700 border-rose-100"
            badge_text = f"✅ WIN ({ratio:.2f}x)" if win else f"⚠️ LOSE ({ratio:.2f}x)"
            out.append(f'''<td class="py-3.5 px-4 transition-colors">
  <div class="flex flex-col gap-0.5">
    <span class="inline-flex items-center px-1.5 py-0.5 rounded text-[10px] border {badge_class} font-bold shadow-sm w-fit">
      {badge_text}
    </span>
    <span class="text-[11px] text-slate-500 font-medium">vmemkv ({cell["vmemkv_label"]})</span>
    <span class="text-[10px] text-slate-400">vs {cell["rival_label"]}</span>
  </div>
</td>''')
        out.append("</tr>")
    return "\n".join(out)


def render_churn_table_html(rows):
    if not rows:
        return ""
    out = ['<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">',
           '<thead><tr class="border-b border-slate-200 bg-slate-50/50">',
           '<th class="py-2 px-3 font-bold text-slate-700">Scenario/Value</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Churn Ratio</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Corpus (keys)</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">defragment() steady-state</th>',
           "</tr></thead><tbody class=\"divide-y divide-slate-100\">"]
    for r in rows:
        status = f'<span class="text-rose-600 font-semibold">≥{r["elapsed_sec"]:.0f}s (timeout)</span>' if r["timed_out"] else f'{r["elapsed_sec"]:.2f}s'
        out.append(f'<tr><td class="py-2 px-3">{r["scenario_val"]}</td><td class="py-2 px-3">{r["churn_ratio"]}</td>'
                    f'<td class="py-2 px-3">{r["key_count"]:,}</td><td class="py-2 px-3">{status}</td></tr>')
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
    reorg_data = build_reorg_scaling_data(args.report_dir)
    churn_rows = build_churn_scaling_rows(args.report_dir)
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
        "const timelineData = " + json.dumps(timeline_data, indent=2) + ";\n    "
    )
    html = replace_block(
        html, "const reorgScalingData = {", "const workloads =",
        "const reorgScalingData = " + json.dumps(reorg_data, indent=2) + ";\n    "
    )

    # variantOrder/colors: this run has no +ScanBaseSequential ablation (removed upstream).
    html = html.replace(
        "const colors = {\n      'RocksDB':'#64748b', 'LMDB':'#10b981', 'RocksDB-BlobDB':'#a855f7',\n      'Baseline':'#94a3b8',\n      '+BF':'#f59e0b', '+Inline':'#6366f1', '+Prefault':'#ec4899', '+ScanBaseSequential':'#0ea5e9'\n    };\n    const rivalStores = ['RocksDB','LMDB','RocksDB-BlobDB'];\n    const variantOrder = ['RocksDB','LMDB','RocksDB-BlobDB','Baseline','+BF','+Inline','+Prefault','+ScanBaseSequential'];",
        "const colors = " + json.dumps(COLORS) + ";\n    const rivalStores = " + json.dumps(RIVAL_STORES) +
        ";\n    const variantOrder = " + json.dumps(VARIANT_ORDER) + ";"
    )

    # reorg-scaling chart: add the new t1t2_steady series (corpus-size invariance sweep).
    html = html.replace(
        "const modeStyle = {\n        t1only: { label: 'T1-only', color: '#6366f1' },\n        t1t2:   { label: 'T1+T2',   color: '#e11d48' },\n      };\n      const datasets = ['t1only', 't1t2'].filter(m => rs[m] && rs[m].length).map(m => {",
        "const modeStyle = {\n        t1only: { label: 'T1-only', color: '#6366f1' },\n        t1t2:   { label: 'T1+T2',   color: '#e11d48' },\n        t1t2_steady: { label: 'T1+T2 steady (reflink/punch)', color: '#059669' },\n      };\n      const datasets = ['t1only', 't1t2', 't1t2_steady'].filter(m => rs[m] && rs[m].length).map(m => {"
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
        (f"churn_scaling_in_memory_1KB.jsonl", "Churn Scaling, In-Mem 1KB"),
        (f"churn_scaling_ltm_1KB.jsonl", "Churn Scaling, LTM 1KB spot check"),
    ]:
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

    # Churn-scaling table: insert as a new section right after the Winners Matrix section closes.
    churn_html = render_churn_table_html(churn_rows)
    if churn_html:
        churn_section = f'''
      <section class="bg-white rounded-xl shadow-sm border border-slate-100 p-6 space-y-4">
        <div class="flex items-center gap-3">
          <div class="p-2 bg-indigo-50 text-indigo-600 rounded-lg">
            <i data-lucide="activity" class="w-6 h-6"></i>
          </div>
          <div>
            <h3 class="text-base font-bold text-slate-900">Churn-Ratio Scaling (new experiment)</h3>
            <p class="text-xs text-slate-500">Steady-state <code class="bg-slate-100 px-1 rounded">checkpoint_and_defragment()</code> duration at fixed corpus size (8M keys, in_memory/1KB), swept across the fraction of the corpus mutated since the last generation (churn ratio). 60s hard cap per point. One additional ltm/1KB spot check at churn_ratio=0.01.</p>
          </div>
        </div>
        {churn_html}
      </section>
'''
        summary_section_marker = html.index('<div id="tab-summary" class="tab-content active space-y-8">') + len('<div id="tab-summary" class="tab-content active space-y-8">')
        html = html[:summary_section_marker] + churn_section + html[summary_section_marker:]

    args.out.write_text(html)
    print(f"Wrote {args.out} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
