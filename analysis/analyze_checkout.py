#!/usr/bin/env python3
"""Offline report for a completed portable Knurl system checkout; runs no workloads."""
import argparse
import csv
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from plot_utils import save_figure


def write_dataset(out, name, rows):
    (out/(name+'.json')).write_text(json.dumps(rows,indent=2,allow_nan=False)+'\n')
    keys=list(dict.fromkeys(k for r in rows for k in r))
    with (out/(name+'.csv')).open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=keys);writer.writeheader()
        writer.writerows({k:json.dumps(v) if isinstance(v,(dict,list)) else v for k,v in r.items()} for r in rows)


def validate_finite(value):
    if isinstance(value,float) and not math.isfinite(value):
        raise ValueError('Nonfinite numeric input')
    if isinstance(value,dict):
        if value.get('synthetic'):
            raise ValueError('Synthetic checkout input is rejected; not hardware evidence')
        for item in value.values():validate_finite(item)
    elif isinstance(value,list):
        for item in value:validate_finite(item)


def numeric(value,field,nullable=False):
    if value is None and nullable:return
    if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value):
        raise ValueError('Expected finite numeric field: '+field)


def validate_summary(summary):
    for key in ('count','min','max','mean','median'):
        numeric(summary[key],'summary.'+key)
    if not isinstance(summary['count'],int) or summary['count']<1:
        raise ValueError('Summary count must be a positive integer')
    for key in ('sd','sample_std','cv','p5','p95','p99','mean_ci95_low','mean_ci95_high','ci_degrees_of_freedom'):
        if key in summary:numeric(summary[key],'summary.'+key,nullable=True)
    for value in summary.get('exact_mode',[]):numeric(value,'summary.exact_mode')


def extract(data):
    validate_finite(data)
    names=[m['module'] for m in data.get('modules',[])]
    if len(names)!=len(set(names)):raise ValueError('Duplicate module names')
    modules={m['module']:m for m in data.get('modules',[])}
    if data.get('schema_version')!=1 or data.get('complete') is not True:
        raise ValueError('Expected complete schema-version-1 checkout JSON')
    repetitions=[];cases=[];rtts=[]
    for name in ('cpu','memory'):
        module=modules.get(name)
        if module is None:continue
        if module.get('complete') is not True:raise ValueError(name+' module incomplete')
        for case in module['cases']:
            dimensions={'module':name,'case':case['name'],'unit':case['unit'],'threads':module['threads']}
            if 'working_set_bytes_per_thread' in case:dimensions['working_set_bytes_per_thread']=case['working_set_bytes_per_thread']
            for rep in case['repetitions']:
                if rep.get('verified') is not True or rep.get('error_code')!=0:raise ValueError('Unverified repetition')
                for field in ('repetition','operations','iterations','accesses','traffic_bytes','error_code'):
                    if field in rep:numeric(rep[field],'repetition.'+field)
                numeric(rep['seconds'],'repetition.seconds')
                numeric(rep['rate' if name=='cpu' else 'value'],'repetition.value')
                if rep['seconds']<=0:raise ValueError('Nonpositive repetition interval')
                repetitions.append({**dimensions,**rep})
            validate_summary(case['summary'])
            if case['summary']['count']!=len(case['repetitions']):raise ValueError('Summary/repetition count mismatch')
            cases.append({**dimensions,**case['summary'],'summary_source':'C engine; preserved verbatim','measurement':module.get('measurement'),'interpretation':case.get('interpretation')})
    net=modules.get('network')
    if net:
        if net.get('verified') is not True:raise ValueError('Unverified network module')
        bysize=defaultdict(list)
        for rep in net['trials']:
            for field in ('repetition','payload_bytes','sent_bytes','seconds','throughput_mib_s'):
                numeric(rep[field],'network.'+field)
            if rep['seconds']<=0:raise ValueError('Nonpositive network interval')
            for value in rep['rtt_us']:numeric(value,'network.rtt_us')
            if not rep['rtt_us'] or not all(math.isfinite(v) and v>=0 for v in rep['rtt_us']):raise ValueError('Invalid RTT observations')
            dimensions={'module':'network','payload_bytes':rep['payload_bytes'],'scope':net['scope'],'physical_link_test':net['physical_link_test']}
            repetitions.append({**dimensions,'case':'throughput_and_pingpong',**rep})
            bysize[rep['payload_bytes']].append(rep)
            for sample,value in enumerate(rep['rtt_us'],1):rtts.append({**dimensions,'repetition':rep['repetition'],'sample':sample,'rtt_us':value})
        for case in net['summary_by_payload']:
            for metric in ('throughput','trial_mean_rtt'):
                summary=case[metric]
                validate_summary(summary)
                if summary['count']!=len(bysize[case['payload_bytes']]):raise ValueError('Network summary/repetition count mismatch')
                cases.append({'module':'network','case':metric,'payload_bytes':case['payload_bytes'],'scope':net['scope'],'physical_link_test':net['physical_link_test'],**summary,'summary_source':'C engine; preserved verbatim'})
    disk=modules.get('disk')
    if disk:
        status=disk.get('status')
        if status not in ('passed','not_run'):raise ValueError('Disk module failed or has unsupported status')
        if status=='passed' and disk.get('full_byte_verification_before_and_after') is not True:raise ValueError('Disk module did not pass verification')
    if not cases:raise ValueError('No CPU, memory or network observations found')
    return modules,repetitions,cases,rtts


