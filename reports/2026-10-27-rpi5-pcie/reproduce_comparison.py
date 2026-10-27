#!/usr/bin/env python3
"""Rebuild this report's matched point comparisons from regenerated case stats."""
import argparse
import csv
import json
from pathlib import Path
DIMS=('file_bytes','block_bytes','concurrency','pattern','operation')
METRICS=('bandwidth_mib_s','iops','p95_latency_us')

def flatten(value,prefix=''):
    result={}
    for key,item in value.items():
        name=prefix+key
        if isinstance(item,dict):result.update(flatten(item,name+'.'))
        else:result[name]=json.dumps(item) if isinstance(item,list) else item
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('per_case',type=Path);p.add_argument('--output',type=Path,required=True);p.add_argument('--baseline',default='gen2');p.add_argument('--candidate',default='gen3');args=p.parse_args()
    cases=json.loads(args.per_case.read_text());index={(tuple(row[k] for k in DIMS),row['metric'],row['mode']):row for row in cases};output=[]
    keys=sorted({tuple(row[k] for k in DIMS) for row in cases if row['mode'] in (args.baseline,args.candidate)})
    for key in keys:
        row={**dict(zip(DIMS,key)),'baseline_label':args.baseline,'candidate_label':args.candidate}
        for metric in METRICS:
            b=index[key,metric,args.baseline];c=index[key,metric,args.candidate]
            bs={k:v for k,v in b.items() if k not in DIMS+('mode','metric')};cs={k:v for k,v in c.items() if k not in DIMS+('mode','metric')}
            ratio=c['mean']/b['mean'] if b['mean'] else None
            endpoints=[b.get('mean_ci95_low'),b.get('mean_ci95_high'),c.get('mean_ci95_low'),c.get('mean_ci95_high')]
            overlap=None if any(v is None for v in endpoints) else max(endpoints[0],endpoints[2])<=min(endpoints[1],endpoints[3])
            row[metric]={'baseline':bs,'candidate':cs,'candidate_over_baseline':ratio,'change_percent':100*(ratio-1) if ratio is not None else None,'ratio_ci_method':'not computed; individual mean CIs retained','mean_ci_overlap':overlap}
        output.append(row)
    args.output.mkdir(parents=True,exist_ok=True);(args.output/'comparison.json').write_text(json.dumps(output,indent=2,allow_nan=False)+'\n')
    rows=[flatten(row) for row in output]
    with (args.output/'comparison.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    print(f'{len(output)} matched operation views')
if __name__=='__main__':main()
