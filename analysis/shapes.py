"""Readable paginated shape plots, preserving exact case dimensions."""
from collections import defaultdict
from itertools import product
import math
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from plot_utils import save_figure

STYLES = ['-', '--', ':', '-.']

def chunks(values, size):
    for i in range(0, len(values), size):
        yield values[i:i+size]

def overview(charts, cases, validation=False):
    """Each panel fixes file size and pattern; no directions double-counted."""
    groups=defaultdict(list)
    for r in cases:
        if r['metric'] in ('bandwidth_mib_s','p95_latency_us'):
            key=tuple(r[k] for k in ('mode','file_bytes','block_bytes','concurrency','pattern','metric'))
            groups[key].append(r)
    chosen=[]
    for rows in groups.values():
        combined=[r for r in rows if r['operation']=='combined']
        chosen.extend(combined or rows)
    sizes=sorted({r['file_bytes'] for r in chosen}); patterns=sorted({r['pattern'] for r in chosen})
    modes=sorted({r['mode'] for r in chosen});workers=sorted({r['concurrency'] for r in chosen})
    colors={mode:plt.get_cmap('tab10')(i%10) for i,mode in enumerate(modes)}
    paths=[]
    for metric,label in [('bandwidth_mib_s','Mean bandwidth (MiB/s)'),('p95_latency_us','Mean of trial P95 latency (µs)')]:
        page=0
        for fs,ps,ms,ws in product(chunks(sizes,2),chunks(patterns,3),chunks(modes,4),chunks(workers,3)):
            page+=1
            fig,axes=plt.subplots(len(fs),len(ps),figsize=(5*len(ps),3.8*len(fs)+1),squeeze=False)
            for i,size in enumerate(fs):
                for j,pattern in enumerate(ps):
                    ax=axes[i,j]
                    rows=[r for r in chosen if r['metric']==metric and r['file_bytes']==size and r['pattern']==pattern]
                    for mode,worker in product(ms,ws):
                        byop=defaultdict(list)
                        for r in rows:
                            if r['mode']==mode and r['concurrency']==worker:byop[r['operation']].append(r)
                        for operation,rs in byop.items():
                            rs.sort(key=lambda r:r['block_bytes'])
                            ax.plot([r['block_bytes'] for r in rs],[r['mean'] for r in rs],color=colors[mode],linestyle=STYLES[ws.index(worker)],marker='o',ms=3)
                    ax.set_xscale('log',base=2);ax.set_xlabel('Block size (bytes)');ax.set_ylabel(label)
                    if metric=='p95_latency_us':ax.set_yscale('log')
                    ax.set_title(f'{size/2**30:g} GiB · {pattern}',fontsize=10);ax.grid(alpha=.2)
            handles=[Line2D([0],[0],color=colors[m],label=m) for m in ms]+[Line2D([0],[0],color='black',linestyle=STYLES[k],label=f'{w} workers') for k,w in enumerate(ws)]
            fig.legend(handles=handles,loc='lower center',ncol=min(len(handles),7),fontsize=8)
            fig.suptitle('Combined operations when mixed; otherwise active direction. Each line fixes label and workers.',fontsize=10)
            fig.tight_layout(rect=(0,.09,1,.94))
            path=charts/f'overview_{metric}_{page:03d}.png';save_figure(fig,path,validation,dpi=140);plt.close(fig);paths.append(path)
    return paths

def pooled_histograms(records):
    """Pool operation counts, not percentiles; preserve every case dimension."""
    pooled={}
    for r in records:
        c=r['config'];key=(c['mode'],c['file_size'],c['block_size'],c['concurrency'],c['pattern'])
        if key not in pooled:pooled[key]={'histogram':[0]*1024,'trials':0,'operations':set()}
        row=pooled[key];row['trials']+=1
        for operation in ('read','write'):
            m=r[operation]
            if not m['ops']:continue
            row['operations'].add(operation)
            for i,n in enumerate(m['latency_ns']['histogram']):row['histogram'][i]+=n
    return [{**dict(zip(('mode','file_bytes','block_bytes','concurrency','pattern'),key)),
             'trials':r['trials'],'operations':'+'.join(sorted(r['operations'])),
             'count':sum(r['histogram']),'histogram':r['histogram']} for key,r in sorted(pooled.items())]

def ccdf(charts, pooled, validation=False):
    facets=defaultdict(list)
    for r in pooled:facets[tuple(r[k] for k in ('file_bytes','pattern','block_bytes','concurrency'))].append(r)
    modes=sorted({r['mode'] for r in pooled});colors={m:plt.get_cmap('tab10')(i%10) for i,m in enumerate(modes)}
    paths=[];page=0
    for modepage in chunks(modes,6):
        for keys in chunks(sorted(facets),9):
            page+=1;rows=math.ceil(len(keys)/3)
            fig,axes=plt.subplots(rows,3,figsize=(15,3.4*rows+1),squeeze=False)
            for ax,key in zip(axes.flat,keys):
                for r in facets[key]:
                    if r['mode'] not in modepage:continue
                    counts=np.array(r['histogram'],dtype=float);indices=np.flatnonzero(counts)
                    # At each occupied bin lower boundary, reverse cumulative count
                    # is the exact number at or above that boundary. No raw samples inferred.
                    x=[max(.0005,(0 if i==0 else math.ceil(2**(int(i)//16)*(1+(int(i)%16)/16)))/1000) for i in indices]
                    survival=np.cumsum(counts[::-1])[::-1][indices]/counts.sum()
                    ax.step(x,survival,where='post',color=colors[r['mode']],label=f"{r['mode']} (n={r['count']:,})",marker='.',ms=3)
                size,pattern,block,worker=key
                ax.set_title(f'{size/2**30:g} GiB · {pattern}\nblock {block} bytes · {worker} workers',fontsize=9)
                ax.set_xscale('log');ax.set_yscale('log');ax.set_xlabel('Latency bin lower boundary (µs)');ax.set_ylabel('Fraction at or above boundary');ax.grid(alpha=.2);ax.legend(fontsize=7)
            for ax in list(axes.flat)[len(keys):]:ax.set_visible(False)
            fig.suptitle('Pooled histogram tail: all repetitions and active read/write operations; operation-weighted, quantized',fontsize=11)
            fig.tight_layout(rect=(0,0,1,.95));path=charts/f'pooled_ccdf_{page:03d}.png';save_figure(fig,path,validation,dpi=140);plt.close(fig);paths.append(path)
    return paths
