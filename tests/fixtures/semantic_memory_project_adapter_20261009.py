"""Catalog-backed MCP identity adaptation; no catalog/database writes."""
import argparse
import json
import ntpath
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import threading

VERSION = "2026-10-09.1"
GUIDANCE = (
    "Project identity: first call memory_resolve_project(workspace=<actual absolute cwd>) "
    "or memory_resolve_project(task_id=<active task id>), then use its project_uuid. "
    "scope='global' still requires this registered audit anchor. Do not invent a project "
    "name or use a remote host label. list_projects lists code indexes, not memory workspaces. "
)
RESOLVER = {
    "name": "memory_resolve_project",
    "description": "Read-only resolution of a registered Semantic Memory workspace or task to its project UUID. "
                   "Does not index code, create a workspace, or write memory. "
                   "Supply exactly one of workspace (actual absolute cwd), task_id (active task), or project (known UUID/legacy path name).",
    "inputSchema": {
        "type": "object", "properties": {
            "workspace": {"type": "string", "description": "Actual absolute workspace directory; never guess a host name."},
            "task_id": {"type": "string", "description": "Existing task id provided by the active lifecycle hook."},
            "project": {"type": "string", "description": "Existing UUID or registered path-derived legacy project name."},
        }, "additionalProperties": False,
    },
}

def redact(value):
    return re.sub(r"(?i)(?:mmc-key-|sk-proj-|sk-)[A-Za-z0-9_-]+", "[REDACTED]", value)

class Catalog:
    def __init__(self, root):
        self.db = Path(root) / "__global__-memory.db"

    def connect(self):
        return sqlite3.connect(self.db.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)

    @staticmethod
    def normalized_path(value):
        # Match only absolute, non-traversing paths. Never accept relative paths.
        if not isinstance(value, str) or not ntpath.isabs(value):
            return None
        if any(part in (".", "..") for part in re.split(r"[\\/]", value)):
            return None
        return ntpath.normpath(value).replace("/", "\\").casefold()

    @staticmethod
    def legacy_name(path):
        return re.sub(r"[:\\/]+", "-", path).strip("-").casefold()

    def resolve(self, *, project=None, workspace=None, task_id=None):
        with self.connect() as c:
            rows = c.execute("SELECT project_uuid,canonical_path,display_name,index_state FROM global_project_catalog").fetchall()
            if task_id is not None:
                binding = c.execute("SELECT project_uuid FROM global_task_workspace WHERE task_id=?", (task_id,)).fetchone()
                matches = [r for r in rows if binding and r[0] == binding[0]]
            elif workspace is not None:
                path = self.normalized_path(workspace)
                matches = [r for r in rows if path and self.normalized_path(r[1]) == path]
            else:
                if not isinstance(project, str) or not project:
                    return None
                path = self.normalized_path(project)
                matches = [r for r in rows if r[0] == project or (path and self.normalized_path(r[1]) == path)]
                if not matches and not any(x in project for x in ("/", "\\", ":", "..")):
                    aliases = c.execute("SELECT project_uuid FROM global_legacy_alias WHERE legacy_kind='project' AND legacy_id=?", (project,)).fetchall()
                    ids = {r[0] for r in aliases}
                    matches = [r for r in rows if r[0] in ids or self.legacy_name(r[1]) == project.casefold()]
            unique = {r[0]: r for r in matches}
            if len(unique) != 1:
                return None
            r = next(iter(unique.values()))
            return {"project_uuid": r[0], "canonical_path": redact(r[1]),
                    "display_name": redact(r[2]), "index_state": r[3]}

    def count(self):
        with self.connect() as c:
            return c.execute("SELECT count(*) FROM global_project_catalog").fetchone()[0]

def tool_result(body, error=False):
    return {"content": [{"type": "text", "text": json.dumps(body, ensure_ascii=False)}],
            "structuredContent": body, "isError": error}

def upgrade_descriptor(tool):
    if tool.get("name") == "memory_resolve_project":
        return tool
    props = tool.get("inputSchema", {}).get("properties", {})
    if "project" in props:
        props["project"]["description"] = GUIDANCE + "Use the returned project_uuid string."
        old = tool.get("description", "")
        # Remove contradictory legacy guidance while retaining all other usage rules.
        old = re.sub(r"(?i)path-derived project name WITH drive/path prefix[^.]*\.", "", old)
        tool["description"] = GUIDANCE + old
    if tool.get("name") == "list_projects":
        tool["description"] = "List indexed code projects. This is not the registered memory workspace catalog. " + GUIDANCE
    return tool

