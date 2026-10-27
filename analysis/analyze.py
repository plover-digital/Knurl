#!/usr/bin/env python3
"""Offline analysis of Knurl JSON/JSONL trials; never runs benchmarks."""
import argparse
import csv
import json
import math
import itertools
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
from scipy.stats import t
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from shapes import overview, pooled_histograms, ccdf
from plot_utils import save_figure

DIMS = ('mode', 'file_bytes', 'block_bytes', 'concurrency', 'pattern')
METRICS = ('bandwidth_mib_s', 'iops', 'mean_latency_us', 'min_latency_us', 'max_latency_us', 'p50_latency_us', 'p95_latency_us', 'p99_latency_us', 'p999_latency_us')

def describe(values, metric):
    a = np.asarray(values, dtype=float)
    if not len(a) or not np.all(np.isfinite(a)):
        raise ValueError('Statistics require finite, nonempty observations')
    n, mean = len(a), float(a.mean())
    std = float(a.std(ddof=1)) if n > 1 else None
    margin = float(t.ppf(.975, n-1) * std / math.sqrt(n)) if n > 1 else None
    counts = Counter(map(float, a)); peak = max(counts.values())
    modes = sorted(k for k, v in counts.items() if v == peak) if peak > 1 else []
    width = 10. if metric == 'bandwidth_mib_s' else 1000. if metric == 'iops' else None
    bins = Counter(math.floor(float(v) / width) for v in a) if width else {}
    bin_peak = max(bins.values()) if bins else 0
    return dict(n=n, min=float(a.min()), max=float(a.max()), mean=mean,
                median=float(np.median(a)), exact_mode=modes,
                exact_mode_status='none/all unique' if not modes else 'mode(s)',
                sample_std=std, cv=std/mean if std is not None and mean != 0 else None,
                p5=float(np.percentile(a, 5)), p95=float(np.percentile(a,95)),
                mean_ci95_low=mean-margin if margin is not None else None,
                mean_ci95_high=mean+margin if margin is not None else None,
                binned_mode_width=width,
                binned_mode_intervals=[[k*width,(k+1)*width] for k,v in sorted(bins.items()) if v==bin_peak])

def load(paths):
    records=[]
    for path in paths:
        p=Path(path)
        files=sorted(p.rglob('*.json'))+sorted(p.rglob('*.jsonl')) if p.is_dir() else [p]
        for f in files:
            if f.suffix == '.jsonl': data=[json.loads(s) for s in f.read_text().splitlines() if s.strip()]
            else:
                data=json.loads(f.read_text()); data=data if isinstance(data,list) else [data]
            for record in data:
                
                if 'schema_version' not in record or 'config' not in record: continue
                record['_source']=str(f); records.append(record)
    return records

def write_csv(path, rows):
    keys=list(dict.fromkeys(k for r in rows for k in r))
    with path.open('w',newline='') as f:
        w=csv.DictWriter(f, fieldnames=keys); w.writeheader()
        w.writerows({k: json.dumps(v) if isinstance(v,(dict,list)) else v for k,v in r.items()} for r in rows)

def histogram_percentiles(hist):
    """Upper-bound quantile estimates from disjoint bins; no percentile averaging."""
    total=sum(x['count'] for x in hist)
    if not total:return {}
    out={}; cumulative=0; targets=iter([('p50_latency_us',.5),('p95_latency_us',.95),('p99_latency_us',.99),('p999_latency_us',.999)])
    target=next(targets,None)
    for b in sorted(hist,key=lambda b:b['upper_us']):
        cumulative+=b['count']
        while target and cumulative>=math.ceil(total*target[1]):
            out[target[0]]=b['upper_us']; target=next(targets,None)
    return out

