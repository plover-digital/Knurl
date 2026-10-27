#!/usr/bin/env python3
"""Validate committed Knurl v1 campaigns offline; never runs benchmarks."""
import argparse
from collections import Counter
import hashlib
import itertools
import json
import math
from pathlib import Path
import re

MASK=(1<<64)-1
PATTERNS=('seq-read','seq-write','rand-read','rand-write','rand-mix70read30write')
ORDER='balanced_repetition_blocks_fisher_yates_splitmix64_v1'


def engine_checksum(raw):
    """Exact engine algorithm; its initial value is NOT the standard FNV offset."""
    value=1469598103934665603
    for byte in raw:value=((value^byte)*1099511628211)&MASK
    return f'{value:016x}\n'.encode('ascii')


def mix(value):
    value^=value>>30;value=value*0xbf58476d1ce4e5b9&MASK
    value^=value>>27;value=value*0x94d049bb133111eb&MASK
    return value^(value>>31)


def expected_trials(manifest):
    state=manifest['seed'];result=[]
    for rep in range(1,manifest['repetitions']+1):
        cells=[tuple(values)+(rep,) for values in itertools.product(manifest['file_sizes'],manifest['block_sizes'],manifest['concurrency'],manifest['patterns'])]
        for size in range(len(cells),1,-1):
            threshold=((-size)&MASK)%size
            while True:
                state=(state+0x9e3779b97f4a7c15)&MASK;value=mix(state)
                if value>=threshold:break
            index=value%size;cells[size-1],cells[index]=cells[index],cells[size-1]
        result.extend(cells)
    return result


def integer(value,minimum=0,maximum=MASK):
    return isinstance(value,int) and not isinstance(value,bool) and minimum<=value<=maximum


def finite(value):
    return isinstance(value,(int,float)) and not isinstance(value,bool) and math.isfinite(value)


