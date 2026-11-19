"""DeckerEngine development CLI. Run python tools/dk.py --help."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from uuid import uuid4
import xml.etree.ElementTree as ET

from dk_index import ROOT, NAMES, index

DOCS = {"design": "spec/design", "development": "spec/development", "guide": "spec/guides",
        "benchmark": "spec/benchmarks", "command": "spec/commands", "research": "spec/research"}
SELECTION = ".agents/skills/decker-build-verify/references/test-selection.md"
PROFILE_MODES = ("cpu", "memory", "memory-disabled", "arena", "arena-disabled", "pool",
                 "pool-disabled", "context", "context-disabled", "gpu", "cloud")
SCRIPTS = {
    "index-tests": [sys.executable, "tools/test_dk_index.py"],
    "spec": ["pwsh", "-NoProfile", "-File", "scripts/check-spec.ps1"],
    "spec-tests": ["pwsh", "-NoProfile", "-File", "scripts/test-check-spec.ps1"],
    "pathtracing-replay": [sys.executable, "scripts/verify-pathtracing-evidence.py",
                           "spec/benchmarks/2026-10-15-pathtracing"],
    "restir-quality": [sys.executable, "scripts/benchmark-restir-sponza.py", "--directory",
                       "spec/benchmarks/2026-10-19-restir-sponza-5090", "--verify-quality"],
}


def find_docs(query, kind, limit):
    files = list((ROOT / DOCS[kind]).rglob("*.md")) if kind else [
        ROOT / "WORK.md", ROOT / "README.md", ROOT / "AGENTS.md", *(ROOT / "spec").rglob("*.md"),
        *(ROOT / ".agents/skills").rglob("*.md")]
    needle = query.casefold()
    results = []
    for path in files:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
        title = next((s[2:] for s in lines if s.startswith("# ")), path.stem)
        best = None
        for number, line in enumerate(lines, 1):
            if needle not in line.casefold():
                continue
            rank = 0 if line.startswith("# ") else 1 if line.startswith("#") else 2
            if best is None or rank < best[0]:
                pos = line.casefold().find(needle)
                best = (rank, number, line[max(0, pos - 60):max(0, pos - 60) + 180].strip())
        if best is None and needle in path.stem.casefold():
            best = (1, 1, title)
        if best:
            rank, number, excerpt = best
            relative = path.relative_to(ROOT).as_posix()
            results.append((rank, relative, dict(title=title, path=relative, line=number, excerpt=excerpt)))
    return [item for _, _, item in sorted(results)[:limit]]


def run_script(command, payload=None):
    result = subprocess.run(command, cwd=ROOT, input=payload, capture_output=True, encoding="utf-8",
                            errors="replace", env={**os.environ, "PYTHONIOENCODING": "utf-8"})
    output = (result.stdout + "\n" + result.stderr).strip()
    data = {"exit_code": result.returncode, "output": output[-2600:]}
    match = re.search(r"^Summary: (.+)$", result.stdout, re.MULTILINE)
    if match:
        path = Path(match[1].strip())
        summary = json.loads(path.read_text(encoding="utf-8-sig"))
        data.update({key: summary[key] for key in ("status", "passed", "failed", "skipped", "error") if key in summary})
        data["summary"] = str(path)
        if not result.returncode and "selected_tests" in summary:
            data["output"] = (f"{summary['status']}: passed={summary['passed']}, failed={summary['failed']}, "
                              f"skipped={summary['skipped']}\nSummary: {path}")
        if summary.get("failed") and path.with_name("results.xml").is_file():
            cases = ET.parse(path.with_name("results.xml")).iter("testcase")
            data["failed_tests"] = [case.get("name") for case in cases if case.find("failure") is not None
                                    or case.find("error") is not None or case.get("status") == "notrun"]
    return data


def powershell(script, options):
    # Typed array values reach the native PowerShell script through splatting.
    # Neither regexes nor paths enter command source.
    bridge = ("$ErrorActionPreference='Stop'; [Console]::InputEncoding=[Text.Encoding]::UTF8; "
              "$job=[Console]::In.ReadToEnd()|ConvertFrom-Json -AsHashtable; "
              "$params=$job.params; & $job.script @params; if (!$?) {exit 1}; exit $LASTEXITCODE")
    return run_script(["pwsh", "-NoProfile", "-Command", bridge],
                      json.dumps({"script": "scripts/" + script, "params": options}))


def profile(args):
    if args.action == "list":
        return list(PROFILE_MODES)
    if args.mode == "cloud":
        output = args.output or f"out/profiling/cloud-{datetime.now():%Y%m%d-%H%M%S}-{uuid4().hex[:8]}"
        directory = (ROOT / output).resolve()
        if directory.exists():
            raise ValueError(f"Cloud capture requires a new output directory: {directory}")
        command = [sys.executable, "scripts/capture-cloud-profiling.py", "--output", output]
        if args.configuration and args.configuration != "RelWithDebInfo":
            raise ValueError("Cloud capture uses RelWithDebInfo")
        for name in ("build_dir", "tools_dir", "port"):
            if getattr(args, name) is not None:
                command.extend(["--" + name.replace("_", "-"), str(getattr(args, name))])
        data = run_script(command)
        data["directory"] = str(directory)
        summary_file = directory / "summary.json"
        if summary_file.is_file():
            report = json.loads(summary_file.read_text(encoding="utf-8"))
            data.update(status=report["status"], summary=str(summary_file))
            if not data["exit_code"]:
                data["output"] = f"Cloud capture {report['status']}. Summary: {summary_file}"
        return data
    if args.output:
        raise ValueError("--output applies to cloud capture. CPU and GPU captures create run directories.")
    options = {key: value for key, value in (("BuildDir", args.build_dir), ("ToolsDir", args.tools_dir),
               ("Configuration", args.configuration), ("Port", args.port)) if value is not None}
    if args.mode == "gpu":
        return powershell("capture-gpu-profiling.ps1", options)
    return powershell("capture-profiling.ps1", dict(options, Mode=args.mode))


def tests(args):
    if args.action == "list":
        if args.limit < 1:
            raise ValueError("Select a positive result limit")
        lines = (ROOT / SELECTION).read_text(encoding="utf-8").splitlines()
        query = args.query.casefold()
        entries = [{"script": name} for name in SCRIPTS if query in name]
        for line in lines:
            if line.startswith("|") and "dk_" in line and query in line.casefold():
                cells = [s.strip().replace(r"\|", "|") for s in re.split(r"(?<!\\)\|", line.strip("|"))]
                behavior, targets, selection = cells
                entries.append(dict(behavior=behavior, targets=targets, selection=selection))
        return entries[:args.limit]
    if args.script:
        if args.target or args.regex or args.build_dir or args.configuration:
            raise ValueError("Named scripts use fixed inputs. Use --target and --regex for CTest.")
        return run_script(SCRIPTS[args.script])
    if not args.target or not args.regex:
        raise ValueError("Use --script NAME or supply both --target and --regex")
    return powershell("verify.ps1", dict(Target=args.target, TestRegex=args.regex,
                      BuildDir=args.build_dir or "out/build/windows-dev", Configuration=args.configuration or "Debug"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("work", help="Read the current handoff")
    docs = sub.add_parser("docs", help="Find documentation by literal keyword or filename")
    docs.add_argument("query")
    docs.add_argument("--kind", choices=DOCS)
    docs.add_argument("--limit", type=int, default=8)
    p = sub.add_parser("profile", help="List or run a profiling workload").add_subparsers(dest="action", required=True)
    p.add_parser("list")
    run = p.add_parser("run")
    run.add_argument("mode", choices=PROFILE_MODES)
    for flag in ("build-dir", "tools-dir", "configuration", "output"):
        run.add_argument("--" + flag)
    run.add_argument("--port", type=int)
    t = sub.add_parser("test", help="Find or run a focused check").add_subparsers(dest="action", required=True)
    listing = t.add_parser("list")
    listing.add_argument("query", nargs="?", default="")
    listing.add_argument("--limit", type=int, default=12, help="Maximum matching entries (default: 12)")
    run = t.add_parser("run")
    run.add_argument("--target", nargs="+")
    run.add_argument("--regex")
    run.add_argument("--build-dir")
    run.add_argument("--configuration")
    run.add_argument("--script", choices=SCRIPTS)
    idx = sub.add_parser("index", help="Inspect or update local code indexes")
    idx.add_argument("action", choices=("status", "update"))
    idx.add_argument("tool", nargs="?", choices=(*NAMES, "all"), default="all")
    parser.add_argument("--json", action="store_true", help="Machine-readable response (also accepted after subcommand)")
    argv = list(sys.argv[1:] if argv is None else argv)
    as_json = "--json" in argv
    args = parser.parse_args([arg for arg in argv if arg != "--json"])
    try:
        if args.command == "docs":
            if not args.query.strip() or args.limit < 1:
                raise ValueError("Provide a nonempty keyword and positive limit")
            data = find_docs(args.query, args.kind, args.limit)
        elif args.command == "work":
            data = (ROOT / "WORK.md").read_text(encoding="utf-8")
        elif args.command == "test":
            data = tests(args)
        elif args.command == "profile":
            data = profile(args)
        else:
            data = index(args.action, args.tool)
        if as_json:
            print(json.dumps(data, ensure_ascii=False))
        elif isinstance(data, str):
            print(data.rstrip())
        elif args.command == "docs":
            for row in data:
                print(f"{row['path']}:{row['line']}  {row['title']}\n  {row['excerpt']}")
        elif isinstance(data, dict):
            print(data["output"])
            for name in data.get("failed_tests", []):
                print(f"FAIL {name}")
            if "directory" in data:
                print(data["directory"])
        else:
            for row in data:
                if isinstance(row, dict) and "selection" in row:
                    print(f"{row['behavior']}  {row['targets']}  {row['selection']}")
                elif isinstance(row, dict) and "script" in row:
                    print(f"--script {row['script']}")
                else:
                    print(json.dumps(row, ensure_ascii=False) if isinstance(row, dict) else row)
        if isinstance(data, dict):
            return data.get("exit_code", 0)
        return int(isinstance(data, list) and any(isinstance(row, dict) and "error" in row for row in data))
    except (OSError, ValueError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False) if as_json else str(error),
              file=sys.stdout if as_json else sys.stderr)
        return 1


if __name__ == "__main__":
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
