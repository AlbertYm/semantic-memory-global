"""Build a clean Windows distribution from one verified native runtime; Python 3.11+."""
import argparse, hashlib, json, pathlib, shutil, zipfile

ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def write_json(path,value):path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def build(runtime,out):
    release=json.loads((ROOT/'RELEASE.json').read_text(encoding='utf-8'))
    if release.get('unreleased_source_revision'):
        raise ValueError('Source maintenance is not a published release. Set a new release version, tag, archive name and acceptance record before building; do not reuse the R5 identity.')
    if sha(runtime)!=release['runtime_sha256']:raise ValueError('Runtime SHA256 does not match RELEASE.json')
    bundle=out/release['archive_name'].removesuffix('.zip')
    if bundle.exists():raise FileExistsError('Release directory already exists; choose an empty output directory')
    out.mkdir(parents=True,exist_ok=True);shutil.copytree(ROOT/'packaging/windows',bundle,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    for name in ['README.md','LICENSE','THIRD_PARTY.md','RELEASE.json']:
        shutil.copyfile(ROOT/name,bundle/name)
    for name in ['docs','licenses','runtime']:shutil.copytree(ROOT/name,bundle/name)
    payload=bundle/'payload';payload.mkdir()
    names={'mcp':'semantic-memory-mcp.exe','hook':'semantic-memory-hook.exe','manager':'semantic-memory-manager.exe'}
    for name in names.values():shutil.copyfile(runtime,payload/name)
    server={'name':'io.github.AlbertYm/semantic-memory-global','version':release['runtime_version'],'description':'Auditable local memory across Codex workspaces','repository':{'url':release['repository'],'source':'github'},'packages':[]}
    write_json(payload/'server.json',server)
    write_json(payload/'payload-manifest.json',{'schema':'stage14-payload-manifest/v1','version':release['runtime_version'],'version_id':release['runtime_version_id'],'entrypoints':names,'files':[{'path':p.name,'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(payload.iterdir())]})
    plugin=bundle/'plugin/semantic-memory'; pm=plugin/'.codex-plugin/plugin.json'
    obj=json.loads(pm.read_text(encoding='utf-8'));obj['version']=release['version'];write_json(pm,obj)
    write_json(bundle/'installer-manifest.json',{'schema':'stage14-offline-installer-manifest/v1','version':release['version'],'personal_plugin':{'root':'plugin/semantic-memory','manifest':'plugin/semantic-memory/.codex-plugin/plugin.json','files':[{'path':p.relative_to(bundle).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(plugin.rglob('*')) if p.is_file()]}})
    files=[{'path':p.relative_to(bundle).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(bundle.rglob('*')) if p.is_file()]
    write_json(bundle/'RELEASE-MANIFEST.json',{'schema':'semantic-memory-public-release-manifest/v1','version':release['version'],'runtime_version_id':release['runtime_version_id'],'files':files})
    allfiles=[p for p in sorted(bundle.rglob('*')) if p.is_file()]
    (bundle/'SHA256SUMS.txt').write_text(''.join(sha(p)+'  '+p.relative_to(bundle).as_posix()+'\n' for p in allfiles),encoding='ascii')
    archive=out/release['archive_name']
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for p in sorted(bundle.rglob('*')):
            if p.is_file():z.write(p,arcname=bundle.name+'/'+p.relative_to(bundle).as_posix())
    (out/(archive.name+'.sha256')).write_text(sha(archive)+'  '+archive.name+'\n',encoding='ascii')
    return {'archive':str(archive),'sha256':sha(archive),'bytes':archive.stat().st_size,'bundle':str(bundle),'version':release['version']}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--runtime-exe',required=True,type=pathlib.Path);p.add_argument('--output',required=True,type=pathlib.Path)
    a=p.parse_args();print(json.dumps(build(a.runtime_exe,a.output)))
