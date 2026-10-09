"""Windows ZIP acceptance. Explicit archives only; all user/config/data are synthetic."""
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import tomllib
import unittest
import uuid
import zipfile

from test_project_adapter import Client

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(os.name == 'nt' and os.environ.get('SEMANTIC_MEMORY_TEST_ARCHIVE'), 'Set SEMANTIC_MEMORY_TEST_ARCHIVE to the Windows ZIP.')
class BundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = ROOT/'build/bundle-tests'; cls.scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = (cls.scratch/('release-'+uuid.uuid4().hex[:8])).resolve()
        cls.temp.mkdir()  # normal inherited Windows profile ACL for legacy R5
        cls.package = cls.extract(Path(os.environ['SEMANTIC_MEMORY_TEST_ARCHIVE']), 'R6 extract 中文')
        cls.release = json.loads((cls.package/'RELEASE.json').read_text(encoding='utf-8'))
        cls.previous = None
        if os.environ.get('SEMANTIC_MEMORY_TEST_PREVIOUS_ARCHIVE'):
            cls.previous = cls.extract(Path(os.environ['SEMANTIC_MEMORY_TEST_PREVIOUS_ARCHIVE']), 'R5 extract 中文')

    @classmethod
    def extract(cls, archive, name):
        # Reject traversal before touching the isolated output tree.
        out = cls.temp/name
        with zipfile.ZipFile(archive) as z:
            for entry in z.namelist():
                if not (out/entry).resolve().is_relative_to(out.resolve()): raise RuntimeError('Unsafe archive member')
            z.extractall(out)
        return next(p for p in out.iterdir() if p.is_dir())

    @classmethod
    def tearDownClass(cls):
        if not cls.temp.is_relative_to(cls.scratch.resolve()): raise RuntimeError('Unsafe test cleanup')
        shutil.rmtree(cls.temp)

    def profile(self, name):
        base = self.temp/name; user = base/'User 中文 with spaces'; codex = user/'.codex'
        codex.mkdir(parents=True)
        config = codex/'config.toml'; config.write_text('model="synthetic-fixture"\n[mcp_servers.memory]\ncommand="fixture-memory"\ntype="stdio"\n', encoding='utf-8')
        (codex/'auth.json').write_text('{"fixture":"SYNTHETIC_AUTH"}', encoding='utf-8')
        return user,codex,base/'runtime with spaces',config.read_bytes()

    def ps(self, package, script, user, codex, runtime, *extra, success=True):
        cmd=['powershell.exe','-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',str(package/script)]
        if script != 'Uninstall-Bundle.ps1': cmd += ['-UserHome',str(user),'-CodexHome',str(codex)]
        cmd += ['-InstallRoot',str(runtime)]
        if script not in ('Verify-Bundle.ps1',): cmd += ['-AllowRunningCodexForIsolatedTest']
        cmd += list(extra)
        env=os.environ.copy()
        # Test the supported Windows PowerShell 5.1 baseline, independent of
        # Codex's bundled PowerShell 7 module path inherited by Python.
        env['PSModulePath']=str(Path(os.environ['SystemRoot'])/'System32/WindowsPowerShell/v1.0/Modules')
        p=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=240,env=env)
        if success: self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        else: self.assertNotEqual(p.returncode,0,p.stdout+p.stderr)
        return p

    def repair(self, user,codex,runtime,mode):
        return self.ps(self.package,'Repair-Codex-Memory.ps1',user,codex,runtime,'-Mode',mode)

    def client(self,codex,runtime):
        registration=tomllib.loads((codex/'config.toml').read_text(encoding='utf-8-sig'))['mcp_servers']['semantic_memory']
        core=runtime/'app/versions'/self.release['runtime_version_id']/'semantic-memory-mcp.exe'
        return Client(core,runtime/'data',[registration['command'],*registration['args']])

    def capture(self,codex,runtime,label):
        c=self.client(codex,runtime)
        try:
            self.assertEqual([t['name'] for t in c.tools].count('memory_resolve_project'),1)
            self.assertGreater(len(c.tools),40)
            workspace=codex.parent/(label+' workspace 中文');workspace.mkdir()
            _,task=c.tool('memory_task_begin',{'project':str(workspace),'workspace':str(workspace),'scope':'project','session_id':label+'-session','turn_id':label+'-turn','prompt_sha256':hashlib.sha256(b'fixture').hexdigest(),'prompt_length':7,'idempotency_key':label+'-task'})
            r,project=c.tool('memory_resolve_project',{'task_id':task['task_id']});self.assertFalse(r.get('isError'))
            r,item=c.tool('events',{'project':project['project_uuid'],'scope':'global','kind':'lesson','summary':'Synthetic isolated release acceptance '+label+'.','content':'2026-10-09 isolated fixture validates packaged project identity and native capture.','source':'isolated-release-fixture','importance':0.7,'payload':{}})
            self.assertFalse(r.get('isError'));self.assertEqual(item['status'],'accepted')
            _,done=c.tool('memory_task_complete',{'project':project['project_uuid'],'task_id':task['task_id'],'outcome':'completed','attributions':[],'idempotency_key':label+'-complete'})
            self.assertEqual(done['status'],'recorded')
            _,status=c.tool('memory_task_status',{'project':project['project_uuid'],'task_id':task['task_id']})
            self.assertEqual(status['state'],'completed')
            return item['item_id']
        finally:c.close()

    def check_item(self,runtime,item):
        with closing(sqlite3.connect((runtime/'data/__global__-memory.db').as_uri()+'?mode=ro',uri=True)) as c:
            self.assertEqual(c.execute('select count(*) from memory_item where id=?',(item,)).fetchone()[0],1)
            self.assertEqual(c.execute('pragma quick_check').fetchone()[0],'ok')

    def test_archive_integrity_version_and_private_file_exclusion(self):
        manifest=json.loads((self.package/'RELEASE-MANIFEST.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['version'],self.release['version'])
        for file in manifest['files']:
            p=self.package/file['path']
            with p.open('rb') as stream:self.assertEqual(hashlib.file_digest(stream,'sha256').hexdigest(),file['sha256'])
            self.assertEqual(p.stat().st_size,file['bytes'])
        plugin=json.loads((self.package/'plugin/semantic-memory/.codex-plugin/plugin.json').read_text(encoding='utf-8'))
        self.assertEqual(plugin['version'],self.release['version'])
        for p in self.package.rglob('*'):
            self.assertNotIn(p.suffix.casefold(),{'.db','.sqlite','.pyc'})
            self.assertNotIn(p.name.casefold(),{'auth.json','config.toml','__pycache__'})

    def test_fresh_install_replay_real_mcp_rollback_uninstall(self):
        user,codex,runtime,before=self.profile('fresh')
        self.ps(self.package,'Run-Bundle.ps1',user,codex,runtime)
        self.assertTrue((runtime/'codex-memory-repair-state.json').exists())
        self.assertEqual((codex/'auth.json').read_text(),'{"fixture":"SYNTHETIC_AUTH"}')
        config=(codex/'config.toml').read_bytes()
        self.ps(self.package,'Run-Bundle.ps1',user,codex,runtime)
        self.assertEqual((codex/'config.toml').read_bytes(),config)
        item=self.capture(codex,runtime,'fresh')
        self.ps(self.package,'Verify-Bundle.ps1',user,codex,runtime)
        self.repair(user,codex,runtime,'Rollback')
        self.ps(self.package,'Uninstall-Bundle.ps1',user,codex,runtime)
        self.assertEqual((codex/'config.toml').read_bytes(),before)
        self.check_item(runtime,item)

    def test_python_preflight_and_non_ascii_root_are_zero_write(self):
        user,codex,runtime,before=self.profile('preflight')
        bad=self.ps(self.package,'Install-Bundle.ps1',user,codex,runtime,'-PythonExe',str(self.temp/'python-does-not-exist.exe'),success=False)
        self.assertFalse(runtime.exists());self.assertEqual((codex/'config.toml').read_bytes(),before)
        other=runtime.parent/'中文 runtime'
        bad=self.ps(self.package,'Install-Bundle.ps1',user,codex,other,success=False)
        self.assertIn('NON_ASCII_RUNTIME_ROOT_UNSUPPORTED',bad.stderr)
        self.assertFalse(other.exists())

    def test_active_adapter_upgrade_uninstall_guard(self):
        user,codex,runtime,before=self.profile('guards')
        self.ps(self.package,'Install-Bundle.ps1',user,codex,runtime,'-PythonExe',sys.executable)
        current=(codex/'config.toml').read_bytes()
        for script in ('Install-Bundle.ps1','Uninstall-Bundle.ps1'):
            failed=self.ps(self.package,script,user,codex,runtime,success=False)
            self.assertIn('PERSISTENT_REPAIR_ACTIVE',failed.stderr)
            self.assertEqual((codex/'config.toml').read_bytes(),current)
        self.repair(user,codex,runtime,'Rollback')
        self.ps(self.package,'Run-Bundle.ps1',user,codex,runtime)
        self.assertTrue((runtime/'codex-memory-repair-state.json').exists())
        self.repair(user,codex,runtime,'Rollback')
        self.ps(self.package,'Uninstall-Bundle.ps1',user,codex,runtime)

    def test_upgrade_from_original_r5_failure_recovery_and_uninstall(self):
        if self.previous is None:self.skipTest('Set SEMANTIC_MEMORY_TEST_PREVIOUS_ARCHIVE for original R5 upgrade.')
        user,codex,runtime,before=self.profile('upgrade')
        self.ps(self.previous,'Install-Bundle.ps1',user,codex,runtime)
        prior_config=(codex/'config.toml').read_bytes()
        prior_pointer=(runtime/'state/current.json').read_bytes()
        sentinel=runtime/'data/fixture-preserved.txt';sentinel.write_text('retained',encoding='utf-8')
        # Trigger a package plugin hash failure after native Upgrade, before persistent repair.
        plugin=self.package/'plugin/semantic-memory/.codex-plugin/plugin.json';original=plugin.read_bytes()
        try:
            plugin.write_bytes(original+b'\n')
            self.ps(self.package,'Install-Bundle.ps1',user,codex,runtime,'-PythonExe',sys.executable,success=False)
        finally:plugin.write_bytes(original)
        pointer=json.loads((runtime/'state/current.json').read_text(encoding='utf-8'))
        self.assertEqual(pointer['version_id'],json.loads(prior_pointer)['version_id'])
        self.assertEqual((codex/'config.toml').read_bytes(),prior_config)
        self.assertEqual(sentinel.read_text(),'retained')
        self.ps(self.package,'Run-Bundle.ps1',user,codex,runtime)
        state=json.loads((runtime/'install-bundle-state.json').read_text(encoding='utf-8'))
        self.assertEqual(state['runtime_mode'],'Upgrade')
        item=self.capture(codex,runtime,'upgrade')
        self.repair(user,codex,runtime,'Rollback')
        self.ps(self.package,'Uninstall-Bundle.ps1',user,codex,runtime)
        pointer=json.loads((runtime/'state/current.json').read_text(encoding='utf-8'))
        self.assertEqual(pointer['version_id'],json.loads(prior_pointer)['version_id'])
        self.assertEqual((codex/'config.toml').read_bytes(),prior_config)
        self.check_item(runtime,item)

if __name__=='__main__':unittest.main(verbosity=2)
