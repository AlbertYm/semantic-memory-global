"""Portable, private, CAS-guarded Codex/MMCAPI registration repair. Python 3.11+."""
import argparse
import copy
from contextlib import closing
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tomllib
import uuid

MCP_NAMES = {"codebase_memory", "codegraph", "filesystem", "memory", "node_repl"}
LEGACY_ADAPTER_SHA256 = "f05d41ffebe7f31dc336d3e9defc2199a5d613e51bfa16ea838537d0d3a57dfe"

def sha(data): return hashlib.sha256(data).hexdigest()
def dumps(obj): return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
def parse(raw): return tomllib.loads(raw.decode("utf-8-sig") if isinstance(raw, bytes) else raw)
def atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name+".memory-repair-"+uuid.uuid4().hex+".tmp")
    with temp.open("xb") as f: f.write(data); f.flush(); os.fsync(f.fileno())
    os.replace(temp, path)

def private_directory(path):
    path.mkdir(parents=True, exist_ok=False)
    if os.name == "nt":
        sid = next(csv.reader(io.StringIO(subprocess.check_output(["whoami", "/user", "/fo", "csv", "/nh"], text=True))))[1]
        subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r", "*"+sid+":(OI)(CI)F", "*S-1-5-18:(OI)(CI)F"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        path.chmod(0o700)

def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))

def attested_legacy_registration(existing, desired, core):
    """Recognize only the pinned earlier adapter and its matching local transaction."""
    root = Path(core).parent.parent
    legacy = root/"tools/semantic_memory_project_adapter_20261009.py"
    args = existing.get("args", [])
    if set(existing)-{"command", "args", "enabled", "env", "type"}: return False
    if len(args) != 7 or args[:2] != ["-I", "-u"] or args[3] != "--core" or args[5] != "--data-root": return False
    if not all(same_path(a,b) for a,b in ((args[2],legacy),(args[4],core),(args[6],root/"data"))): return False
    if any(k not in desired["env"] or v != desired["env"][k] for k,v in existing.get("env",{}).items()): return False
    if not legacy.is_file() or legacy.is_symlink() or sha(legacy.read_bytes()) != LEGACY_ADAPTER_SHA256: return False
    # Do not infer ownership from the filename or an arbitrary Python executable.
    # The exact registration (including interpreter) must match the old private receipt.
    for index, path in enumerate((root/"backups").glob("project-adapter_*/transaction.json")):
        if index >= 100: break
        try:
            if path.stat().st_size > 65536: continue
            tx = json.loads(path.read_text(encoding="utf-8"))
            if (tx.get("schema") == "semantic-memory-project-adapter-transaction/v1"
                and tx.get("phase") == "APPLIED_RUNTIME_VERIFIED"
                and tx.get("adapter_sha256") == LEGACY_ADAPTER_SHA256
                and tx.get("new_registration") == existing): return True
        except (OSError, ValueError): continue
    return False

def compatible(existing, desired, core):
    if existing is None or existing == desired: return True
    if same_path(existing.get("command", ""), core) and existing.get("args", []) == []:
        # A disabled native entry is repairable; extra options/unknown env require review.
        allowed = {"command", "args", "enabled", "env", "type"}
        return not (set(existing)-allowed) and set(existing.get("env", {})) <= set(desired["env"])
    return attested_legacy_registration(existing, desired, core)