def bin_upper(index):
    return min(MASK,((1<<(index//16))*(17+index%16)+15)//16-1)


def bin_index(value):
    if not value:return 0
    exponent=value.bit_length()-1;base=1<<exponent
    return min(1023,16*exponent+((value-base)*16)//base)


def load_json(path):
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result:raise ValueError('duplicate JSON key: '+key)
            result[key]=value
        return result
    def invalid_constant(value):raise ValueError('nonfinite JSON constant: '+value)
    return json.loads(path.read_bytes(),object_pairs_hook=unique,parse_constant=invalid_constant)


def validate_manifest(m):
    if type(m.get('schema_version')) is not int or m.get('schema_version')!=1 or m.get('engine_version')!='1.0':raise ValueError('unsupported schema/engine version')
    if m.get('order')!=ORDER:raise ValueError('unsupported deterministic ordering algorithm')
    if m.get('concurrency_kind')!='synchronous_worker_threads':raise ValueError('unsupported concurrency semantics')
    if m.get('write_buffer_generation')!='seeded_offset_hash_each_write_outside_latency_timer_inside_measured_window':raise ValueError('unsupported write buffer generation semantics')
    if m.get('histogram')!={'unit':'ns','bins':1024,'sub_bins_per_octave':16,'percentiles':'bin_upper_bound'}:raise ValueError('unsupported histogram contract')
    if not isinstance(m.get('mode'),str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}',m['mode']):raise ValueError('invalid experiment label')
    if not integer(m.get('seed')) or not integer(m.get('repetitions'),1,100000):raise ValueError('invalid seed/repetitions')
    if not finite(m.get('duration_seconds')) or m['duration_seconds']<=0 or not finite(m.get('warmup_seconds')) or m['warmup_seconds']<0 or not isinstance(m.get('direct'),bool):raise ValueError('invalid timing/direct configuration')
    for field in ('file_sizes','block_sizes','concurrency','patterns'):
        values=m.get(field)
        if not isinstance(values,list) or not values or len(values)>64 or len(set(values))!=len(values):raise ValueError('invalid/duplicate dimension: '+field)
        if field=='patterns':
            if any(value not in PATTERNS for value in values):raise ValueError('unsupported pattern')
        elif any(not integer(value,1) for value in values):raise ValueError('invalid positive integer dimension: '+field)
    total=math.prod(len(m[key]) for key in ('file_sizes','block_sizes','concurrency','patterns'))*m['repetitions']
    if total>1000000 or not integer(m.get('total_trials'),1,1000000) or m.get('total_trials')!=total:raise ValueError('manifest total/matrix mismatch or oversized matrix')
    if any(size%block for size in m['file_sizes'] for block in m['block_sizes']):raise ValueError('file size not divisible by block size')
    return total


def validate(run,allow_partial=False):
    run=Path(run);issues=[];counts=Counter();seen_configs=set();seen_ids=set();checksums=0;histograms=0;valid_trials=0
    result={'validator_version':1,'status':'FAIL','allow_partial':allow_partial,'complete':False,
            'semantics':'Checks committed trial artifacts and sampled per-trial verification flags; does not establish full-file post-campaign verification or hardware health.',
            'checksum':{'initial':1469598103934665603,'prime':1099511628211,'arithmetic':'XOR byte then multiply modulo 2^64; exact raw bytes and newline marker'},'issues':issues}
    def check(condition,message):
        if not condition:issues.append(message)
    try:
        m=load_json(run/'manifest.json');total=validate_manifest(m);expected=expected_trials(m)
        result.update(manifest=m,manifest_sha256=hashlib.sha256((run/'manifest.json').read_bytes()).hexdigest(),expected_trials=total)
    except (OSError,ValueError,KeyError,TypeError) as error:
        issues.append('manifest: '+str(error));return result
    files=sorted((run/'trials').glob('*.json'));markers=sorted((run/'trials').glob('*.ok'))
    expected_names={f'{index:06d}' for index in range(total)}
    json_names={p.stem for p in files};marker_names={p.stem for p in markers}
    missing=sorted(expected_names-json_names);extra=sorted(json_names-expected_names)
    missing_markers=sorted(json_names-marker_names);orphan_markers=sorted(marker_names-json_names)
    check(not extra,'extra/unexpected JSON IDs: '+','.join(extra))
    check(not missing_markers,'missing OK markers: '+','.join(missing_markers))
    check(not orphan_markers,'orphan/extra OK markers: '+','.join(orphan_markers))
    if not allow_partial:check(not missing,'missing expected trial IDs: '+','.join(missing))
    result.update(missing_trial_ids=missing,extra_trial_ids=extra,missing_ok_ids=missing_markers,orphan_ok_ids=orphan_markers)
    for path in files:
        prior=len(issues)
        try:
            raw=path.read_bytes();r=load_json(path)
            marker=path.with_suffix('.ok')
            if marker.exists():
                match=marker.read_bytes()==engine_checksum(raw);check(match,path.name+': checksum mismatch');checksums+=int(match)
            trial_id=r['trial_id'];check(integer(trial_id,0,total-1),path.name+': invalid trial_id')
            check(trial_id not in seen_ids,path.name+': duplicate trial_id');seen_ids.add(trial_id)
            check(path.stem==f'{trial_id:06d}',path.name+': filename/trial_id mismatch')
            check(type(r.get('schema_version')) is int and r.get('schema_version')==1,path.name+': unsupported trial schema')
            check(r.get('complete') is True and r.get('verified') is True and integer(r.get('errors')) and r['errors']==0 and integer(r.get('first_errno')) and r['first_errno']==0,path.name+': incomplete/unverified/error trial')
            c=r['config'];key=tuple(c[k] for k in ('file_size','block_size','concurrency','pattern','repetition'))
            check(key not in seen_configs,path.name+': duplicate configuration/repetition');seen_configs.add(key)
            if integer(trial_id,0,total-1):check(key==expected[trial_id],path.name+': seeded order/configuration/repetition mismatch')
            check(all(integer(c[field],1) for field in ('file_size','block_size','concurrency','repetition')) and isinstance(c['direct'],bool),path.name+': invalid configuration types')
            counts[key[:4]]+=1
            check(all(c[k]==m[k] for k in ('mode','seed','duration_seconds','warmup_seconds','direct')),path.name+': config differs from manifest')
            check(r.get('verification')=='16_evenly_spaced_4096_byte_sectors_before_and_after',path.name+': unsupported verification semantics')
            check(r.get('concurrency_kind')=='synchronous_worker_threads' and r.get('measurement_rule')=='launch_before_deadline_include_terminal_completions',path.name+': unsupported measurement semantics')
            elapsed=r['measured_seconds'];start=r['monotonic_start_seconds'];finish=r['monotonic_finish_seconds']
            check(all(finite(v) for v in (elapsed,start,finish)) and elapsed>=m['duration_seconds'] and start>=0 and math.isclose(elapsed,finish-start,rel_tol=1e-9,abs_tol=1e-8),path.name+': timing inconsistency')
            check(r['requested_window_seconds']==m['duration_seconds'],path.name+': requested window differs')
            total_ops=0
            for op in ('read','write'):
                stats=r[op];lat=stats['latency_ns'];ops=stats['ops'];byte_count=stats['bytes'];total_ops+=ops
                check(integer(ops) and integer(byte_count) and byte_count==ops*c['block_size'],path.name+': '+op+' byte/operation inconsistency')
                hist=lat['histogram'];hist_ok=isinstance(hist,list) and len(hist)==1024 and all(integer(v) for v in hist) and sum(hist)==ops
                check(hist_ok,path.name+': '+op+' histogram inconsistency');histograms+=int(hist_ok)
                check(finite(stats['iops']) and finite(stats['bw_Bps']) and elapsed>0 and math.isclose(stats['iops'],ops/elapsed,rel_tol=1e-12,abs_tol=1e-12) and math.isclose(stats['bw_Bps'],byte_count/elapsed,rel_tol=1e-12,abs_tol=1e-12),path.name+': '+op+' rate inconsistency')
                check(integer(lat['sum']) and finite(lat['mean']) and math.isclose(lat['mean'],lat['sum']/ops if ops else 0,rel_tol=1e-12,abs_tol=1e-9),path.name+': '+op+' mean/sum inconsistency')
                if hist_ok:
                    for field,numerator,denominator in (('p50',1,2),('p95',95,100),('p99',99,100),('p999',999,1000)):
                        quantile=0
                        if ops:
                            rank=(ops*numerator+denominator-1)//denominator;cumulative=0
                            for index,amount in enumerate(hist):
                                cumulative+=amount
                                if cumulative>=rank:quantile=bin_upper(index);break
                        check(integer(lat[field]) and lat[field]==quantile,path.name+': '+op+' '+field+' disagrees with histogram')
                if 'min' in lat and 'max' in lat:
                    low=lat['min'];high=lat['max'];valid=integer(low) and integer(high) and low<=high and low*ops<=lat['sum']<=high*ops
                    if ops and hist_ok:valid=valid and hist[bin_index(low)]>0 and hist[bin_index(high)]>0
                    if not ops:valid=valid and low==high==lat['sum']==0
                    check(valid,path.name+': '+op+' extrema inconsistency')
            check(total_ops>0,path.name+': no completed operations')
            if c['pattern'] in ('seq-read','rand-read'):check(r['write']['ops']==0,path.name+': writes in read-only pattern')
            if c['pattern'] in ('seq-write','rand-write'):check(r['read']['ops']==0,path.name+': reads in write-only pattern')
        except (OSError,ValueError,KeyError,TypeError,IndexError,ZeroDivisionError,OverflowError) as error:
            issues.append(path.name+': unreadable/invalid trial: '+str(error))
        if len(issues)==prior and path.stem in marker_names:valid_trials+=1
    complete=not missing and len(files)==total
    if complete:check(len(counts)==total//m['repetitions'] and set(counts.values())=={m['repetitions']},'case matrix/repetition totals mismatch')
    if (run/'progress.json').exists():
        try:
            progress=load_json(run/'progress.json');result['progress']=progress
            check(progress.get('schema_version')==1 and progress.get('total')==total and progress.get('errors')==0,'progress schema/total/errors mismatch')
            if allow_partial:check(integer(progress.get('completed'),0,len(files)),'partial progress completed exceeds available trials')
            else:check(progress.get('status')=='complete' and progress.get('completed')==total,'progress not complete')
        except (OSError,ValueError,TypeError) as error:issues.append('progress: '+str(error))
    result.update(status='FAIL' if issues else ('PASS' if complete else 'PASS_PARTIAL'),complete=complete and not issues,
                  counts={'json':len(files),'ok':len(markers),'checksum_matches':checksums,'validated_trials':valid_trials,'case_configurations':len(counts),'repetitions_per_observed_case':sorted(set(counts.values())),'valid_directional_histograms':histograms},
                  checks={'all_committed_raw_checksums_match':checksums==len(files) and not missing_markers,'exact_seeded_order_reproduced':not any('order' in v or 'config' in v or 'trial_id' in v for v in issues)})
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--allow-partial',action='store_true');args=parser.parse_args()
    target=args.output.resolve();run=args.run.resolve()
    if target in (run/'manifest.json',run/'progress.json') or target.is_relative_to(run/'trials'):
        parser.error('--output must not overwrite campaign inputs')
    result=validate(args.run,args.allow_partial);args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(f"{result['status']}: {result.get('counts',{})}; issues={len(result['issues'])}")
    return 1 if result['status']=='FAIL' else 0
if __name__=='__main__':raise SystemExit(main())
