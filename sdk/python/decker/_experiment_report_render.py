"""Portable, script-free presentation of verified experiment report data."""

import csv
import html
import io
from pathlib import Path
import os
from urllib.parse import quote

from .experiment_index import METRICS
from .experiments import canonical


def _text(value):
    if value is None:
        return "—"
    if type(value) is float:
        return f"{value:.7g}"
    return str(value)


def _escape(value):
    return html.escape(_text(value), quote=True)


def _csv(header, rows):
    output = io.StringIO(newline="")
    writer = csv.writer(output)
    writer.writerow(header)
    for row in rows:
        # Spreadsheet formula protection for user-controlled textual cells.
        writer.writerow(["'" + v if isinstance(v, str) and v.startswith(("=", "+", "-", "@", "\t", "\r"))
                         else v for v in row])
    return output.getvalue().encode("utf-8-sig")


def _experiment_rows(report):
    rows = []
    for label in ("current", "baseline"):
        snapshot = report[label]
        if snapshot is None:
            continue
        for row in snapshot["experiments"]:
            config, metrics, performance = row["config"], row["metrics"], row["last_batch"]
            rows.append([label, snapshot["batch_id"], snapshot["plan_id"], row["experiment_id"],
                row["attempt_id"], row["run_id"], row["status"], config["solver"], config["cloth"]["seed"],
                config["steps"], config["fixed_dt_ns"], canonical(config["cloth"]).decode(),
                *[metrics[name] if metrics else None for name in METRICS],
                *[performance[name] if performance else None for name in ("steps", "batch_steps", "wall_time_ns")],
                row["gpu_timestamp_ns"], row["error"]])
    return rows


def _comparison_rows(report):
    return [[p["kind"], p["status"], *[p[side]["experiment_id"] if p[side] else None for side in ("left", "right")],
             *[p[k] for k in ("position_max_abs", "position_rms", "velocity_max_abs", "velocity_rms", "inverse_mass_equal")],
             p["tolerance"]["position_abs"], p["tolerance"]["velocity_abs"], p["last_batch_delta_ns"],
             p["last_batch_ratio"], *[p["metric_deltas"][k] if p["metric_deltas"] else None for k in METRICS], p["reason"]]
            for p in report["comparisons"]]


def _table(headers, rows):
    return '<div class="scroll"><table><thead><tr>' + ''.join(f'<th>{_escape(x)}</th>' for x in headers) + \
        '</tr></thead><tbody>' + ''.join('<tr>' + ''.join(f'<td>{x}</td>' for x in row) + '</tr>' for row in rows) + \
        '</tbody></table></div>'


def _chart(title, unit, points):
    available = [(i, value, label) for i, (value, label) in enumerate(points) if value is not None]
    if not available:
        return f'<section class="chart"><h3>{_escape(title)}</h3><p>No measurements available.</p></section>'
    low = min(0, min(value for _, value, _ in available))
    high = max(0, max(value for _, value, _ in available))
    extent = high - low or 1
    circles = []
    for i, value, label in available:
        x = 50 + 370 * i / max(1, len(points)-1)
        y = 160 - 125 * ((value-low) / extent)
        circles.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4"><title>{_escape(label)}: {_escape(value)} {_escape(unit)}</title></circle>')
    return f'<section class="chart"><h3>{_escape(title)}</h3><svg viewBox="0 0 470 200" role="img" aria-label="{_escape(title)}">' + \
        f'<text x="4" y="22">{_escape(high)} {_escape(unit)}</text><text x="4" y="168">{_escape(low)}</text>' + \
        '<path d="M45 30V165H445"/>' + ''.join(circles) + \
        f'<text x="50" y="190">1</text><text x="310" y="190">Row / pair {len(points)}</text></svg></section>'