def normalize(record):
    if record.get('schema_version') != 1 or not record.get('complete') or not record.get('verified') or record.get('errors') != 0:
        raise ValueError(f"Incomplete, unverified, erroneous or unsupported trial: {record.get('_source')}")
    c=record['config']
    dims=dict(zip(DIMS,(c['mode'],c['file_size'],c['block_size'],c['concurrency'],c['pattern'])))
    seconds=record['measured_seconds']
    if seconds<=0:raise ValueError('Nonpositive measurement interval')
    rows=[]
    operations={op:record[op] for op in ('read','write')}
    # Combined quantiles come from pooled counts, never averages of quantiles.
    active=[v for v in operations.values() if v['ops']]
    if len(active)>1:
        pooled=[sum(v['latency_ns']['histogram'][i] for v in active) for i in range(1024)]
        operations['combined']={'bytes':sum(v['bytes'] for v in active),'ops':sum(v['ops'] for v in active),'latency_ns':{'sum':sum(v['latency_ns']['sum'] for v in active),'histogram':pooled}}
    if len(active)>1 and all('min' in v['latency_ns'] and 'max' in v['latency_ns'] for v in active):
        operations['combined']['latency_ns'].update(min=min(v['latency_ns']['min'] for v in active),max=max(v['latency_ns']['max'] for v in active))
    for op,m in operations.items():
        if not m['ops']:continue
        lat=m['latency_ns'];hist=lat['histogram']
        if len(hist)!=1024 or sum(hist)!=m['ops']:raise ValueError('Histogram count mismatch')
        bins=[{'upper_us':(math.ceil(2**(i//16)*(1+(i%16+1)/16))-1)/1000,'count':n} for i,n in enumerate(hist) if n]
        row={**dims,'repetition':c['repetition'],'operation':op,'source':record['_source'],
             'bandwidth_mib_s':m['bytes']/seconds/2**20,'iops':m['ops']/seconds,
             'mean_latency_us':lat['sum']/m['ops']/1000,'count':m['ops'],
             'latency_percentile_method':'pooled histogram upper bound' if op=='combined' else 'histogram upper bound'}
        for extreme in ('min','max'):
            if extreme in lat:row[f'{extreme}_latency_us']=lat[extreme]/1000
        row.update(histogram_percentiles(bins));rows.append(row)
    return rows

def plots(out, cases, baseline=None, candidate=None, validation=False):
    charts=out/'charts'; charts.mkdir(exist_ok=True)
    bandwidth=[r for r in cases if r['metric']=='bandwidth_mib_s']
    latency={(tuple(r[k] for k in DIMS),r['operation']):r for r in cases if r['metric']=='mean_latency_us'}
    plt.figure(figsize=(9,6))
    for mode in sorted({r['mode'] for r in bandwidth}):
        rs=[r for r in bandwidth if r['mode']==mode and (tuple(r[k] for k in DIMS),r['operation']) in latency]
        plt.scatter([latency[(tuple(r[k] for k in DIMS),r['operation'])]['mean'] for r in rs],[r['mean'] for r in rs],label=mode,alpha=.65)
    plt.xlabel('Mean operation latency (µs)');plt.ylabel('Bandwidth (MiB/s)');plt.xscale('log');plt.legend();plt.tight_layout();save_figure(plt.gcf(),charts/'bandwidth_latency.png',validation);plt.close()
    for dimension,label in [('block_bytes','Block size (bytes)'),('concurrency','Concurrency'),('file_bytes','File size (bytes)')]:
        fig,ax=plt.subplots(figsize=(12,7))
        groups=defaultdict(list)
        for r in bandwidth:groups[tuple((k,r[k]) for k in DIMS if k!=dimension)+(('operation',r['operation']),)].append(r)
        # Each curve fixes every other dimension. Many curves are deliberately faint.
        for key,rs in sorted(groups.items()):
            rs=sorted(rs,key=lambda r:r[dimension]); ax.plot([r[dimension] for r in rs],[r['mean'] for r in rs],alpha=.3,marker='.',lw=.8)
        ax.set_xscale('log');ax.set_xlabel(label);ax.set_ylabel('Mean bandwidth (MiB/s)');ax.set_title('Each curve fixes mode, operation and all other dimensions');fig.tight_layout();save_figure(fig,charts/f'{dimension}_sweep.png',validation);plt.close(fig)
    fig,ax=plt.subplots(figsize=(11,5))
    rs=sorted(bandwidth,key=lambda r:tuple(str(r[k]) for k in DIMS)+(r['operation'],))
    ax.errorbar(range(len(rs)),[r['mean'] for r in rs],yerr=[(r['mean_ci95_high']-r['mean']) if r['mean_ci95_high'] is not None else 0 for r in rs],fmt='.',alpha=.6)
    ax.set_xlabel('Case index (see per_case.csv for dimensions)');ax.set_ylabel('Bandwidth (MiB/s), mean ± 95% t CI');fig.tight_layout();save_figure(fig,charts/'variation_ci.png',validation);plt.close(fig)
    gain=[]
    pairs=defaultdict(dict)
    for r in bandwidth:pairs[tuple(r[k] for k in DIMS if k!='mode')+(r['operation'],)][r['mode']]=r['mean']
    for key,values in sorted(pairs.items()):
        if baseline and candidate and values.get(baseline,0)>0 and candidate in values:gain.append((key,values[candidate]/values[baseline]))
    if gain:
        facets=defaultdict(list)
        for key,value in gain:
            file_size,block,concurrency,pattern,op=key
            facets[(file_size,pattern,op)].append((block,concurrency,value))
        columns=3; nrows=math.ceil(len(facets)/columns)
        fig,axes=plt.subplots(nrows,columns,figsize=(13,3*nrows),squeeze=False,layout='constrained')
        for ax,((size,pattern,op),items) in zip(axes.flat,sorted(facets.items())):
            blocks=sorted({x[0] for x in items}); concs=sorted({x[1] for x in items})
            matrix=np.full((len(blocks),len(concs)),np.nan)
            for block,conc,value in items:matrix[blocks.index(block),concs.index(conc)]=value
            ax.imshow(matrix,aspect='auto',cmap='RdYlGn',vmin=0,vmax=2)
            ax.set_xticks(range(len(concs)),concs);ax.set_yticks(range(len(blocks)),[f'{b//1024}K' for b in blocks])
            ax.set_xlabel('Concurrency');ax.set_ylabel('Block size');ax.set_title(f'{size/2**30:g} GiB · {pattern} · {op}',fontsize=9)
            for (i,j),value in np.ndenumerate(matrix):
                if np.isfinite(value):ax.text(j,i,f'{value:.2f}×',ha='center',va='center',fontsize=8)
        for ax in list(axes.flat)[len(facets):]:ax.set_visible(False)
        fig.suptitle(f'{candidate} / {baseline} mean bandwidth ratio (green ≥ 2×; labels show exact ratio)')
        save_figure(fig,charts/'comparison_gain.png',validation);plt.close(fig)
    names=['bandwidth_latency.png','block_bytes_sweep.png','concurrency_sweep.png','file_bytes_sweep.png','variation_ci.png']
    if gain:names.append('comparison_gain.png')
    return [charts/name for name in names]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('inputs',nargs='+');p.add_argument('--output',type=Path,required=True);p.add_argument('--validation-only',action='store_true');p.add_argument('--baseline',help='Exact experiment label used as comparison denominator');p.add_argument('--candidate',help='Exact experiment label used as comparison numerator');p.add_argument('--check-gen-campaign',action='store_true',help='Check the specific full 3,600-trial gen2/gen3 campaign');a=p.parse_args()
    records=load(a.inputs)
    if not records:raise ValueError('No trials found')
    if any(r.get('synthetic',False) for r in records) and not a.validation_only:raise ValueError('Synthetic data requires --validation-only')
    rows=[row for r in records for row in normalize(r)]
    unique=set()
    for r in records:
        c=r['config']; key=(c['mode'],c['file_size'],c['block_size'],c['concurrency'],c['pattern'],c['repetition'])
        if key in unique:raise ValueError(f'Duplicate trial: {key}')
        unique.add(key)
    groups=defaultdict(list)
    for r in rows:
        for metric in METRICS:
            if metric in r:groups[tuple(r[k] for k in DIMS)+(r['operation'],metric)].append(r[metric])
    cases=[{**dict(zip(DIMS+('operation','metric'),key)),**describe(vals,key[-1])} for key,vals in sorted(groups.items())]
    a.output.mkdir(parents=True,exist_ok=True)
    for name,data in [('per_trial',rows),('per_case',cases)]:
        (a.output/f'{name}.json').write_text(json.dumps(data,indent=2,allow_nan=False)+'\n');write_csv(a.output/f'{name}.csv',data)
    labels=sorted({r['mode'] for r in cases})
    if bool(a.baseline) != bool(a.candidate):p.error('--baseline and --candidate must be supplied together')
    if a.baseline and (a.baseline not in labels or a.candidate not in labels or a.baseline==a.candidate):p.error('Comparison labels must be distinct and exist in input')
    if not a.baseline:
        matches={name:[label for label in labels if label.lower()==name] for name in ('gen2','gen3')}
        if all(len(v)==1 for v in matches.values()):a.baseline=matches['gen2'][0];a.candidate=matches['gen3'][0]
    charts=plots(a.output,cases,a.baseline,a.candidate,a.validation_only)
    pooled=pooled_histograms(records)
    (a.output/'pooled_histograms.json').write_text(json.dumps(pooled,indent=2)+'\n')
    write_csv(a.output/'pooled_histograms.csv',pooled)
    charts += overview(a.output/'charts',cases,a.validation_only) + ccdf(a.output/'charts',pooled,a.validation_only)
    cells=Counter((r['config']['mode'],r['config']['file_size'],r['config']['block_size'],r['config']['concurrency'],r['config']['pattern']) for r in records)
    expected=set(itertools.product(('gen2','gen3'),(256*2**20,4*2**30,16*2**30),(4096,16384,65536,2**20),(1,8,32),('seq-read','seq-write','rand-read','rand-write','rand-mix70read30write')))
    actual={(str(k[0]).lower(),)+k[1:] for k in cells}
    complete=actual==expected and all(n==10 for n in cells.values()) and len(records)==3600
    label_text=', '.join(labels)
    campaign_status='No predefined campaign completeness check requested.'
    if a.check_gen_campaign:
        campaign_result='PASS' if complete else 'INCOMPLETE'
        campaign_status=f'Full gen2/gen3 campaign check: {campaign_result}.'
    trial_summary=(f'Analyzed {len(records)} trials across {len(cells)} cases. '
                   f'Experiment labels: {label_text}. '
                   f'Repetitions per case: {min(cells.values())}–{max(cells.values())}. '
                   f'{campaign_status}')
    report=[
        '# '+('SYNTHETIC VALIDATION ONLY' if a.validation_only else 'Knurl benchmark analysis'),
        '',trial_summary,'',
        'Each operation is analyzed separately. Latency percentile summaries describe trial percentile variation; they are not a combined operation percentile. Histogram-derived percentiles use bin upper bounds.',
        '',
        'Mixed combined bandwidth sums read and write traffic. On a full-duplex link, this bidirectional total can exceed a unidirectional payload ceiling. Compare like patterns and operation directions; this sum does not establish a higher link rate. Separate directional statistics remain available.',
        '',
        '[Per-trial CSV](per_trial.csv) · [Per-trial JSON](per_trial.json) · [Per-case CSV](per_case.csv) · [Per-case JSON](per_case.json) · [Pooled histogram CSV](pooled_histograms.csv) · [Pooled histogram JSON](pooled_histograms.json)',
        '',
        'Sample standard deviation and Student-t 95% mean confidence intervals use independent trial values. Inspect variation and campaign completeness before drawing conclusions.',
        '',
    ]
    if not a.validation_only:
        report += ['## Observations', '']
        for mode in sorted({r['mode'] for r in cases}):
            candidates=[r for r in cases if r['mode']==mode and r['metric']=='bandwidth_mib_s']
            if candidates:
                best=max(candidates,key=lambda r:r['mean'])
                report += [f"Highest observed case mean for {mode}: {best['mean']:.2f} MiB/s ({best['pattern']}, {best['operation']}, file {best['file_bytes']} bytes, block {best['block_bytes']} bytes, concurrency {best['concurrency']}, n={best['n']}). This identifies a measured case, not a universal hardware ceiling.", '']
    report += ['The overview uses combined operations for mixed cases and the active direction otherwise. P95 overview curves summarize trial P95 values; the separate pooled CCDF weights each operation equally across repetitions and uses histogram boundaries, not averaged percentiles.', '']
    report += [f'![{f.stem}](charts/{f.name})' for f in charts]
    (a.output/'report.md').write_text('\n'.join(report)+'\n')
    print(f'{len(records)} trials, {len(cases)} metric summaries → {a.output}')
if __name__=='__main__':main()