def errorbar(ax,rows,xkey,scale=1,label=None):
    rows=sorted(rows,key=lambda r:r[xkey]);x=[r[xkey] for r in rows];y=[r['mean']/scale for r in rows]
    errors=[[(r['mean']-r['mean_ci95_low'])/scale if r.get('mean_ci95_low') is not None else 0 for r in rows],[(r['mean_ci95_high']-r['mean'])/scale if r.get('mean_ci95_high') is not None else 0 for r in rows]]
    ax.errorbar(x,y,yerr=errors,marker='o',capsize=3,label=label)
    ax.set_xscale('log',base=2);ax.grid(alpha=.2)


def charts(out,cases,rtts,validation):
    directory=out/'charts';directory.mkdir(exist_ok=True);paths=[]
    cpu=[r for r in cases if r['module']=='cpu']
    if cpu:
        for page in range(0,len(cpu),4):
            subset=cpu[page:page+4];fig,axes=plt.subplots(1,len(subset),figsize=(4*len(subset),4),squeeze=False)
            for ax,row in zip(axes.flat,subset):
                mean=row['mean']/1e6;low=(row['mean']-row['mean_ci95_low'])/1e6 if row.get('mean_ci95_low') is not None else 0;high=(row['mean_ci95_high']-row['mean'])/1e6 if row.get('mean_ci95_high') is not None else 0
                ax.bar([0],[mean],yerr=[[low],[high]],capsize=5);ax.set_xticks([0],[row['case'].replace('_','\n')]);ax.set_ylabel('Million '+row['unit']);ax.set_title(f"{row['threads']} thread(s), n={row['count']}");ax.grid(axis='y',alpha=.2)
            fig.suptitle('CPU application kernels: mean and engine 95% mean CI');fig.tight_layout();path=directory/f'cpu_{page//4+1:03d}.png';save_figure(fig,path,validation,dpi=140);plt.close(fig);paths.append(path)
    memory=[r for r in cases if r['module']=='memory']
    for latency in (False,True):
        rows=[r for r in memory if (r['unit']=='ns/access')==latency]
        if not rows:continue
        fig,ax=plt.subplots(figsize=(9,5))
        for name in sorted({r['case'] for r in rows}):errorbar(ax,[r for r in rows if r['case']==name],'working_set_bytes_per_thread',1 if latency else 2**30,label=name)
        ax.set_xlabel('Working set per thread (bytes)');ax.set_ylabel('Dependent chain time (ns/access)' if latency else 'Application traffic (GiB/s)')
        ax.set_title('Pointer chase includes loop and scheduling' if latency else 'Memory application throughput; memcpy counts read + write')
        ax.legend(fontsize=8);fig.tight_layout();path=directory/('memory_pointer_latency.png' if latency else 'memory_bandwidth.png');save_figure(fig,path,validation,dpi=140);plt.close(fig);paths.append(path)
    network=[r for r in cases if r['module']=='network']
    if network:
        loopback=all(r.get('scope')=='loopback_selftest' for r in network)
        title='LOOPBACK ONLY — NOT PHYSICAL NETWORK PERFORMANCE' if loopback else 'Explicit peer application TCP — physical path not established'
        fig,axes=plt.subplots(1,2,figsize=(12,5))
        for ax,metric,label in zip(axes,('throughput','trial_mean_rtt'),('Verified send-to-ACK throughput (MiB/s)','Trial-mean round-trip latency (µs)')):
            rows=[r for r in network if r['case']==metric];errorbar(ax,rows,'payload_bytes');ax.set_xlabel('Request/payload size (bytes)');ax.set_ylabel(label)
        fig.suptitle(title,fontsize=11);fig.tight_layout();path=directory/'network_bandwidth_rtt.png';save_figure(fig,path,validation,dpi=140);plt.close(fig);paths.append(path)
        fig,ax=plt.subplots(figsize=(9,5))
        for size in sorted({r['payload_bytes'] for r in rtts}):
            rows=[r for r in rtts if r['payload_bytes']==size]
            ax.scatter(range(1,len(rows)+1),[r['rtt_us'] for r in rows],s=14,label=f'{size} bytes')
        ax.set_yscale('log');ax.set_xlabel('RTT observation index within payload size (repetitions concatenated)');ax.set_ylabel('Application round-trip latency (µs)');ax.set_title(title,fontsize=10);ax.legend();ax.grid(alpha=.2);fig.tight_layout();path=directory/'network_raw_rtt.png';save_figure(fig,path,validation,dpi=140);plt.close(fig);paths.append(path)
    return paths


