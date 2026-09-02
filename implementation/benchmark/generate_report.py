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


def build_background_jobs_data(report_dir):
    """Reads background_jobs_in_memory.jsonl / background_jobs_ltm.jsonl
    (run_background_jobs_probe.sh via run_bench_aws_c6id.sh's run_remote_probe(), matching
    the file-per-scenario convention the retired reorg-scaling/checkpoint-throughput/
    maintenance-contention probes used) into {(job, scenario): rec}. Each rec carries
    job_elapsed_sec plus insert/update/scan_degradation_pct, measured against a fixed 1KB x
    10,000,000-record corpus (in_memory unconstrained, ltm cgroup-constrained to the same
    LTM_MEMORY_BUDGET_BYTES as the rest of the suite) -- a single reproducible reference point
    rather than a sweep across the matrix's 4 scenario/value-size combos, see
    render_background_jobs_summary_html()."""
    data = {}
    lines = []
    for fname in ["background_jobs_in_memory.jsonl", "background_jobs_ltm.jsonl"]:
        path = report_dir / fname
        if path.exists():
            lines.extend(path.read_text().splitlines())
    for line in lines:
        if not line.strip():
            continue
        rec = json.loads(line)
        # run_probe_point()'s (common/reorg_probe_common.sh) synthesized outer-timeout fallback
        # (setup itself hung, before bench_kv could print its own record) doesn't know this
        # script's "job" field -- skip rather than KeyError; the table just renders that combo
        # as n/a (build_background_jobs_data() finds no entry for it), same as a missing file.
        if "job" not in rec or "scenario" not in rec:
            continue
        data[(rec["job"], rec["scenario"])] = rec
    return data


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


def _badge_for_slowdown(pct):
    """pct: percentage drop in concurrent workload QPS vs. isolated (higher = worse). Same 3-tier
    color language as _badge_for_ratio()'s strong tiers, just collapsed to 3 steps since slowdown
    has no "better than isolated" side to distinguish."""
    if pct >= 50:
        return "bg-rose-600 text-white border-transparent shadow-sm"
    if pct >= 20:
        return "bg-amber-50 text-amber-700 border-amber-200"
    return "bg-emerald-50 text-emerald-700 border-emerald-200"


def render_background_jobs_summary_html(background_jobs_data):
    """4 rows (reorganize/in_memory, reorganize/ltm, checkpoint/in_memory, checkpoint/ltm) x 4
    columns (job duration, Insert/Update/Scan QPS degradation %% while the job runs concurrently),
    all measured against one fixed 1KB x 10,000,000-record corpus (see build_background_jobs_data()).
    Deliberately not swept across the matrix's 4 scenario/value-size combos -- this table exists to
    answer one question (how much does reorganize()/checkpoint() cost, and what does it cost
    concurrent writers/readers while it runs), not to reproduce the CRUD matrix."""
    if not background_jobs_data:
        return ""
    job_labels = {"reorganize": "reorganize()", "checkpoint": "checkpoint()"}
    scenario_labels = {"in_memory": "in-memory", "ltm": "LTM"}
    out = ['<div class="overflow-x-auto"><table class="w-full text-left border-collapse text-xs">',
           '<thead><tr class="border-b border-slate-200 bg-slate-50/50">',
           '<th class="py-2 px-3 font-bold text-slate-700">Job / Scenario</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Duration (1 call)</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Insert QPS degradation</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Update QPS degradation</th>',
           '<th class="py-2 px-3 font-bold text-slate-700">Scan QPS degradation</th>',
           "</tr></thead><tbody class=\"divide-y divide-slate-100\">"]
    for job in ["reorganize", "checkpoint"]:
        for scenario in ["in_memory", "ltm"]:
            rec = background_jobs_data.get((job, scenario))
            row_label = f'{job_labels[job]} <span class="text-slate-400">/ {scenario_labels[scenario]}</span>'
            if not rec:
                out.append(f'<tr><td class="py-2 px-3">{row_label}</td>'
                            f'<td class="py-2 px-3 text-slate-300" colspan="4">n/a</td></tr>')
                continue
            if rec.get("timed_out"):
                out.append(f'<tr><td class="py-2 px-3">{row_label}</td>'
                            f'<td class="py-2 px-3" colspan="4"><span class="text-rose-600 font-semibold">'
                            f'did not complete (timeout)</span></td></tr>')
                continue
            duration_cell = f'{rec["job_elapsed_sec"]:.3f}s'
            cells = [duration_cell]
            for key in ["insert_degradation_pct", "update_degradation_pct", "scan_degradation_pct"]:
                pct = rec.get(key)
                if pct is None:
                    cells.append('<span class="text-slate-300">n/a</span>')
                else:
                    cells.append(f'<span class="inline-flex items-center px-1.5 py-0.5 rounded text-[10px] border '
                                  f'{_badge_for_slowdown(pct)} font-bold w-fit">{pct:.0f}%</span>')
            out.append(f'<tr><td class="py-2 px-3">{row_label}</td>' +
                        "".join(f'<td class="py-2 px-3">{c}</td>' for c in cells) + '</tr>')
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
    background_jobs_data = build_background_jobs_data(args.report_dir)
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
        html, "const timelineData = {", "const workloads =",
        "const timelineData = " + json.dumps(timeline_data, indent=2) + ";\n    " +
        "const forcedEventsData = " + json.dumps(forced_events_data, indent=2) + ";\n    "
    )

    # Header title / links / description. Regex-based (not an exact previous-string match): the
    # template chain mutates its own <title>/<h1> text on every generation (each report's own
    # --title becomes baked-in literal text, not a stable marker), so matching against a fixed
    # assumed-previous string silently no-ops once any report in the chain used a custom title --
    # confirmed as the root cause of the 2026090215 report keeping 2026082819's title verbatim.
    html = re.sub(r"<title>.*?</title>", lambda m: f"<title>{args.title}</title>", html, count=1)
    html = re.sub(
        r'(<h1 class="text-3xl font-bold tracking-tight text-slate-900">).*?(</h1>)',
        lambda m: m.group(1) + args.title + m.group(2),
        html, count=1,
    )

    download_links_start = html.index('<div class="flex flex-wrap gap-2">')
    download_links_end = html.index("</div>", download_links_start)
    links = []
    for fname, label in [
        (f"results_in_memory_8B.json", "In-mem 8B"),
        (f"results_in_memory_1KB.json", "In-mem 1KB"),
        (f"results_ltm_1KB.json", "LTM 1KB"),
        (f"results_ltm_64KB.json", "LTM 64KB"),
        (f"background_jobs_in_memory.jsonl", "Background Jobs, In-Memory"),
        (f"background_jobs_ltm.jsonl", "Background Jobs, LTM"),
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
        heading="Background Jobs: reorganize() / checkpoint()",
        icon_bg="bg-rose-50", icon_text="text-rose-600", icon_name="swords",
        title="Background Jobs: reorganize() / checkpoint()",
        description_html=(
            "Fixed reference point (1KB values, 10,000,000 records; in-memory unconstrained, "
            "LTM cgroup-constrained to the same memory budget as the rest of the suite) -- "
            "not swept across the CRUD matrix's 4 scenario/value-size combos. Duration is a "
            "single call in isolation; the three degradation columns are the percentage drop in "
            "concurrent Insert/Update/Scan QPS while that one call runs, measured over a "
            "matched-duration window on both sides (see run_background_jobs_probe.sh)."
        ),
        table_html=render_background_jobs_summary_html(background_jobs_data),
    )


    args.out.write_text(html)
    print(f"Wrote {args.out} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
