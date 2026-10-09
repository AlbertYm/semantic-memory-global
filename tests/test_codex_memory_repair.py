"""Isolated persistent-source transactions; never read the user's credentials or databases."""
import copy
from contextlib import closing
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/"packaging/windows/memory-adapter"
spec = importlib.util.spec_from_file_location("repair", SOURCE/"repair_codex_memory.py")
repair = importlib.util.module_from_spec(spec); spec.loader.exec_module(repair)

class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.scratch = ROOT/"build/repair-tests"
        self.scratch.mkdir(parents=True, exist_ok=True)
        self.temp = Path(tempfile.mkdtemp(prefix="case-", dir=self.scratch)).resolve()
        self.user = self.temp/"User home 中文"
        self.codex = self.user/".codex"; self.codex.mkdir(parents=True)
        self.runtime = self.temp/"runtime with space"; (self.runtime/"bin").mkdir(parents=True)
        self.core = self.runtime/"bin/semantic-memory-mcp.exe"; self.core.write_bytes(b"isolated-core-integrity-fixture")
        (self.runtime/".semantic-memory-managed.json").write_text(json.dumps({"schema":"stage14-install-marker/v2", "product":"semantic-memory", "stable_entrypoint_sha256":{self.core.name:repair.sha(self.core.read_bytes())}}), encoding="utf-8")
        (self.codex/"auth.json").write_text('{"fixture":"SYNTHETIC_AUTH_NOT_A_CREDENTIAL"}', encoding="utf-8")
        self.base = 'model = "fixture-model"\n[model_providers.fixture]\nbase_url = "https://invalid.example/v1"\nenv_key = "SYNTHETIC_FIXTURE_ENV"\n[mcp_servers.memory]\ncommand = "unchanged-memory"\ntype = "stdio"\n'
        (self.codex/"config.toml").write_bytes(b"\xef\xbb\xbf"+self.base.replace("\n", "\r\n").encode("utf-8"))
        self.db = self.user/".mmcapi/mmcapi.db"; self.db.parent.mkdir()
        with closing(sqlite3.connect(self.db)) as c, c:
            c.executescript('''
                create table mcp_servers(id text primary key,name text,server_config text,description text,homepage text,docs text,tags text,enabled_codex integer,enabled_claude integer);
                create table settings(key text primary key,value text);
                create table providers(id text,app_type text,settings_config text,primary key(id,app_type));
                create table proxy_live_backup(app_type text primary key,original_config text);
            ''')
            c.execute("insert into mcp_servers values(?,?,?,?,?,?,?,?,?)", ("memory", "fixture", '{"command":"unchanged-memory","type":"stdio"}', "", "", "", "[]", 1, 0))
            c.execute("insert into settings values(?,?)", ("common_config_codex", self.base))
            for name in ("provider-a", "provider-b"):
                c.execute("insert into providers values(?,?,?)", (name, "codex", json.dumps({"config":self.base,"auth":{"fixture":"SYNTHETIC_NO_SECRET"},"other":{"preserved":True}})))
            c.execute("insert into providers values(?,?,?)", ("other-app", "claude", json.dumps({"fixture":"unchanged"})))
            c.execute("insert into proxy_live_backup values(?,?)", ("codex", json.dumps({"config":self.base,"fixture":"unchanged"})))
        self.r = repair.Repair(self.user,self.codex,self.runtime,self.db)
        self.before = (self.codex/"config.toml").read_bytes()
        self.rows_before = self.rows()

    def rows(self):
        with closing(sqlite3.connect(self.db)) as c, c:
            return {t:c.execute("select * from "+t+" order by 1").fetchall() for t in ("mcp_servers","settings","providers","proxy_live_backup")}

    def tearDown(self):
        if not self.temp.is_relative_to(self.scratch.resolve()): raise RuntimeError("Unsafe cleanup target")
        shutil.rmtree(self.temp)

    def test_install_verify_rollback_preserves_sources_and_auth(self):
        auth = (self.codex/"auth.json").read_bytes()
        folder = self.r.prepare()
        self.assertEqual(self.rows(),self.rows_before)
        self.assertEqual((self.codex/"config.toml").read_bytes(),self.before)
        self.r.apply(folder)
        self.assertEqual(self.r.verify(folder)["status"],"PASS")
        live = repair.parse((self.codex/"config.toml").read_bytes())
        self.assertEqual(live["model"],"fixture-model")
        self.assertNotIn("type",live["mcp_servers"]["memory"])
        self.assertEqual(live["mcp_servers"]["semantic_memory"],self.r.desired)
        with closing(sqlite3.connect(self.db)) as c, c:
            for row in c.execute("select settings_config from providers where app_type='codex'"):
                obj = json.loads(row[0]); self.assertEqual(obj["auth"],{"fixture":"SYNTHETIC_NO_SECRET"})
                self.assertEqual(obj["other"],{"preserved":True})
                self.assertEqual(repair.parse(obj["config"])["mcp_servers"]["semantic_memory"],self.r.desired)
        data = self.runtime/"data"; data.mkdir(); sentinel = data/"retained.txt"; sentinel.write_text("new memory stays",encoding="utf-8")
        self.r.rollback(folder)
        self.assertEqual(self.rows(),self.rows_before)
        self.assertEqual((self.codex/"config.toml").read_bytes(),self.before)
        self.assertEqual((self.codex/"auth.json").read_bytes(),auth)
        self.assertEqual(sentinel.read_text(),"new memory stays")
        self.assertFalse(self.r.adapter.exists())

    def test_existing_native_registration_is_upgraded(self):
        native = copy.deepcopy(self.r.desired); native.update(command=str(self.core),args=[])
        raw = repair.patch_toml(self.base,native,self.core)
        upgraded = repair.patch_toml(raw,self.r.desired,self.core)
        self.assertEqual(repair.parse(upgraded)["mcp_servers"]["semantic_memory"],self.r.desired)

    def test_unknown_registration_rejected_without_mutation(self):
        (self.codex/"config.toml").write_text(self.base+'\n[mcp_servers.semantic_memory]\ncommand="unrecognized-server"\n',encoding="utf-8")
        before = (self.codex/"config.toml").read_bytes()
        with self.assertRaisesRegex(RuntimeError,"UNKNOWN_SEMANTIC_REGISTRATION"):self.r.prepare()
        self.assertEqual((self.codex/"config.toml").read_bytes(),before);self.assertEqual(self.rows(),self.rows_before)

    def test_source_row_cas_refuses_concurrent_provider_edit(self):
        folder = self.r.prepare()
        with closing(sqlite3.connect(self.db)) as c, c:c.execute("update providers set settings_config='{}' where id='provider-a'")
        before = self.rows()
        with self.assertRaisesRegex(RuntimeError,"SOURCE_ROW_CAS_FAILED"):self.r.apply(folder)
        self.assertEqual(self.rows(),before);self.assertEqual((self.codex/"config.toml").read_bytes(),self.before)

    def test_config_cas_refuses_concurrent_edit(self):
        folder = self.r.prepare(); changed = self.before+b"\n# newer user edit\n"
        (self.codex/"config.toml").write_bytes(changed)
        with self.assertRaisesRegex(RuntimeError,"CONFIG_OR_CREDENTIAL_CAS_FAILED"):self.r.apply(folder)
        self.assertEqual((self.codex/"config.toml").read_bytes(),changed);self.assertEqual(self.rows(),self.rows_before)

    def test_failure_after_swap_automatically_restores(self):
        folder = self.r.prepare()
        with self.assertRaisesRegex(RuntimeError,"INJECTED_AFTER_SWAP_FAILURE"):self.r.apply(folder,fault="after_swap")
        self.assertEqual(self.rows(),self.rows_before);self.assertEqual((self.codex/"config.toml").read_bytes(),self.before)
        self.assertFalse(self.r.adapter.exists());self.assertFalse(self.r.state.exists())

    def test_rollback_refuses_newer_config(self):
        folder = self.r.prepare();self.r.apply(folder)
        newer = (self.codex/"config.toml").read_bytes()+b"\n# newer\n"; (self.codex/"config.toml").write_bytes(newer)
        rows = self.rows()
        with self.assertRaisesRegex(RuntimeError,"NEWER_CONFIG_ROLLBACK_REFUSED"):self.r.rollback(folder)
        self.assertEqual((self.codex/"config.toml").read_bytes(),newer);self.assertEqual(self.rows(),rows)

    def test_rollback_refuses_newer_source_row(self):
        folder = self.r.prepare();self.r.apply(folder)
        with closing(sqlite3.connect(self.db)) as c, c:c.execute("update settings set value='newer' where key='common_config_codex'")
        rows = self.rows(); config = (self.codex/"config.toml").read_bytes()
        with self.assertRaisesRegex(RuntimeError,"NEWER_SOURCE_ROW_ROLLBACK_REFUSED"):self.r.rollback(folder)
        self.assertEqual(self.rows(),rows);self.assertEqual((self.codex/"config.toml").read_bytes(),config)

    def test_shared_other_application_transport_is_protected(self):
        with closing(sqlite3.connect(self.db)) as c, c:c.execute("update mcp_servers set enabled_claude=1 where id='memory'")
        before = self.rows()
        with self.assertRaisesRegex(RuntimeError,"SHARED_APPLICATION_TRANSPORT_REQUIRES_REVIEW"):self.r.prepare()
        self.assertEqual(before,self.rows());self.assertEqual((self.codex/"config.toml").read_bytes(),self.before)

    def test_no_mmcapi_and_initial_config_absent(self):
        (self.codex/"config.toml").unlink()
        r = repair.Repair(self.user,self.codex,self.runtime)
        folder = r.prepare();r.apply(folder);self.assertFalse(r.verify(folder)["mmcapi_checked"])
        r.rollback(folder);self.assertFalse((self.codex/"config.toml").exists());self.assertEqual(self.rows(),self.rows_before)

    def test_repeated_cli_apply_is_zero_write(self):
        cmd = [sys.executable,"-I",str(SOURCE/"repair_codex_memory.py"),"apply","--user-home",str(self.user),"--codex-home",str(self.codex),"--install-root",str(self.runtime),"--mmcapi-db",str(self.db)]
        a = subprocess.run(cmd,capture_output=True,text=True,encoding="utf-8",check=True)
        self.assertEqual(json.loads(a.stdout)["status"],"APPLIED_VERIFIED")
        before = self.rows();config = (self.codex/"config.toml").read_bytes()
        b = subprocess.run(cmd,capture_output=True,text=True,encoding="utf-8",check=True)
        self.assertEqual(json.loads(b.stdout)["status"],"REPLAYED_ZERO_WRITE")
        self.assertEqual(before,self.rows());self.assertEqual(config,(self.codex/"config.toml").read_bytes())

    def test_adapter_tamper_and_runtime_drift_rejected(self):
        folder = self.r.prepare();self.r.apply(folder)
        self.r.adapter.write_bytes(b"tampered")
        with self.assertRaisesRegex(RuntimeError,"INSTALLED_CONFIG_OR_ADAPTER_DRIFT"):self.r.verify(folder)
        self.core.write_bytes(b"tampered")
        with self.assertRaisesRegex(RuntimeError,"RUNTIME_INTEGRITY_MISMATCH"):self.r.preflight()

    def test_custom_codex_home_does_not_auto_select_real_mmcapi(self):
        custom = self.user/"isolated-codex"
        r = subprocess.run([sys.executable,"-I",str(SOURCE/"repair_codex_memory.py"),"apply","--user-home",str(self.user),"--codex-home",str(custom),"--install-root",str(self.runtime)],capture_output=True,text=True,encoding="utf-8",check=True)
        self.assertEqual(json.loads(r.stdout)["source_rows_changed"],0);self.assertEqual(self.rows(),self.rows_before)

    def test_verify_allows_unrelated_model_auth_and_provider_changes(self):
        folder = self.r.prepare();self.r.apply(folder)
        path = self.codex/"config.toml"
        path.write_bytes(path.read_bytes().replace(b'fixture-model',b'new-fixture-model'))
        (self.codex/"auth.json").write_text('{"fixture":"new-synthetic-auth"}',encoding="utf-8")
        with repair.closing(sqlite3.connect(self.db)) as c, c:
            for row in c.execute("select id,settings_config from providers where app_type='codex'").fetchall():
                obj = json.loads(row[1]);obj["auth"]={"fixture":"updated-synthetic-auth"}
                obj["config"]=obj["config"].replace("fixture-model","updated-model")
                c.execute("update providers set settings_config=? where id=? and app_type='codex'",(json.dumps(obj),row[0]))
        self.assertEqual(self.r.verify(folder)["status"],"PASS")

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell entry point")
    def test_powershell_entry_apply_verify_and_rollback(self):
        for mode,status in (("Apply","APPLIED_VERIFIED"),("Verify","PASS"),("Rollback","ROLLED_BACK")):
            args=["powershell.exe","-NoLogo","-NoProfile","-NonInteractive","-ExecutionPolicy","Bypass","-File",str(ROOT/"packaging/windows/Repair-Codex-Memory.ps1"),"-Mode",mode,"-UserHome",str(self.user),"-CodexHome",str(self.codex),"-InstallRoot",str(self.runtime),"-MmcapiDatabase",str(self.db),"-PythonExe",sys.executable,"-AllowRunningCodexForIsolatedTest"]
            r=subprocess.run(args,capture_output=True,text=True,encoding="utf-8",check=True)
            self.assertEqual(json.loads(r.stdout)["status"],status)
        self.assertEqual(self.rows(),self.rows_before)

    def test_unreleased_sources_cannot_reuse_r5_archive_identity(self):
        spec=importlib.util.spec_from_file_location("builder",ROOT/"tools/build_release.py")
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        output=self.temp/"unreleased-dist"
        with self.assertRaisesRegex(ValueError,"not a published release"):builder.build(self.core,output)
        self.assertFalse(output.exists())

if __name__ == "__main__": unittest.main()