def patch_toml(raw, desired, core):
    before = parse(raw)
    old = before.get("mcp_servers", {}).get("semantic_memory")
    if not compatible(old, desired, core): raise RuntimeError("UNKNOWN_SEMANTIC_REGISTRATION")
    text = raw.decode("utf-8-sig") if isinstance(raw, bytes) else raw
    eol = "\r\n" if "\r\n" in text else "\n"
    section = ""; lines = []
    for line in text.splitlines(keepends=True):
        header = re.match(r"^\s*\[([^\]]+)\]\s*(?:#.*)?$", line.rstrip("\r\n"))
        if header: section = header[1]
        if section == "mcp_servers.semantic_memory" or section.startswith("mcp_servers.semantic_memory."):
            continue
        if section in {"mcp_servers."+n for n in MCP_NAMES} and re.match(r"^\s*type\s*=", line):
            if before["mcp_servers"][section.split(".", 1)[1]]["type"] != "stdio":
                raise RuntimeError("NON_STDIO_TRANSPORT_REQUIRES_REVIEW")
            continue
        lines.append(line)
    block = ["[mcp_servers.semantic_memory]", "command = "+dumps(desired["command"]), "args = "+dumps(desired["args"]), "enabled = true", "", "[mcp_servers.semantic_memory.env]"]
    block += [k+" = "+dumps(v) for k,v in desired["env"].items()]
    result = "".join(lines).rstrip("\r\n")+eol+eol+eol.join(block)+eol
    def unmanaged(obj):
        obj = copy.deepcopy(obj); servers = obj.get("mcp_servers", {}); servers.pop("semantic_memory", None)
        for name in MCP_NAMES:
            if name in servers and servers[name].get("type") == "stdio": servers[name].pop("type")
        if not servers: obj.pop("mcp_servers", None)
        return obj
    if unmanaged(before) != unmanaged(parse(result)): raise RuntimeError("UNRELATED_TOML_CHANGED")
    if parse(result)["mcp_servers"]["semantic_memory"] != desired: raise RuntimeError("REGISTRATION_PARSE_FAILED")
    if isinstance(raw, bytes): return (b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b"")+result.encode("utf-8")
    return result

def read_row(c, item):
    clause = " and ".join('"'+k+'"=?' for k in item["pk"])
    row = c.execute('select * from "'+item["table"]+'" where '+clause, tuple(item["pk"].values())).fetchone()
    return dict(row) if row else None

def write_row(c, item, value):
    clause = " and ".join('"'+k+'"=?' for k in item["pk"]); pk = tuple(item["pk"].values())
    if value is None:
        c.execute('delete from "'+item["table"]+'" where '+clause, pk)
    elif read_row(c, item) is None:
        cols = list(value)
        c.execute('insert into "'+item["table"]+'" ('+",".join('"'+k+'"' for k in cols)+') values ('+",".join("?" for _ in cols)+")", tuple(value.values()))
    else:
        cols = [k for k in value if k not in item["pk"]]
        c.execute('update "'+item["table"]+'" set '+",".join('"'+k+'"=?' for k in cols)+" where "+clause, tuple(value[k] for k in cols)+pk)

