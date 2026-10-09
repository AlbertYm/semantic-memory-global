"""Native source-build smoke, using disposable data and the actual MCP/Hook entrypoints."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_project_adapter import Client, ROOT

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--runtime',required=True);args=parser.parse_args()
    original=Path(args.runtime).resolve()
    scratch=ROOT/'build';scratch.mkdir(exist_ok=True)
    temporary=Path(tempfile.mkdtemp(prefix='learning-runtime-',dir=scratch))
    payload=temporary/'app/versions/verified-learning';payload.mkdir(parents=True)
    core=payload/'semantic-memory-mcp.exe';shutil.copy2(original,core)
    data=temporary/'data';data.mkdir();workspace=temporary/'workspace';workspace.mkdir()
    os.environ['CBM_VERIFIED_LEARNING']='1'
    client=Client(core,data,command=[str(core)])
    try:
        names=[tool['name'] for tool in client.tools]
        assert 'memory_learning_status' in names and 'memory_learning_control' in names,names
        begin={'project':str(workspace),'workspace':str(workspace),'scope':'project','session_id':'learning-native-session',
            'turn_id':'learning-native-turn','prompt_sha256':hashlib.sha256(b'native learning').hexdigest(),
            'prompt_length':15,'idempotency_key':'learning-native-begin'}
        result,task=client.tool('memory_task_begin',begin);assert not result.get('isError'),task
        result,status=client.tool('memory_learning_status',{'project':str(workspace)})
        assert not result.get('isError') and status['enabled'],status
        hook={'hook_event_name':'PostToolUse','session_id':begin['session_id'],'turn_id':begin['turn_id'],
            'tool_name':'functions.exec_command','tool_use_id':'learning-real-process-result','tool_input':{'cmd':'python --version'}}
        actual=subprocess.run(['python','--version'],capture_output=True,text=True,check=True)
        hook['tool_response']={'exit_code':actual.returncode,'output':actual.stdout,'wall_time_seconds':0}
        env=os.environ.copy();env.update(CBM_DATA_ROOT=str(data),CBM_CACHE_DIR=str(data),CBM_MEMORY_EMBED_BACKEND='static',CBM_MEMORY_AUTO_MAINTAIN='0')
        completed=subprocess.run([str(core),'memory-post-tool'],input=json.dumps(hook),capture_output=True,text=True,encoding='utf-8',env=env,timeout=15)
        assert completed.returncode==0 and 'degraded' not in completed.stdout,(completed.returncode,completed.stdout,completed.stderr)
        result,task_status=client.tool('memory_task_status',{'project':str(workspace),'task_id':task['task_id']})
        assert not result.get('isError') and 'external_verified' in json.dumps(task_status),task_status
        result,bad=client.tool('memory_learning_status',{'project':'unknown-other-project'})
        assert result.get('isError') and bad['code']=='SECURITY_SCOPE_VIOLATION',bad
        result,paused=client.tool('memory_learning_control',{'project':str(workspace),'action':'pause','expected_generation':status['generation'],'idempotency_key':'learning-native-pause'})
        assert not result.get('isError') and not paused['enabled'],paused
        result,resumed=client.tool('memory_learning_control',{'project':str(workspace),'action':'resume','expected_generation':paused['generation'],'idempotency_key':'learning-native-resume'})
        assert not result.get('isError') and resumed['enabled'],resumed
        output={'schema':'verified-learning-native-evidence/v1','runtime_sha256':hashlib.sha256(original.read_bytes()).hexdigest(),
            'tool_count':len(names),'real_process_hook_evidence':True,'scope_guard':True,'pause_resume':True,'synthetic_data':True,
            'live_codex_upgraded':False}
        (scratch/'learning-runtime-evidence.json').write_text(json.dumps(output,indent=2),encoding='utf-8')
        print(json.dumps(output))
    finally:client.close()

if __name__=='__main__':main()
