#!/usr/bin/env python3
"""SYNTHETIC VALIDATION ONLY. Generates no hardware evidence."""
import json
import math
import sys
from pathlib import Path
from analyze import describe, normalize

out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
assert describe([1,2,3,4],'iops')['exact_mode_status']=='none/all unique'
assert describe([1,1,2,2,3],'iops')['exact_mode']==[1.,2.]
s=describe([1,2,3],'iops')
assert s['sample_std']==1 and s['mean']==2 and s['n']==3
assert s['mean_ci95_low']<2<s['mean_ci95_high']
assert describe([0],'iops')['sample_std'] is None
for mode in ('Gen2','Gen3'):
    for rep in range(1,11):
        hist=[0]*1024;hist[160]=70;hist[176]=30
        r={'schema_version':1,'synthetic':True,'complete':True,'verified':True,'errors':0,
           'config':dict(mode=mode,file_size=268435456,block_size=4096,concurrency=1,pattern='rand-mix70read30write',repetition=rep),
           'measured_seconds':1+rep*.001,
           'read':{'bytes':70*4096,'ops':70,'latency_ns':{'sum':70*1024,'histogram':[70 if i==160 else 0 for i in range(1024)]}},
           'write':{'bytes':30*4096,'ops':30,'latency_ns':{'sum':30*2048,'histogram':[30 if i==176 else 0 for i in range(1024)]}}}
        (out/f'{mode}-{rep}.json').write_text(json.dumps(r))
        r['_source']='synthetic';rows=normalize(r);combined=rows[-1]
        assert combined['operation']=='combined' and combined['count']==100
        assert math.isclose(combined['mean_latency_us'],1.3312)
        assert combined['p50_latency_us']<combined['p95_latency_us']
print('Synthetic statistical invariants passed; fixtures written to',out)