class Repair:
    def __init__(self, user_home, codex_home, install_root, mmcapi_db=None, python_exe=None):
        self.user_home = Path(user_home).resolve(); self.codex_home = Path(codex_home).resolve()
        self.root = Path(install_root).resolve(); self.config = self.codex_home/"config.toml"
        self.core = self.root/"bin/semantic-memory-mcp.exe"
        self.adapter = self.root/"tools/semantic_memory_project_adapter.py"
        self.source = Path(__file__).with_name("semantic_memory_project_adapter.py")
        self.state = self.root/"codex-memory-repair-state.json"
        self.db = Path(mmcapi_db).resolve() if mmcapi_db else None
        self.desired = {"command": str(Path(python_exe or sys.executable).resolve()),
                        "args": ["-I", "-u", str(self.adapter), "--core", str(self.core), "--data-root", str(self.root/"data")],
                        "enabled": True, "env": {"CBM_DATA_ROOT": str(self.root/"data"), "CBM_MEMORY_EMBED_BACKEND": "static", "CBM_MEMORY_AUTO_MAINTAIN": "0", "CBM_VERIFIED_LEARNING": "1"}}

    def protected(self):
        paths = [self.codex_home/"auth.json", self.user_home/".mmcapi/settings.json", self.user_home/".mmcapi/codex_oauth_auth.json",
                 self.root/"tools/semantic_memory_project_adapter_20261009.py"]
        return {str(p): sha(p.read_bytes()) if p.exists() else None for p in paths}

    def preflight(self):
        if not self.core.is_file(): raise RuntimeError("MANAGED_RUNTIME_MISSING")
        marker = json.loads((self.root/".semantic-memory-managed.json").read_text(encoding="utf-8"))
        if marker.get("product") != "semantic-memory" or marker.get("schema") != "stage14-install-marker/v2": raise RuntimeError("UNKNOWN_RUNTIME_MARKER")
        expected = marker.get("stable_entrypoint_sha256", {}).get(self.core.name)
        if not expected or sha(self.core.read_bytes()) != expected: raise RuntimeError("RUNTIME_INTEGRITY_MISMATCH")
        if self.db and not self.db.is_file(): raise RuntimeError("REQUESTED_MMCAPI_DATABASE_MISSING")

    def plan_rows(self, c):
        entries = []
        def add(table, pk, old, new):
            if old != new: entries.append({"table": table, "pk": pk, "before": old, "after": new})
        c.row_factory = sqlite3.Row
        for row in c.execute("select * from mcp_servers where enabled_codex=1").fetchall():
            old = dict(row); cfg = json.loads(old["server_config"])
            if old["id"] != "semantic_memory" and same_path(cfg.get("command", ""), self.core): raise RuntimeError("DUPLICATE_SEMANTIC_ALIAS")
            if old["id"] in MCP_NAMES and cfg.get("type") == "stdio":
                if any(v for k,v in old.items() if k.startswith("enabled_") and k != "enabled_codex"):
                    raise RuntimeError("SHARED_APPLICATION_TRANSPORT_REQUIRES_REVIEW")
                cfg.pop("type"); new = copy.deepcopy(old); new["server_config"] = dumps(cfg)
                add("mcp_servers", {"id": old["id"]}, old, new)
        row = c.execute("select * from mcp_servers where id='semantic_memory'").fetchone()
        if row:
            old = dict(row)
            if not compatible(json.loads(old["server_config"]), self.desired, self.core): raise RuntimeError("UNKNOWN_PERSISTENT_REGISTRATION")
            if any(v for k,v in old.items() if k.startswith("enabled_") and k != "enabled_codex"):
                raise RuntimeError("SHARED_SEMANTIC_REGISTRATION_REQUIRES_REVIEW")
            new = copy.deepcopy(old)
            new.update(server_config=old["server_config"] if json.loads(old["server_config"]) == self.desired else dumps(self.desired), enabled_codex=1)
        else:
            old = None
            new = {"id": "semantic_memory", "name": "Semantic Memory", "server_config": dumps(self.desired), "description": "Catalog-backed local memory for Codex", "homepage": "https://github.com/AlbertYm/semantic-memory-global", "docs": "https://github.com/AlbertYm/semantic-memory-global/blob/main/docs/INSTALL.zh-CN.md", "tags": dumps(["memory", "local", "codex"])}
            cols = [r[1] for r in c.execute("pragma table_info(mcp_servers)")]
            if not set(new) <= set(cols): raise RuntimeError("UNSUPPORTED_MMCAPI_SCHEMA")
            new.update({k:int(k == "enabled_codex") for k in cols if k.startswith("enabled_")})
        add("mcp_servers", {"id": "semantic_memory"}, old, new)
        row = c.execute("select * from settings where key='common_config_codex'").fetchone()
        if row is None: raise RuntimeError("COMMON_CODEX_CONFIG_MISSING")
        old = dict(row); new = copy.deepcopy(old); new["value"] = patch_toml(old["value"], self.desired, self.core)
        add("settings", {"key": old["key"]}, old, new)
        for table, field in (("providers", "settings_config"), ("proxy_live_backup", "original_config")):
            for row in c.execute('select * from "'+table+'" where app_type=\'codex\'').fetchall():
                old = dict(row); obj = json.loads(old[field]); updated = copy.deepcopy(obj)
                updated["config"] = patch_toml(obj["config"], self.desired, self.core)
                new = copy.deepcopy(old); new[field] = old[field] if updated == obj else dumps(updated)
                pk = {"id":old["id"], "app_type":"codex"} if table == "providers" else {"app_type":"codex"}
                add(table, pk, old, new)
        return entries

    def prepare(self):
        self.preflight()
        raw = self.config.read_bytes() if self.config.exists() else b""
        candidate = patch_toml(raw, self.desired, self.core)
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        folder = self.root/"backups"/("codex-memory-repair-"+stamp+"-"+uuid.uuid4().hex[:8])
        private_directory(folder)
        (folder/"config.before.toml").write_bytes(raw); (folder/"config.after.toml").write_bytes(candidate)
        adapter_before = self.adapter.read_bytes() if self.adapter.exists() else None
        if adapter_before is not None: (folder/"adapter.before.py").write_bytes(adapter_before)
        source = self.source.read_bytes(); (folder/"adapter.after.py").write_bytes(source)
        rows = []
        if self.db:
            with closing(sqlite3.connect(self.db.as_uri()+"?mode=ro", uri=True)) as c, c:
                c.execute("begin")
                with closing(sqlite3.connect(folder/"mmcapi.before.sqlite")) as dst: c.backup(dst)
                rows = self.plan_rows(c)
        (folder/"rows.private.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
        tx = {"schema":"codex-memory-persistent-repair/v1", "phase":"PREPARED", "backup":str(folder),
              "user_home":str(self.user_home), "codex_home":str(self.codex_home), "install_root":str(self.root), "mmcapi_db":str(self.db) if self.db else None,
              "config_existed":self.config.exists(), "config_before_sha256":sha(raw), "config_after_sha256":sha(candidate),
              "adapter_before_sha256":sha(adapter_before) if adapter_before is not None else None, "adapter_after_sha256":sha(source),
              "registration":self.desired, "protected":self.protected(), "source_rows_changed":len(rows)}
        self.save(folder, tx)
        return folder

    def save(self, folder, tx): atomic(folder/"transaction.json", (json.dumps(tx, ensure_ascii=False, indent=2)+"\n").encode())
    def load(self, folder):
        tx = json.loads((folder/"transaction.json").read_text(encoding="utf-8"))
        if tx.get("schema") != "codex-memory-persistent-repair/v1" or not same_path(tx["install_root"], self.root) or not same_path(tx["codex_home"], self.codex_home): raise RuntimeError("TRANSACTION_SCOPE_MISMATCH")
        if tx.get("mmcapi_db") != (str(self.db) if self.db else None): raise RuntimeError("TRANSACTION_DATABASE_SCOPE_MISMATCH")
        return tx, json.loads((folder/"rows.private.json").read_text(encoding="utf-8"))

    def apply(self, folder, fault=None):
        tx, rows = self.load(folder)
        if tx["phase"] != "PREPARED": raise RuntimeError("PREPARED_TRANSACTION_REQUIRED")
        self.preflight()
        raw = self.config.read_bytes() if self.config.exists() else b""
        if sha(raw) != tx["config_before_sha256"] or self.protected() != tx["protected"]: raise RuntimeError("CONFIG_OR_CREDENTIAL_CAS_FAILED")
        current_adapter = sha(self.adapter.read_bytes()) if self.adapter.exists() else None
        if current_adapter != tx["adapter_before_sha256"]: raise RuntimeError("ADAPTER_CAS_FAILED")
        c = sqlite3.connect(self.db, timeout=5) if self.db else None
        swapped = False
        committed = False
        try:
            if c:
                c.row_factory = sqlite3.Row; c.execute("begin immediate")
                for e in rows:
                    if read_row(c,e) != e["before"]: raise RuntimeError("SOURCE_ROW_CAS_FAILED")
                for e in rows: write_row(c,e,e["after"])
            fresh = self.config.read_bytes() if self.config.exists() else b""
            if sha(fresh) != tx["config_before_sha256"]: raise RuntimeError("CONFIG_CAS_FAILED_BEFORE_SWAP")
            atomic(self.adapter, (folder/"adapter.after.py").read_bytes())
            atomic(self.config, (folder/"config.after.toml").read_bytes()); swapped = True
            if fault == "after_swap": raise RuntimeError("INJECTED_AFTER_SWAP_FAILURE")
            if self.protected() != tx["protected"]: raise RuntimeError("PROTECTED_FILE_CHANGED")
            if c: c.commit()
            committed = True
            tx["phase"] = "APPLIED"; self.save(folder,tx)
            atomic(self.state, (json.dumps({"schema":"codex-memory-repair-pointer/v1", "transaction":str(folder)}, indent=2)+"\n").encode())
            return {"status":"APPLIED_VERIFIED", "transaction":str(folder), "source_rows_changed":len(rows)}
        except Exception:
            if committed:
                self.rollback(folder)
                raise
            if c: c.rollback()
            if swapped:
                if sha(self.config.read_bytes()) == tx["config_after_sha256"]:
                    if tx["config_existed"]: atomic(self.config, (folder/"config.before.toml").read_bytes())
                    else: self.config.unlink()
            if self.adapter.exists() and sha(self.adapter.read_bytes()) == tx["adapter_after_sha256"]:
                if tx["adapter_before_sha256"]: atomic(self.adapter, (folder/"adapter.before.py").read_bytes())
                else: self.adapter.unlink()
            tx["phase"] = "FAILED_ROLLED_BACK"; self.save(folder,tx)
            raise
        finally:
            if c: c.close()

    def verify(self, folder):
        tx, rows = self.load(folder)
        self.preflight()
        if tx["phase"] != "APPLIED": raise RuntimeError("APPLIED_TRANSACTION_REQUIRED")
        live = parse(self.config.read_bytes())
        if live.get("mcp_servers", {}).get("semantic_memory") != tx["registration"] or sha(self.adapter.read_bytes()) != tx["adapter_after_sha256"]: raise RuntimeError("INSTALLED_CONFIG_OR_ADAPTER_DRIFT")
        if any(live.get("mcp_servers", {}).get(n, {}).get("type") == "stdio" for n in MCP_NAMES): raise RuntimeError("OBSOLETE_STDIO_FIELD_RETURNED")
        if self.db:
            with closing(sqlite3.connect(self.db.as_uri()+"?mode=ro", uri=True)) as c, c:
                c.row_factory = sqlite3.Row
                if self.plan_rows(c): raise RuntimeError("PERSISTENT_REGISTRATION_DRIFT")
                if c.execute("pragma quick_check").fetchone()[0] != "ok": raise RuntimeError("SOURCE_DATABASE_INTEGRITY_FAILED")
        return {"status":"PASS", "transaction":str(folder), "mmcapi_checked":bool(self.db), "source_rows_checked":len(rows), "adapter_sha256":tx["adapter_after_sha256"], "registration":tx["registration"]}

    def rollback(self, folder):
        tx, rows = self.load(folder)
        current = sha(self.config.read_bytes()) if self.config.exists() else sha(b"")
        if current not in (tx["config_before_sha256"], tx["config_after_sha256"]): raise RuntimeError("NEWER_CONFIG_ROLLBACK_REFUSED")
        adapter_sha = sha(self.adapter.read_bytes()) if self.adapter.exists() else None
        if adapter_sha not in (tx["adapter_before_sha256"], tx["adapter_after_sha256"]): raise RuntimeError("NEWER_ADAPTER_ROLLBACK_REFUSED")
        c = sqlite3.connect(self.db, timeout=5) if self.db else None
        try:
            if c:
                c.row_factory = sqlite3.Row; c.execute("begin immediate")
                for e in rows:
                    if read_row(c,e) not in (e["before"], e["after"]): raise RuntimeError("NEWER_SOURCE_ROW_ROLLBACK_REFUSED")
                for e in reversed(rows): write_row(c,e,e["before"])
            if current == tx["config_after_sha256"]:
                if tx["config_existed"]: atomic(self.config, (folder/"config.before.toml").read_bytes())
                elif self.config.exists(): self.config.unlink()
            if adapter_sha == tx["adapter_after_sha256"]:
                if tx["adapter_before_sha256"]: atomic(self.adapter, (folder/"adapter.before.py").read_bytes())
                else: self.adapter.unlink()
            if c: c.commit()
            tx["phase"] = "ROLLED_BACK"; self.save(folder,tx)
            if self.state.exists():
                pointer = json.loads(self.state.read_text(encoding="utf-8"))
                if pointer.get("transaction") == str(folder): self.state.unlink()
            return {"status":"ROLLED_BACK", "transaction":str(folder), "memory_data_preserved":True}
        finally:
            if c: c.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("check", "prepare", "apply", "verify", "rollback"))
    parser.add_argument("--user-home", type=Path, default=Path.home())
    parser.add_argument("--codex-home", type=Path)
    parser.add_argument("--install-root", type=Path)
    parser.add_argument("--mmcapi-db", type=Path)
    parser.add_argument("--transaction", type=Path)
    a = parser.parse_args()
    codex_home = a.codex_home or Path(os.environ.get("CODEX_HOME") or a.user_home/".codex")
    root = a.install_root or Path(os.environ.get("SEMANTIC_MEMORY_HOME") or Path(os.environ.get("LOCALAPPDATA") or a.user_home/"AppData/Local")/"SemanticMemory")
    db = a.mmcapi_db
    if db is None and same_path(codex_home, a.user_home/".codex") and (a.user_home/".mmcapi/mmcapi.db").exists(): db = a.user_home/".mmcapi/mmcapi.db"
    repair = Repair(a.user_home, codex_home, root, db)
    folder = a.transaction
    if folder is None and repair.state.exists(): folder = Path(json.loads(repair.state.read_text(encoding="utf-8"))["transaction"])
    if a.mode == "check":
        repair.preflight()
        patch_toml(repair.config.read_bytes() if repair.config.exists() else b"", repair.desired, repair.core)
        if repair.db:
            with closing(sqlite3.connect(repair.db.as_uri()+"?mode=ro",uri=True)) as c:
                c.execute("begin"); repair.plan_rows(c)
        result = {"status":"COMPATIBLE_ZERO_WRITE", "wrote":False}
    elif a.mode == "rollback" and folder is None:
        result = {"status":"NOTHING_TO_ROLLBACK", "code":"NO_PERSISTENT_REPAIR_TRANSACTION", "wrote":False,
                  "registration_unchanged":True}
    elif a.mode == "prepare":
        folder = repair.prepare(); result = {"status":"PREPARED", "transaction":str(folder)}
    elif a.mode == "apply":
        if folder and repair.state.exists():
            result = repair.verify(folder); result["status"] = "REPLAYED_ZERO_WRITE"
        else:
            folder = folder or repair.prepare()
            result = repair.apply(folder)
            try: repair.verify(folder)
            except Exception:
                repair.rollback(folder); raise
    else:
        if folder is None: raise RuntimeError("TRANSACTION_REQUIRED")
        result = getattr(repair,a.mode)(folder)
    print(json.dumps(result, ensure_ascii=False))

if __name__ == "__main__":
    try: main()
    except Exception as error:
        code = str(error)
        if not re.fullmatch(r"[A-Z][A-Z0-9_]+", code): code = "REPAIR_FAILED_CLOSED"
        print(json.dumps({"status":"FAILED_CLOSED", "code":code}))
        raise SystemExit(1)
