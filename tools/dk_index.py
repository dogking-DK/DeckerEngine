"""Project-local code index commands and verified update history (Python 3.11+)."""
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import tomllib

ROOT = Path(__file__).resolve().parents[1]
HISTORY = ROOT / "spec/guides/code-index-history.md"
NAMES = ("codegraph", "codebase-memory")
PROBE = "parse_id_bytes"
SOURCE = "engine/foundation/core/src/StableId.cpp"


def command(tool):
    binary = "codebase-memory-mcp" if tool == "codebase-memory" else tool
    # Prefer the same installation and environment as the project's MCP session.
    config = Path(os.environ.get("CODEX_HOME", Path.home() / ".codex")) / "config.toml"
    settings = {}
    if config.is_file():
        with config.open("rb") as stream:
            settings = tomllib.load(stream).get("mcp_servers", {}).get(tool, {})
    env = {**os.environ, **settings.get("env", {})}
    if settings.get("command"):
        args = settings.get("args", [])
        if tool == "codegraph" and "serve" in args:
            return [settings["command"], *args[:args.index("serve")]], env
        if tool == "codebase-memory":
            return [settings["command"]], env
    exe = shutil.which(binary)
    if exe:
        return [exe], env
    raise ValueError(f"{binary} is unavailable. Configure the local MCP server or add its CLI to PATH.")


def run(argv, env=None):
    result = subprocess.run(argv, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, encoding="utf-8", errors="replace")
    if result.returncode:
        raise ValueError((result.stderr or result.stdout or str(argv[0]))[-2500:].strip())
    return result.stdout.strip()


def cbm(cmd, env, action, **args):
    output = run([*cmd, "cli", "--quiet", "--json", action, json.dumps(args)], env)
    reply = json.loads(output)
    if reply.get("isError"):
        raise ValueError(str(reply.get("content"))[:1200])
    if "structuredContent" in reply:
        return reply["structuredContent"]
    return json.loads(reply["content"][0]["text"])


def cbm_project(cmd, env):
    offset = 0
    while True:
        page = cbm(cmd, env, "list_projects", format="json", offset=offset, limit=100)
        for item in page["projects"]:
            if Path(item["root_path"]).resolve() == ROOT:
                return item["name"]
        if not page["has_more"]:
            return None
        offset += len(page["projects"])


def update_cbm(cmd, env, args):
    # The MCP watcher and this CLI share the upstream project's writer lock.
    deadline = time.monotonic() + 60
    while True:
        try:
            cbm(cmd, env, "index_repository", **args)
            break
        except ValueError as error:
            if "another index operation for this project is active" not in str(error) or time.monotonic() >= deadline:
                raise
            time.sleep(2)
    identity = {key: args[key] for key in ("repo_path", "name") if key in args}
    job = cbm(cmd, env, "index_repository", **identity, status=True)
    if job.get("state") != "succeeded":
        raise ValueError(f"Index job has not succeeded: {job.get('state', 'unknown')}")


def operate(tool, update):
    cmd, env = command(tool)
    version = run([*cmd, "--version"], env).splitlines()[-1]
    state = {"tool": tool, "version": version}
    if tool == "codegraph":
        exists = (ROOT / ".codegraph/codegraph.db").is_file()
        if update:
            args = ["sync", str(ROOT)] if exists else ["init", "--yes", str(ROOT)]
            output = run([*cmd, *args], env)
        if not update and not exists:
            return dict(state, status="未建库")
        info = json.loads(run([*cmd, "status", "--json", str(ROOT)], env))
        if Path(info["projectPath"]).resolve() != ROOT or not info["initialized"]:
            raise ValueError("CodeGraph returned a different project")
        state.update(indexed_at=info.get("lastIndexed"), pending=info.get("pendingChanges"),
                     nodes=info["nodeCount"], edges=info["edgeCount"])
        matches = json.loads(run([*cmd, "query", PROBE, "-p", str(ROOT), "--json"], env))
        found = any(row["node"]["name"] == PROBE and row["node"]["filePath"].replace("\\", "/") == SOURCE
                    for row in matches)
        operation = "增量同步" if exists else "首次建库"
        if update and exists and "Already up to date" in output:
            operation = "已是最新"
    else:
        project = cbm_project(cmd, env)
        if update:
            args = {"repo_path": str(ROOT), "mode": "moderate"}
            if project:
                args["name"] = project
            update_cbm(cmd, env, args)
        if not project and not update:
            return dict(state, status="未建库")
        operation = "重新索引" if project else "首次建库"
        project = project or cbm_project(cmd, env)
        if not project:
            raise ValueError("DeckerEngine index did not register its repository root")
        info = cbm(cmd, env, "index_status", project=project, format="json")
        if info["status"] != "ready" or Path(info["root_path"]).resolve() != ROOT:
            raise ValueError("codebase-memory index is not ready for this repository")
        state.update(indexed_at=info.get("indexed_at"), nodes=info["nodes"], edges=info["edges"],
                     partial_files=info.get("parse_partial", {}).get("count", 0))
        matches = cbm(cmd, env, "search_graph", project=project, name_pattern="^" + PROBE + "$",
                      file_pattern="StableId.cpp", format="json", limit=5)
        name_column = matches["cols"].index("name")
        found = any(group["file"].replace("\\", "/") == SOURCE and any(row[name_column] == PROBE for row in group["rows"])
                    for group in matches["groups"])
    if not found:
        raise ValueError(f"{tool}: could not verify {SOURCE} / {PROBE}. Result: {str(matches)[:500]}")
    if update:
        state["operation"] = operation
    return dict(state, status="源码查询通过")


@contextmanager
def update_lock():
    lock = ROOT / "out/dk-index.lock"
    lock.parent.mkdir(exist_ok=True)
    with lock.open("a+b") as stream:
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError("Another index update is running") from error
        yield


def history_rows(body):
    return [line for line in body.splitlines() if any(line.startswith(f"| {name} |") for name in NAMES)]


def record(result, commit, state):
    body = HISTORY.read_text(encoding="utf-8")
    rows = history_rows(body)
    stamp = datetime.now(timezone(timedelta(hours=8))).isoformat(timespec="seconds")
    tool = result["tool"]
    row = f"| {tool} | {stamp} | {result['operation']} | {result['version']} | {commit} | {state} |"
    rows = [row, *rows]
    prefix = [line for line in body.splitlines() if line not in rows]
    if not prefix or "| --- |" not in prefix[-1]:
        raise ValueError("Unexpected index history table layout")
    for name in NAMES:
        prefix.extend([line for line in rows if line.startswith(f"| {name} |")][:3])
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                     dir=HISTORY.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write("\n".join(prefix) + "\n")
    try:
        temporary.replace(HISTORY)
    finally:
        temporary.unlink(missing_ok=True)


def index(action, selected):
    tools = NAMES if selected == "all" else [selected]
    data = []
    if action == "status":
        rows = history_rows(HISTORY.read_text(encoding="utf-8"))
        for tool in tools:
            try:
                item = operate(tool, False)
            except (OSError, ValueError) as error:
                item = {"tool": tool, "error": str(error)}
            item["history"] = [r for r in rows if r.startswith(f"| {tool} |")]
            data.append(item)
        return data
    with update_lock():
        for tool in tools:
            try:
                commit = run(["git", "rev-parse", "--short", "HEAD"])
                state = "有修改" if run(["git", "status", "--porcelain"]) else "干净"
                item = operate(tool, True)
                if item["operation"] != "已是最新":
                    record(item, commit, state)
                data.append(item)
            except (OSError, ValueError) as error:
                data.append({"tool": tool, "error": str(error)})
    return data
