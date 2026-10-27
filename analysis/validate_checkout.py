#!/usr/bin/env python3
"""Offline validation using supplied real checkout data; starts no workloads.

Small negative fixtures are derived from that input and explicitly marked when
synthetic. They are never exported as machine performance results.
"""
import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from analyze_checkout import extract, scope_notes


def rejected(data):
    try:extract(data)
    except (ValueError,KeyError,TypeError):return
    raise AssertionError('Invalid checkout was accepted')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('actual',type=Path);p.add_argument('--analysis-dir',type=Path);p.add_argument('--expected-counts',help='Expected repetition,case,RTT counts, comma separated');a=p.parse_args()
    data=json.loads(a.actual.read_text());modules,reps,cases,rtts=extract(data)
    if a.expected_counts:assert (len(reps),len(cases),len(rtts))==tuple(map(int,a.expected_counts.split(',')))
    # Keep genuine measured numbers for the valid CPU-only path.
    cpu=copy.deepcopy(modules['cpu']);cpu['cases']=cpu['cases'][:1]
    small={**copy.deepcopy(data),'modules':[cpu]}
    synthetic=copy.deepcopy(small);synthetic['synthetic']=True;rejected(synthetic)
    nested=copy.deepcopy(small);nested['modules'][0]['synthetic']=True;rejected(nested)
    incomplete=copy.deepcopy(small);incomplete['complete']=False;rejected(incomplete)
    unverified=copy.deepcopy(small);unverified['modules'][0]['cases'][0]['repetitions'][0]['verified']=False;rejected(unverified)
    duplicate=copy.deepcopy(small);duplicate['modules'].append(copy.deepcopy(cpu));rejected(duplicate)
    for value in (float('nan'),float('inf'),'NaN'):
        bad=copy.deepcopy(small);bad['modules'][0]['cases'][0]['summary']['mean']=value;rejected(bad)
        bad=copy.deepcopy(small);bad['modules'][0]['cases'][0]['repetitions'][0]['rate']=value;rejected(bad)
    assert not scope_notes({'cpu':cpu},data.get('config',{}))
    with tempfile.TemporaryDirectory(prefix='knurl-offline-checkout-validation-') as tmp:
        root=Path(tmp);input_path=root/'real_cpu_subset.json';input_path.write_text(json.dumps(small))
        subprocess.run([sys.executable,str(Path(__file__).with_name('analyze_checkout.py')),str(input_path),'--output',str(root/'analysis')],check=True,capture_output=True,text=True)
        report=(root/'analysis'/'report.md').read_text()
        assert 'Verified retained memory' not in report and 'disk scratch file' not in report
        assert 'physical NIC/link' not in report and '## Memory' not in report and '## Disk' not in report and '## Network' not in report
        assert '## CPU' in report
        # Synthetic fixtures remain rejected even if the presentation flag is set.
        input_path.write_text(json.dumps(synthetic))
        result=subprocess.run([sys.executable,str(Path(__file__).with_name('analyze_checkout.py')),str(input_path),'--output',str(root/'synthetic-rejected'),'--validation-only'],capture_output=True,text=True)
        assert result.returncode!=0 and not (root/'synthetic-rejected').exists()
    if a.analysis_dir:
        assert (a.analysis_dir/'raw_checkout.json').read_bytes()==a.actual.read_bytes()
        assert json.loads((a.analysis_dir/'per_case.json').read_text())==cases
        assert json.loads((a.analysis_dir/'per_repetition.json').read_text())==reps
        assert json.loads((a.analysis_dir/'raw_rtt.json').read_text())==rtts
    print(f'Offline validation passed: {len(reps)} repetitions, {len(cases)} exact case summaries, {len(rtts)} RTTs; invalid/synthetic/duplicate/nonfinite rejection and CPU-only report scope verified')
if __name__=='__main__':main()