def number(value):return 'undefined' if value is None else f'{value:,.3f}'
def table(rows,dimension,scale=1,unit=None):
    lines=['| Case | n | Mean | 95% mean CI | Min–max | CV |','|---|---:|---:|---|---|---:|']
    for row in rows:
        label=dimension(row)
        ci='undefined' if row.get('mean_ci95_low') is None else number(row['mean_ci95_low']/scale)+'–'+number(row['mean_ci95_high']/scale)
        lines.append(f"| {label} | {row['count']} | {number(row['mean']/scale)} {unit or row['unit']} | {ci} | {number(row['min']/scale)}–{number(row['max']/scale)} | {number(row.get('cv'))} |")
    return lines


def scope_notes(modules,config):
    notes=[]
    if 'memory' in modules:
        notes.append(f"Verified retained memory working sets within a {config.get('memory_bytes','unknown')}-byte total allocation budget; this does not verify all installed RAM.")
    if 'disk' in modules:
        disk=modules['disk']
        if disk.get('status')=='not_run':
            notes.append('Disk test was not run; scratch-file verification and device performance remain unassessed.')
        else:
            notes.append(f"Verified {disk.get('scratch_file_bytes','unknown')}-byte disk scratch file before and after testing; this does not verify the entire disk.")
    if 'inventory' in modules:
        health=modules['inventory'].get('storage_health',{}).get('status','unavailable')
        notes.append(f'Storage SMART/device health: {health}.')
    if 'network' in modules:
        if modules['network'].get('scope')=='loopback_selftest':
            notes.append('Network is loopback only: software path, copies, verification and scheduling; physical NIC/link performance remains untested.')
        else:
            notes.append('Network used an explicit peer; physical path/link capacity is not established by the application measurements.')
    return notes


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('input',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--source-commit',default='not supplied');parser.add_argument('--validation-only',action='store_true');args=parser.parse_args()
    raw=args.input.read_bytes();data=json.loads(raw)
    modules,reps,cases,rtts=extract(data);args.output.mkdir(parents=True,exist_ok=True)
    raw_copy=args.output/'raw_checkout.json'
    if raw_copy.resolve()!=args.input.resolve():raw_copy.write_bytes(raw)
    provenance={'source_commit':args.source_commit,'input_sha256':hashlib.sha256(raw).hexdigest(),'schema_version':data['schema_version'],'config':data.get('config'),'validation_only':args.validation_only,'analysis':'offline only; C summaries retained without recalculation'}
    (args.output/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
    for name,rows in [('per_repetition',reps),('per_case',cases),('raw_rtt',rtts)]:write_dataset(args.output,name,rows)
    paths=charts(args.output,cases,rtts,args.validation_only)
    inventory=modules.get('inventory',{});config=data.get('config',{});disk=modules.get('disk',{})
    report=['# '+('SYNTHETIC VALIDATION ONLY' if args.validation_only else 'Knurl system checkout'),'',f"Source commit: `{args.source_commit}`. Input SHA-256: `{provenance['input_sha256']}`.",'',f"Host inventory: {inventory.get('cpu_description','unavailable')}; {inventory.get('logical_cpus','unknown')} logical CPUs; {inventory.get('ram_total_bytes','unknown')} bytes OS-reported RAM. Tested threads: {config.get('threads','unknown')}; repetitions: {config.get('repetitions','unknown')}; requested timed window: {config.get('duration_seconds','unknown')} seconds per CPU/memory repetition.",'','This report uses only the supplied completed checkout; no earlier development runs are combined. Only modules present in this input are reported. This is a short application checkout, not hardware certification or a peak-performance claim.','',' '.join(scope_notes(modules,config)),'','[Raw checkout JSON](raw_checkout.json) · [Provenance](provenance.json) · [Per-repetition CSV](per_repetition.csv) · [Per-repetition JSON](per_repetition.json) · [Per-case CSV](per_case.csv) · [Per-case JSON](per_case.json) · [Every raw RTT CSV](raw_rtt.csv) · [Every raw RTT JSON](raw_rtt.json)','','All case statistics are preserved from the C engine, including exact mode/status, min/max, mean, median, sample SD, CV, P5/P95/P99 and 95% mean CI. Percentiles use the recorded nearest-rank method. A percentile across trial means is not a pooled operation percentile. With three repetitions, P95/P99 equal the largest trial observation; small-sample intervals are wide and do not cover systematic bias. Read the retained CI/method fields in per_case.json. Charts show mean and engine mean CI; raw RTTs remain separate rather than treating within-trial samples as independent trials.','']
    if 'cpu' in modules:report+=['## CPU','']
    if 'cpu' in modules:
        report+=table([r for r in cases if r['module']=='cpu'],lambda r:r['case'])
        report+=['','CPU figures describe a scalar hash dependency chain and an exact-fraction dot product, not general-purpose peak FLOP/s.','']
    if 'memory' in modules:
        report+=['## Memory','']
        report+=table([r for r in cases if r['module']=='memory' and r['unit']=='ns/access'],lambda r:f"{r['case']}, {r['working_set_bytes_per_thread']} bytes")
        report+=['','Memory curves show scalar application read/write and memcpy traffic. Memcpy counts read plus write bytes and can reflect caches; it is not physical memory-bus bandwidth. Pointer-chase time includes loop, periodic timing and scheduling overhead; it is not isolated DRAM latency.','']
    if 'network' in modules:
        report+=['## Network','']
        report+=table([r for r in cases if r['module']=='network'],lambda r:f"{r['payload_bytes']} bytes, {r['case']}")
        report+=['','Throughput times send-to-verified-ACK; RTT is full application ping-pong round trip, not one-way latency. The first large-payload RTT can include receiver expected-payload setup; all observations are retained.','']
    if 'disk' in modules:
        if disk['status']=='not_run':
            report+=['## Disk','','Disk status: not_run. The intentional default omission performs no disk workload or scratch-file verification; disk performance and device health are unassessed.','']
        else:
            report+=['## Disk','',f"Disk status: {disk['status']}; expected/completed trials: {disk.get('expected_and_completed_trials','unknown')}. Raw disk results are separate; no disk percentiles are recomputed by this script.",'']
        if disk['status']=='passed' and disk.get('raw_results_directory'):
            import os
            link=os.path.relpath(disk['raw_results_directory'],args.output)
            report += [f'[Raw disk results directory]({link})','']
    report+=['## Charts','']+[f'![{p.stem}](charts/{p.name})' for p in paths]
    (args.output/'report.md').write_text('\n'.join(report)+'\n');print(f'{len(reps)} repetition records, {len(cases)} case summaries, {len(rtts)} RTT samples; {len(paths)} charts → {args.output}')
if __name__=='__main__':main()
