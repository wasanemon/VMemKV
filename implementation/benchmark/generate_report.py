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


def render_reorg_steady_table_html(reorg_data):
    points = reorg_data.get("1KB_LTM", {}).get("t1t2_steady", [])
    if not points:
        return ""
    out = ['<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">',
           '<thead><tr class="border-b border-slate-200 bg-slate-50/50">',
           '<th class="py-2 px-3 font-bold text-slate-700">Corpus Ratio</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Corpus (keys)</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Steady-state defragment() (churn=0.01)</th>',
           "</tr></thead><tbody class=\"divide-y divide-slate-100\">"]
    for p in points:
        status = f'<span class="text-rose-600 font-semibold">≥{p["elapsed_sec"]:.0f}s (timeout)</span>' if p["timed_out"] else f'{p["elapsed_sec"]:.2f}s'
        ratio_label = f'{p["ratio"]:.0%}' if p["ratio"] is not None else "n/a"
        out.append(f'<tr><td class="py-2 px-3">{ratio_label}</td><td class="py-2 px-3">{p["key_count"]:,}</td>'
                    f'<td class="py-2 px-3">{status}</td></tr>')
    out.append("</tbody></table></div>")
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
    forced_events_data = build_forced_events_data(args.report_dir)
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
        "const timelineData = " + json.dumps(timeline_data, indent=2) + ";\n    " +
        "const forcedEventsData = " + json.dumps(forced_events_data, indent=2) + ";\n    "
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

    # "Stacking Variants" legend: same +ScanBaseSequential removal, in the static HTML this time.
    old_stacking_legend = """          <li><strong>+Prefault</strong>: +Inline + T2 Async Prefaulting</li>
          <li><strong>+ScanBaseSequential</strong>: +Prefault + base領域専用mmap(MADV_SEQUENTIAL)。本ラウンドから Get もこのmmapの高速パスを使う(全部盛り、= VMemKVStore)</li>
        </ul>"""
    new_stacking_legend = """          <li><strong>+Prefault</strong>: +Inline + T2 Async Prefaulting(全部盛り、= VMemKVStore)。旧<code class="bg-slate-100 px-1 rounded text-xs">ScanBaseSequential</code>アブレーションは実測の結果撤去され、その最適化(base領域専用mmap経路)はGet/Scan双方の標準パスへ無条件で統合済み。</li>
        </ul>"""
    if old_stacking_legend not in html:
        raise RuntimeError("Stacking Variants legend template text not found -- template drifted")
    html = html.replace(old_stacking_legend, new_stacking_legend)

    # reorg-scaling chart: add the new t1t2_steady series (corpus-size invariance sweep).
    html = html.replace(
        "const modeStyle = {\n        t1only: { label: 'T1-only', color: '#6366f1' },\n        t1t2:   { label: 'T1+T2',   color: '#e11d48' },\n      };\n      const datasets = ['t1only', 't1t2'].filter(m => rs[m] && rs[m].length).map(m => {",
        "const modeStyle = {\n        t1only: { label: 'T1-only', color: '#6366f1' },\n        t1t2:   { label: 'T1+T2',   color: '#e11d48' },\n        t1t2_steady: { label: 'T1+T2 steady (reflink/punch)', color: '#059669' },\n      };\n      const datasets = ['t1only', 't1t2', 't1t2_steady'].filter(m => rs[m] && rs[m].length).map(m => {"
    )

    # reorgLinesPlugin: previously drew one shared amber line for ALL variants' forced-reorganize
    # seconds and one shared purple line for ALL variants' forced-defragment seconds (union across
    # variants), which (a) silently dropped defragment triggers entirely (bug, now fixed) and
    # (b) made independent per-variant events look like a single aggregate cluster with no way to
    # tell which variant fired when. Now draws one line+marker per (variant, event), colored and
    # shaped by that variant (same shape as its pointStyle on the scan-ops line itself).
    old_plugin = """    const reorgLinesPlugin = {
      id: 'reorgLines',
      afterDraw(chart, _args, opts) {
        const naturalSecs = opts.reorgSecs || [];
        const forcedSecs = opts.forcedReorgSecs || [];
        if (!naturalSecs.length && !forcedSecs.length) return;
        const { ctx, chartArea:{top,bottom}, scales:{x} } = chart;
        ctx.save();
        ctx.lineWidth = 1.5;
        ctx.strokeStyle = 'rgba(99,102,241,0.4)';
        ctx.setLineDash([5,4]);
        for (const s of naturalSecs) {
          const px = x.getPixelForValue(s);
          ctx.beginPath(); ctx.moveTo(px,top); ctx.lineTo(px,bottom); ctx.stroke();
        }
        ctx.strokeStyle = 'rgba(217,119,6,0.55)';
        ctx.setLineDash([2,3]);
        for (const s of forcedSecs) {
          const px = x.getPixelForValue(s);
          ctx.beginPath(); ctx.moveTo(px,top); ctx.lineTo(px,bottom); ctx.stroke();
        }
        ctx.restore();
      }
    };
    Chart.register(reorgLinesPlugin);"""
    new_plugin = """    const variantMarkers = {
      'RocksDB':'rect', 'LMDB':'triangle', 'RocksDB-BlobDB':'rectRot',
      'Baseline':'circle', '+BF':'cross', '+Inline':'crossRot', '+Prefault':'star'
    };

    function drawMarkerShape(ctx, shape, cx, cy, size, color) {
      ctx.save();
      ctx.fillStyle = color;
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.8;
      switch (shape) {
        case 'rect':
          ctx.fillRect(cx - size, cy - size, size * 2, size * 2);
          break;
        case 'rectRot':
          ctx.beginPath();
          ctx.moveTo(cx, cy - size); ctx.lineTo(cx + size, cy);
          ctx.lineTo(cx, cy + size); ctx.lineTo(cx - size, cy);
          ctx.closePath(); ctx.fill();
          break;
        case 'triangle':
          ctx.beginPath();
          ctx.moveTo(cx, cy - size); ctx.lineTo(cx + size, cy + size); ctx.lineTo(cx - size, cy + size);
          ctx.closePath(); ctx.fill();
          break;
        case 'cross':
          ctx.beginPath();
          ctx.moveTo(cx - size, cy); ctx.lineTo(cx + size, cy);
          ctx.moveTo(cx, cy - size); ctx.lineTo(cx, cy + size);
          ctx.stroke();
          break;
        case 'crossRot':
          ctx.beginPath();
          ctx.moveTo(cx - size, cy - size); ctx.lineTo(cx + size, cy + size);
          ctx.moveTo(cx - size, cy + size); ctx.lineTo(cx + size, cy - size);
          ctx.stroke();
          break;
        case 'star': {
          const spikes = 5, outerR = size, innerR = size * 0.45;
          let rot = Math.PI / 2 * 3;
          const step = Math.PI / spikes;
          ctx.beginPath();
          ctx.moveTo(cx, cy - outerR);
          for (let i = 0; i < spikes; i++) {
            let x = cx + Math.cos(rot) * outerR, y = cy + Math.sin(rot) * outerR;
            ctx.lineTo(x, y); rot += step;
            x = cx + Math.cos(rot) * innerR; y = cy + Math.sin(rot) * innerR;
            ctx.lineTo(x, y); rot += step;
          }
          ctx.lineTo(cx, cy - outerR);
          ctx.closePath(); ctx.fill();
          break;
        }
        case 'circle':
        default:
          ctx.beginPath(); ctx.arc(cx, cy, size, 0, Math.PI * 2); ctx.fill();
          break;
      }
      ctx.restore();
    }

    const reorgLinesPlugin = {
      id: 'reorgLines',
      afterDraw(chart, _args, opts) {
        const naturalSecs = opts.reorgSecs || [];
        const variantEvents = opts.variantEvents || {};
        const variantList = opts.variantList || [];
        const hasVariantEvents = variantList.some(v => (variantEvents[v] || []).length);
        if (!naturalSecs.length && !hasVariantEvents) return;
        const { ctx, chartArea:{top,bottom}, scales:{x} } = chart;
        ctx.save();
        ctx.lineWidth = 1.5;
        ctx.strokeStyle = 'rgba(99,102,241,0.35)';
        ctx.setLineDash([5,4]);
        for (const s of naturalSecs) {
          const px = x.getPixelForValue(s);
          ctx.beginPath(); ctx.moveTo(px,top); ctx.lineTo(px,bottom); ctx.stroke();
        }
        variantList.forEach((v, vi) => {
          const events = variantEvents[v] || [];
          if (!events.length) return;
          const col = colors[v] || '#6366f1';
          const shape = variantMarkers[v] || 'circle';
          for (const ev of events) {
            const px = x.getPixelForValue(ev.sec);
            ctx.strokeStyle = col + '80';
            ctx.setLineDash(ev.kind === 'reorganize' ? [2,3] : [6,3]);
            ctx.lineWidth = 1.3;
            ctx.beginPath(); ctx.moveTo(px, top); ctx.lineTo(px, bottom); ctx.stroke();
            const markerY = top + 8 + (vi % 4) * 13;
            drawMarkerShape(ctx, shape, px, markerY, 4.5, col);
          }
        });
        ctx.restore();
      }
    };
    Chart.register(reorgLinesPlugin);"""
    if old_plugin not in html:
        raise RuntimeError("reorgLinesPlugin template text not found -- template drifted")
    html = html.replace(old_plugin, new_plugin)

    old_timeline_fn = """    function makeYcsbTimelineConfig(valSizeKey) {
      const tl = timelineData[valSizeKey];
      if (!tl || !Object.keys(tl).length) return null;
      const reorgSecs = [];
      const forcedReorgSecs = [];
      for (const data of Object.values(tl)) {
        data.forEach((d,i) => {
          if (d.t1_reorg_ops > 0 && !reorgSecs.includes(i+1)) reorgSecs.push(i+1);
          if (d.t1_forced_reorg_ops > 0 && !forcedReorgSecs.includes(i+1)) forcedReorgSecs.push(i+1);
        });
      }
      reorgSecs.sort((a,b) => a-b);
      forcedReorgSecs.sort((a,b) => a-b);
      const datasets = variantOrder.filter(v => tl[v] && tl[v].length).map(v => {
        const isRocks = rivalStores.includes(v);
        const col = colors[v] || '#6366f1';
        return {
          label: v,
          data: tl[v].map((d,i) => ({ x: i+1, y: d.scan_ops })),
          borderColor: col,
          backgroundColor: 'transparent',
          borderWidth: isRocks ? 2.5 : 1.8,
          borderDash: isRocks ? [6,6] : [],
          tension: 0.25, fill: false,
          pointStyle: 'circle', pointRadius: 0, pointHoverRadius: 4
        };
      });
      if (!datasets.length) return null;
      return {
        type: 'line',
        data: { datasets },
        options: {
          responsive: true, maintainAspectRatio: false,
          animation: { duration: 900, easing: 'easeInOutQuart' },
          plugins: {
            reorgLines: { reorgSecs, forcedReorgSecs },
            legend: { position:'bottom', labels:{ boxWidth:12, font:{size:12,family:'Inter',weight:'500'}, usePointStyle:true } },
            tooltip: {
              mode: 'index', intersect: false,
              backgroundColor: 'rgba(15,23,42,0.95)',
              titleFont: {size:12,family:'Inter',weight:'bold'},
              bodyFont: {size:11,family:'Inter'},
              padding:10, cornerRadius:8,
              callbacks: {
                title: items => {
                  const s = items[0].raw.x;
                  let suffix = '';
                  if (reorgSecs.includes(s)) suffix += '  ⟳ T1 Reorg (natural)';
                  if (forcedReorgSecs.includes(s)) suffix += '  ⚑ T1 Reorg (forced@15s)';
                  return `Second ${s}${suffix}`;
                },
                label: ctx => ` ${ctx.dataset.label}: ${formatVal(ctx.raw.y)} scan/s`
              }
            }
          },
          scales: {
            x: {
              type: 'linear', min: 1, max: 30,
              title: { display:true, text:'Elapsed Time (sec)', font:{size:11,family:'Inter'}, color:'#64748b' },
              grid: { color:'rgba(100,116,139,0.08)' },
              ticks: { stepSize:5, font:{size:10,family:'Inter'}, color:'#94a3b8' }
            },
            y: {
              title: { display:true, text:'Scan Ops / sec', font:{size:11,family:'Inter'}, color:'#64748b' },
              grid: { color:'rgba(100,116,139,0.08)' },
              ticks: { font:{size:10,family:'Inter'}, color:'#94a3b8', callback: v => formatVal(v) }
            }
          }
        }
      };
    }"""
    new_timeline_fn = """    function makeYcsbTimelineConfig(valSizeKey) {
      const tl = timelineData[valSizeKey];
      if (!tl || !Object.keys(tl).length) return null;
      const reorgSecs = [];
      for (const data of Object.values(tl)) {
        data.forEach((d,i) => {
          if (d.t1_reorg_ops > 0 && !reorgSecs.includes(i+1)) reorgSecs.push(i+1);
        });
      }
      reorgSecs.sort((a,b) => a-b);
      // Forced triggers are scheduled at t=5s (reorganize()) / t=10s & t=25s (defragment()), but
      // under sustained write contention the actual call can be delayed arbitrarily past its
      // scheduled second -- this is intentionally unguarded (cascading is itself an experiment
      // result), and can even starve later triggers out of the 30s window entirely if an earlier
      // one runs long enough. fired_sec (from forced_events, per variant, keyed by kind) is where
      // it actually landed -- kept per-variant (not unioned) so independent variants' events don't
      // look like one aggregate cluster.
      const variantEvents = {};
      const variantList = variantOrder.filter(v => tl[v] && tl[v].length);
      const fe = forcedEventsData[valSizeKey] || {};
      for (const v of variantList) {
        variantEvents[v] = (fe[v] || []).map(ev => ({ sec: ev.fired_sec, kind: ev.kind, ev }));
      }
      const datasets = variantList.map(v => {
        const isRocks = rivalStores.includes(v);
        const col = colors[v] || '#6366f1';
        return {
          label: v,
          data: tl[v].map((d,i) => ({ x: i+1, y: d.scan_ops })),
          borderColor: col,
          backgroundColor: 'transparent',
          borderWidth: isRocks ? 2.5 : 1.8,
          borderDash: isRocks ? [6,6] : [],
          tension: 0.25, fill: false,
          pointStyle: variantMarkers[v] || 'circle', pointRadius: 0, pointHoverRadius: 4
        };
      });
      if (!datasets.length) return null;
      return {
        type: 'line',
        data: { datasets },
        options: {
          responsive: true, maintainAspectRatio: false,
          animation: { duration: 900, easing: 'easeInOutQuart' },
          plugins: {
            reorgLines: { reorgSecs, variantEvents, variantList },
            legend: { position:'bottom', labels:{ boxWidth:12, font:{size:12,family:'Inter',weight:'500'}, usePointStyle:true } },
            tooltip: {
              mode: 'index', intersect: false,
              backgroundColor: 'rgba(15,23,42,0.95)',
              titleFont: {size:12,family:'Inter',weight:'bold'},
              bodyFont: {size:11,family:'Inter'},
              padding:10, cornerRadius:8,
              callbacks: {
                title: items => {
                  const s = items[0].raw.x;
                  const hits = [];
                  if (reorgSecs.includes(s)) hits.push('⟳ T1 Reorg (natural)');
                  for (const v of variantList) {
                    for (const e of (variantEvents[v] || [])) {
                      if (e.sec === s) hits.push(`⚑ ${v}: ${e.ev.kind}(sched@${e.ev.scheduled_sec}s, took ${e.ev.elapsed_sec.toFixed(1)}s)`);
                    }
                  }
                  return hits.length ? `Second ${s}  ` + hits.join('  ') : `Second ${s}`;
                },
                label: ctx => ` ${ctx.dataset.label}: ${formatVal(ctx.raw.y)} scan/s`
              }
            }
          },
          scales: {
            x: {
              type: 'linear', min: 1, max: 30,
              title: { display:true, text:'Elapsed Time (sec)', font:{size:11,family:'Inter'}, color:'#64748b' },
              grid: { color:'rgba(100,116,139,0.08)' },
              ticks: { stepSize:5, font:{size:10,family:'Inter'}, color:'#94a3b8' }
            },
            y: {
              title: { display:true, text:'Scan Ops / sec', font:{size:11,family:'Inter'}, color:'#64748b' },
              grid: { color:'rgba(100,116,139,0.08)' },
              ticks: { font:{size:10,family:'Inter'}, color:'#94a3b8', callback: v => formatVal(v) }
            }
          }
        }
      };
    }"""
    if old_timeline_fn not in html:
        raise RuntimeError("makeYcsbTimelineConfig template text not found -- template drifted")
    html = html.replace(old_timeline_fn, new_timeline_fn)

    # Static per-tab captions describing the vertical-line legend (still said "t=15s single
    # trigger" from the old schedule; now t=5s reorganize() / t=10s+25s defragment(), unguarded).
    old_caption = """          <strong class="text-indigo-600">藍色の点線</strong>: 自然発生のT1 Reorganize(Adaptive Soft Limitによる自動トリガー)。
          <strong class="text-amber-600">橘色の点線</strong>: t=15秒時点で強制的に1回実行されるT1 Reorganize。
        </p>"""
    new_caption = """          <strong class="text-indigo-600">藍色の点線</strong>: 自然発生のT1 Reorganize(Adaptive Soft Limitによる自動トリガー、全バリアント共通)。
          強制トリガーはバリアントごとに独立集計(全バリアント共通の1本の線には集約しない): t=5秒予定の<code class="bg-slate-100 px-1 rounded text-xs">reorganize()</code>(細かい点線)、t=10秒・t=25秒予定の<code class="bg-slate-100 px-1 rounded text-xs">defragment()</code>(粗い点線)を、そのバリアント自身の色+マーカー形状(スキャンQPS線をホバーした際の点と同じ形。凡例のポイント形状も対応)で描画。
          実際に発火する秒(線の位置)は予定秒とは限らない(将棋倒しに対するガード無し、書き込み負荷次第で数十秒遅延することがある)。8B In-Memoryのように挿入スループットが極端に高いワークロードでは、t=5秒予定のreorganize()自体が20秒以上かかり、後続のdefragment()トリガー(t=10s/25s)がこの30秒間に一度も発火しないまま終わることもある(この場合、線は1本しか出ない)。カーソルを線に合わせると、その秒に発火したバリアント・種類・予定秒・実発火秒・所要時間を表示。
        </p>"""
    if old_caption not in html:
        raise RuntimeError("YCSB-E caption template text not found -- template drifted")
    if html.count(old_caption) != 4:
        raise RuntimeError(f"expected 4 YCSB-E caption occurrences, found {html.count(old_caption)}")
    html = html.replace(old_caption, new_caption)

    # Reorg-scaling caption: mention the new t1t2_steady (reflink/punch) series. Both 藍色/赤色 use
    # the SAME current checkpoint_and_defragment() code -- 赤色 just measures it invoked cold (no
    # prior checkpoint to reflink from, i.e. bootstrap cost, always O(N)); 緑色 is the 2nd-or-later
    # call against an existing prior generation (the real O(diff) steady-state this design exists
    # for). There is no separate "old design" being measured here.
    old_reorg_caption = """          <strong class="text-indigo-600">藍色</strong> = T1-only、<strong class="text-rose-600">赤色</strong> = T1+T2。
          <strong class="text-rose-600">▲マーカー</strong>はタイムアウトを示す。
        </p>"""
    new_reorg_caption = """          <strong class="text-indigo-600">藍色</strong> = T1-only(T2は一切触らない)。<strong class="text-rose-600">赤色</strong> = T1+T2、ただし<strong>ブートストラップ</strong>(直前チェックポイントが存在しない新規コーパスへの初回<code class="bg-slate-100 px-1 rounded text-xs">defragment()</code>。reflink元が無いのでO(N)。現行のreflink+punch実装そのものを、コールド状態で計測した数値であって別実装ではない)。<strong class="text-emerald-600">緑色</strong> = T1+T2 steady(既存世代からのreflink clone + hole punch、O(diff)の本来の定常状態。churn_ratio=0.01でのコーパスサイズ不変性実験、1KB LTMタブのみ)。
          <strong class="text-rose-600">▲マーカー</strong>はタイムアウトを示す。
        </p>"""
    if old_reorg_caption not in html:
        raise RuntimeError("reorg-scaling caption template text not found -- template drifted")
    if html.count(old_reorg_caption) != 4:
        raise RuntimeError(f"expected 4 reorg-scaling caption occurrences, found {html.count(old_reorg_caption)}")
    html = html.replace(old_reorg_caption, new_reorg_caption)

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

    # Drop the Tier 1 / Tier 1+2 reorganize-duration summary cards entirely. They were found to be
    # 100% redundant with the per-tab "Reorganize Duration vs. Corpus Size" chart (same
    # reorg_scaling_*.jsonl data, avg/max-collapsed instead of shown against corpus size, which
    # is the actually interesting axis) -- and this exact card was also where a real staleness bug
    # was found (it silently carried over the *previous* report's hardcoded numbers verbatim for a
    # full cycle, since nothing in this script ever touched it). Removing the whole thing is
    # simpler and more honest than fixing numbers nobody was cross-checking against the chart
    # anyway.
    grid_open_marker = '      <div class="grid grid-cols-1 lg:grid-cols-2 gap-8">'
    grid_close_marker = '</section>\n      </div>\n    </div>\n'
    grid_start = html.index(grid_open_marker)
    grid_close_start = html.index(grid_close_marker, grid_start)
    # Keep the trailing "    </div>\n" (tab-summary's own closing tag); only the grid + its two
    # <section> children are being removed.
    grid_end = grid_close_start + len('</section>\n      </div>\n')
    html = html[:grid_start] + html[grid_end:]

    # New-experiment summary sections: insert right after the Winners Matrix section closes (so
    # final tab-summary order is Workload Winners -> Corpus-Size Invariance -> Churn-Ratio Scaling;
    # Workload Winners is already first in the template, and reorganize-duration no longer has its
    # own section per above).
    churn_html = render_churn_table_html(churn_rows)
    reorg_steady_html = render_reorg_steady_table_html(reorg_data)
    new_sections = ""
    if reorg_steady_html:
        new_sections += f'''
      <section class="bg-white rounded-xl shadow-sm border border-slate-100 p-6 space-y-4">
        <div class="flex items-center gap-3">
          <div class="p-2 bg-emerald-50 text-emerald-600 rounded-lg">
            <i data-lucide="git-commit" class="w-6 h-6"></i>
          </div>
          <div>
            <h3 class="text-base font-bold text-slate-900">Corpus-Size Invariance across Generations (new experiment)</h3>
            <p class="text-xs text-slate-500">Steady-state <code class="bg-slate-100 px-1 rounded">checkpoint_and_defragment()</code> duration (reflink clone + hole punch, O(diff), 2nd-or-later call against an existing prior generation) at fixed churn ratio (0.01), swept across corpus size. Scoped to ltm/1KB only. Same series also plotted (green) on the Reorg Scaling chart in the 1KB LTM tab, alongside the T1-only/T1+T2 bootstrap sweep (1st-ever call, no prior generation to reflink from) for comparison.</p>
          </div>
        </div>
        {reorg_steady_html}
      </section>
'''
    if churn_html:
        new_sections += f'''
      <section class="bg-white rounded-xl shadow-sm border border-slate-100 p-6 space-y-4">
        <div class="flex items-center gap-3">
          <div class="p-2 bg-indigo-50 text-indigo-600 rounded-lg">
            <i data-lucide="activity" class="w-6 h-6"></i>
          </div>
          <div>
            <h3 class="text-base font-bold text-slate-900">Churn-Ratio Scaling (new experiment)</h3>
            <p class="text-xs text-slate-500">Steady-state <code class="bg-slate-100 px-1 rounded">checkpoint_and_defragment()</code> duration at fixed corpus size (8M keys, in_memory/1KB), swept across the fraction of the corpus mutated since the last generation (churn ratio). 60s hard cap per point. One additional ltm/1KB spot check at churn_ratio=0.01. Notably, churn_ratio=0.05 -- just 400,000 of the 8M keys (5%) touched since the last checkpoint -- is already enough to blow the 60s cap; the O(diff) design's cost still scales with how much actually changed, it isn't a fixed cheap constant.</p>
          </div>
        </div>
        {churn_html}
      </section>
'''
    if new_sections:
        # Right after the Workload Winners <section>'s own closing tag (the first </section>
        # following its tbody, before the grid block that used to sit here).
        winners_section_close = html.index("</section>", tbody_content_start) + len("</section>")
        html = html[:winners_section_close] + new_sections + html[winners_section_close:]

    args.out.write_text(html)
    print(f"Wrote {args.out} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