def adapt_request(request, catalog):
    if request.get("method") != "tools/call":
        return request
    params = request.get("params", {})
    name = params.get("name", "")
    args = params.get("arguments", {})
    # Preserve lifecycle project provenance and all unknown names. Only deterministic,
    # unique, registered identities are translated; the core still applies every guard.
    if name in ("memory_task_begin", "memory_task_status", "memory_task_complete"):
        return request
    if name == "events" or name == "memories_retrieve" or name.startswith(("memory_", "neuroplastic_")):
        if isinstance(args, dict) and "project" in args:
            found = catalog.resolve(project=args["project"])
            if found:
                args["project"] = found["project_uuid"]
    return request

def run(core, root):
    sys.stdin.reconfigure(encoding="utf-8", errors="strict")
    sys.stdout.reconfigure(encoding="utf-8", errors="strict", line_buffering=True)
    catalog = Catalog(root)
    child = subprocess.Popen([str(core)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=None, text=True, encoding="utf-8", bufsize=1)
    lock = threading.Lock()
    pending = {}
    stopping = threading.Event()

    def emit(obj):
        with lock:
            print(json.dumps(obj, ensure_ascii=False, separators=(",", ":")), flush=True)

    def receive():
        for line in child.stdout:
            try:
                response = json.loads(line)
                meta = pending.pop(str(response.get("id")), {})
                if meta.get("method") == "tools/list" and "result" in response:
                    ts = response["result"].get("tools", [])
                    response["result"]["tools"] = [upgrade_descriptor(t) for t in ts] + [RESOLVER]
                elif meta.get("name") == "describe_tool" and "result" in response:
                    result = response["result"]
                    if isinstance(result.get("structuredContent"), dict) and "inputSchema" in result["structuredContent"]:
                        upgrade_descriptor(result["structuredContent"])
                    for content in result.get("content", []):
                        if content.get("type") == "text":
                            try:
                                obj = json.loads(content["text"])
                                if "inputSchema" in obj:
                                    content["text"] = json.dumps(upgrade_descriptor(obj), ensure_ascii=False)
                            except ValueError:
                                pass
                elif meta.get("name") == "list_projects" and "result" in response:
                    result = response["result"]
                    obj = result.get("structuredContent")
                    if obj is None:
                        try:
                            obj = json.loads(result["content"][0]["text"])
                        except (KeyError, ValueError, IndexError):
                            obj = None
                    if isinstance(obj, dict):
                        obj["memory_workspace_count"] = catalog.count()
                        obj["memory_project_resolver"] = "memory_resolve_project"
                        obj["hint"] = "Code index inventory only. For memory writes resolve the actual workspace/task UUID with memory_resolve_project; indexing is not required."
                        result["structuredContent"] = obj
                        result["content"] = [{"type": "text", "text": json.dumps(obj, ensure_ascii=False)}]
                emit(response)
            except Exception:
                # Avoid leaking tool content through exceptions. Fail closed.
                sys.stderr.write("Semantic Memory adapter: invalid core response.\n")
                child.terminate()
                break
        if not stopping.is_set():
            os._exit(child.wait())

    threading.Thread(target=receive, daemon=True).start()
    try:
        for line in sys.stdin:
            request = {}
            try:
                request = json.loads(line)
                if request.get("method") == "tools/call":
                    p = request.get("params", {})
                    if p.get("name") == "memory_resolve_project":
                        args = p.get("arguments", {})
                        keys = ("workspace", "task_id", "project")
                        if not isinstance(args, dict) or set(args) - set(keys) or sum(k in args for k in keys) != 1:
                            found = None
                            code = "INVALID_RESOLVER_ARGUMENTS"
                        else:
                            found = catalog.resolve(**args)
                            code = "PROJECT_NOT_FOUND_OR_AMBIGUOUS"
                        result = tool_result({"status": "resolved", "adapter_version": VERSION, **found}) if found else tool_result({"status": "rejected", "code": code}, True)
                        emit({"jsonrpc": "2.0", "id": request.get("id"), "result": result})
                        continue
                    if p.get("name") == "describe_tool" and p.get("arguments", {}).get("name") == "memory_resolve_project":
                        emit({"jsonrpc": "2.0", "id": request.get("id"), "result": tool_result(RESOLVER)})
                        continue
                request = adapt_request(request, catalog)
                params = request.get("params", {})
                if "id" in request:
                    pending[str(request["id"])] = {"method": request.get("method"), "name": params.get("name")}
                child.stdin.write(json.dumps(request, ensure_ascii=False, separators=(",", ":")) + "\n")
                child.stdin.flush()
            except Exception:
                if "id" in request:
                    emit({"jsonrpc": "2.0", "id": request["id"], "error": {"code": -32603, "message": "Project adapter failed closed; no request forwarded."}})
    finally:
        stopping.set()
        child.stdin.close()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.terminate()
            child.wait(timeout=5)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--data-root", type=Path, required=True)
    args = parser.parse_args()
    # Fixture overrides follow the existing core cache precedence.
    root = os.environ.get("CBM_CACHE_DIR") or os.environ.get("CBM_DATA_ROOT") or str(args.data_root)
    run(args.core, root)