def _html(report):
    current = report["current"]
    report_dir = Path(current["directory"]) / "reports" / report["report_id"]
    rows = []
    for i, row in enumerate(current["experiments"], 1):
        c, m, p = row["config"], row["metrics"], row["last_batch"]
        details = f'<details><summary>{row["experiment_id"][:12]}</summary><pre>{_escape(canonical(c).decode())}</pre>'
        details += f'<p>Run {_escape(row["run_id"])}<br>Attempt {_escape(row["attempt_id"])}</p>'
        details += f'<pre>Metrics: {_escape(canonical(m).decode())}</pre>'
        if row["evidence"]:
            for name, stamp in row["evidence"]["files"].items():
                relative = os.path.relpath(Path(current["directory"]) / stamp["path"], report_dir).replace("\\", "/")
                details += f'<a href="{quote(relative, safe="/")}">{_escape(name)}</a> '
        details += f'<p>{_escape(row["error"])}</p></details>'
        rows.append([str(i), details, _escape(row["status"]), _escape(c["solver"]), str(c["cloth"]["seed"]),
            str(c["steps"]), str(c["cloth"]["iterations"]), _escape(c["cloth"]["compliance"]),
            _escape(m["max_constraint_error"] if m else None),
            _escape(p["wall_time_ns"] / 1e6 if p else None), _escape(p["batch_steps"] if p else None)])
    comparison_rows = [[str(i), _escape(p["kind"]), _escape(p["status"]),
        *[_escape(p[side]["experiment_id"][:12] if p[side] else None) for side in ("left", "right")],
        _escape(p["position_max_abs"]), _escape(p["tolerance"]["position_abs"]),
        _escape(p["velocity_max_abs"]), _escape(p["tolerance"]["velocity_abs"]),
        _escape(p["last_batch_ratio"]), _escape(p["reason"])] for i, p in enumerate(report["comparisons"], 1)]
    count = sum(r["status"] == "succeeded" for r in current["experiments"])
    observation = report["execution_observation"]
    end_to_end = _text(observation["wall_time_ns"] / 1e9) + ' s' if observation else '— (not collected)'
    charts = _chart("Constraint error by experiment", "m", [
        (r["metrics"]["max_constraint_error"] if r["metrics"] else None, f'Row {i}: {r["experiment_id"][:12]}')
        for i, r in enumerate(current["experiments"], 1)])
    charts += _chart("Most recent batch wall time", "ms", [
        (r["last_batch"]["wall_time_ns"] / 1e6 if r["last_batch"] else None, f'Row {i}: {r["config"]["solver"]}')
        for i, r in enumerate(current["experiments"], 1)])
    charts += _chart("Paired particle position error", "m", [
        (p["position_max_abs"], f'Pair {i}: {p["kind"]} {p["status"]}') for i, p in enumerate(report["comparisons"], 1)])
    return ('<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">'
        '<title>DeckerEngine experiment report</title><style>'
        'body{margin:0;background:#f3f5f8;color:#172b43;font:15px/1.55 system-ui,sans-serif}'
        'main{max-width:1400px;margin:auto;padding:40px 28px}h1{font-size:32px;margin:8px 0}h2{margin-top:32px}'
        'a{color:#155ab5}small,.muted{color:#52657b}.cards{display:flex;flex-wrap:wrap;gap:16px;margin:24px 0}'
        '.card,.chart{background:white;border:1px solid #dce2ea;border-radius:10px;padding:18px}.card{flex:1;min-width:180px}'
        '.card strong{display:block;font-size:23px}.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px}'
        '.chart h3{font-size:16px;margin:0}svg{width:100%}svg text{font:14px system-ui;fill:#52657b}svg path{stroke:#aab7c7;fill:none}circle{fill:#1b7b9b}'
        '.scroll{overflow:auto;max-height:600px;background:white;border:1px solid #dce2ea}table{width:100%;border-collapse:collapse;white-space:nowrap}'
        'td,th{padding:10px 12px;border-bottom:1px solid #e4e9ef;text-align:left}th{position:sticky;top:0;background:#eaf0f7}'
        'pre{white-space:pre-wrap;word-break:break-all;max-width:680px;font-size:12px}details p{white-space:normal}summary{cursor:pointer}'
        '</style><main><small>DECKERENGINE · EXPERIMENTS</small><h1>Parameter scan &amp; comparison</h1>'
        f'<p class="muted">{_escape(report["created_at"])} · Report {_escape(report["report_id"])}</p>'
        '<p><a href="report.json">Full JSON evidence</a> · <a href="experiments.csv">Experiment CSV</a> · <a href="comparisons.csv">Comparison CSV</a></p>'
        f'<div class="cards"><div class="card">Execution<strong>{_escape(current["status"])}</strong>{count} / {len(current["experiments"])} succeeded</div>'
        f'<div class="card">Numerical comparison<strong>{_escape(report["comparison_status"])}</strong>{len(report["comparisons"])} pairs</div>'
        f'<div class="card">This CLI invocation<strong>{_escape(end_to_end)}</strong>GPU timestamp: — (not collected)</div></div>'
        '<p>Missing values are shown as —; missing points are omitted. Row order follows the plan. Hover points for source rows.</p>'
        f'<div class="charts">{charts}</div><h2>Experiments</h2>' +
        _table(["#", "Evidence / parameters", "Status", "Solver", "Seed", "Steps", "Iterations", "Compliance", "Constraint error (m)", "Last batch (ms)", "Batch steps"], rows) +
        '<h2>Numerical comparisons</h2><p>Left is CPU or baseline; right is GPU or current. Differences and timing ratios use right minus / divided by left.</p>' +
        _table(["#", "Kind", "Status", "Left", "Right", "Max position (m)", "Limit (m)", "Max velocity (m/s)", "Limit (m/s)", "Last batch ratio", "Reason"], comparison_rows) +
        '<h2>Measurement scope &amp; provenance</h2><ul>' + ''.join(f'<li>{_escape(note)}</li>' for note in report["measurement_notes"]) +
        '</ul><details><summary>Tolerances and source manifests</summary><pre>' +
        _escape(canonical({"tolerances": report["tolerances"], **{label: {k: v for k, v in report[label].items() if k != "experiments"}
            if report[label] else None for label in ("current", "baseline")}}).decode()) + '</pre></details></main></html>').encode("utf-8")


def render_files(report):
    return {"report.json": canonical(report) + b"\n", "report.html": _html(report),
        "experiments.csv": _csv(["source", "batch_id", "plan_id", "experiment_id", "attempt_id", "run_id", "status",
            "solver", "seed", "steps", "fixed_dt_ns", "cloth_json", *METRICS, "last_sample_steps", "last_batch_steps",
            "last_batch_wall_time_ns", "gpu_timestamp_ns", "error"], _experiment_rows(report)),
        "comparisons.csv": _csv(["kind", "status", "left_experiment_id", "right_experiment_id", "position_max_abs",
            "position_rms", "velocity_max_abs", "velocity_rms", "inverse_mass_equal", "position_tolerance", "velocity_tolerance",
            "last_batch_delta_ns", "last_batch_ratio", *[name + "_delta" for name in METRICS], "reason"], _comparison_rows(report))}
