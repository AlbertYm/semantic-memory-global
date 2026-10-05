"""Apply the published exact-base compatibility changes without touching an installation."""
import argparse, hashlib, json, pathlib

def apply(source, output, manifest):
    spec=json.loads(manifest.read_text(encoding='utf-8'))
    blob=bytearray(source.read_bytes())
    current=hashlib.sha256(blob).hexdigest()
    if current not in (spec['base_sha256'],spec['candidate_sha256']):
        raise ValueError('Unsupported runtime SHA256; original file was not modified')
    if current==spec['base_sha256']:
        for region in spec['regions']:
            old=bytes.fromhex(region['old_bytes']); new=bytes.fromhex(region['new_bytes']); offset=region['offset']
            if len(old)!=len(new) or blob[offset:offset+len(old)]!=old:
                raise ValueError('Patch precondition failed')
            blob[offset:offset+len(old)]=new
    if hashlib.sha256(blob).hexdigest()!=spec['candidate_sha256']:
        raise ValueError('Output verification failed')
    if output.exists():
        if hashlib.sha256(output.read_bytes()).hexdigest()!=spec['candidate_sha256']:
            raise FileExistsError('Refusing to overwrite an unrelated output')
    else:
        output.parent.mkdir(parents=True,exist_ok=True)
        with output.open('xb') as stream: stream.write(blob)
    return spec['candidate_sha256']

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=pathlib.Path);parser.add_argument('output',type=pathlib.Path)
    parser.add_argument('--manifest',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[1]/'runtime/PATCH-MANIFEST.json')
    args=parser.parse_args()
    print(apply(args.source,args.output,args.manifest))
