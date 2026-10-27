#!/usr/bin/env python3
"""Software-only negative fixtures derived from one supplied real trial.

Creates explicitly marked synthetic validation artifacts only in a temporary
folder. Does not collect measurements or retain fictitious campaign results.
"""
import argparse
import copy
import json
from pathlib import Path
import tempfile
from validate_campaign import engine_checksum, validate


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('actual_run',type=Path);args=parser.parse_args()
    source_manifest=json.loads((args.actual_run/'manifest.json').read_text())
    source=json.loads(next(iter(sorted((args.actual_run/'trials').glob('*.json')))).read_text())
    manifest=copy.deepcopy(source_manifest);config=source['config']
    manifest.update(mode='SYNTHETIC-VALIDATION-ONLY',synthetic=True,
                    file_sizes=[config['file_size']],block_sizes=[config['block_size']],
                    concurrency=[config['concurrency']],patterns=[config['pattern']],repetitions=2,total_trials=2)
    trials=[]
    for index in range(2):
        trial=copy.deepcopy(source);trial.update(trial_id=index,synthetic=True)
        trial['config'].update(mode=manifest['mode'],repetition=index+1);trials.append(trial)
    def write_trial(root,index,record):
        raw=(json.dumps(record,separators=(',',':'))+'\n').encode();path=root/'trials'/f'{index:06d}.json';path.write_bytes(raw);path.with_suffix('.ok').write_bytes(engine_checksum(raw))
    with tempfile.TemporaryDirectory(prefix='knurl-SYNTHETIC-VALIDATION-ONLY-') as temporary:
        root=Path(temporary);(root/'trials').mkdir()
        def reset():
            for path in (root/'trials').iterdir():path.unlink()
            (root/'manifest.json').write_text(json.dumps(manifest))
            (root/'progress.json').write_text(json.dumps({'schema_version':1,'completed':2,'total':2,'errors':0,'status':'complete'}))
            for index,trial in enumerate(trials):write_trial(root,index,trial)
        reset();assert validate(root)['status']=='PASS'
        raw=root/'trials/000000.json';raw.write_bytes(raw.read_bytes()+b' ')
        assert any('checksum mismatch' in e for e in validate(root)['issues'])
        reset()
        for index in range(2):
            trial=copy.deepcopy(trials[index]);trial['config']['repetition']=2-index;write_trial(root,index,trial)
        assert any('seeded order' in e for e in validate(root)['issues'])
        reset();(root/'trials/000000.ok').unlink()
        assert validate(root)['status']=='FAIL' and validate(root,True)['status']=='FAIL'
        reset();trial=copy.deepcopy(trials[1]);trial['config']['repetition']=1;write_trial(root,1,trial)
        assert any('duplicate configuration' in e for e in validate(root)['issues'])
        reset();(root/'trials/000001.json').unlink();(root/'trials/000001.ok').unlink()
        (root/'progress.json').write_text(json.dumps({'schema_version':1,'completed':1,'total':2,'errors':0,'status':'running'}))
        assert validate(root)['status']=='FAIL' and validate(root,True)['status']=='PASS_PARTIAL'
        reset();trial=copy.deepcopy(trials[0]);trial['trial_id']=2;write_trial(root,2,trial)
        assert any('extra/unexpected JSON IDs' in e for e in validate(root)['issues'])
        reset();unsupported=copy.deepcopy(manifest);unsupported['engine_version']='2.0';(root/'manifest.json').write_text(json.dumps(unsupported))
        assert validate(root)['status']=='FAIL'
    assert engine_checksum(b'')==b'14650fb0739d0383\n'
    print('SYNTHETIC VALIDATION ONLY: positive integrity fixture and corruption/order/missingOK/duplicate/partial/extraID/version guards passed; temporary fixtures removed')
if __name__=='__main__':main()
