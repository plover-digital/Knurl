# Raspberry Pi 5 PCIe Gen2/Gen3 research results — 2026-10-27

Both campaigns were accepted: 1,800 committed trials per label, 180 cases per label, ten repetitions per case. The Linux NVMe link was recorded at 5.0 GT/s ×1 and 8.0 GT/s ×1. This is one chronological before/after pair on one device; it does not isolate link-rate causality or establish universal/peak performance or hardware certification.

Start with [the results and limits](final-summary.md), [all charts](report.md), [sanitized health evidence](health-comparison.json), and [the independent technical cross-check](independent-review.md). Lower Gen3 means are retained for ten mixed and three random-write cases; throughput gains do not uniformly improve tail latency. Workers are synchronous threads, not native NVMe queue depth. Syscall completion latency is not durable-media/fsync latency. Mixed combined bandwidth adds read and write traffic and may exceed a unidirectional payload ceiling.

## Published data

- [Matched comparison CSV](comparison.csv) / [JSON](comparison.json): 252 operation views, with bandwidth, IOPS and trial-P95 means, full per-label summary fields, and point ratios. Main ratio CIs are not computed; exploratory selected-case bootstrap intervals in the independent review retain their assumptions and limits.
- [Per-case CSV](per_case.csv) / [JSON](per_case.json): all 4,536 metric summaries, including n, extrema, means, medians, exact/binned modes, sample SD, CV, P5/P95 and Student-t mean CIs.
- [Per-trial CSV](per_trial.csv) / [JSON](per_trial.json): 5,040 directional/combined operation rows. Only the `source` metadata was made portable; metrics are unchanged. A source such as `raw/gen2-run.tar.gz::trials/000000.json` names an archive and its internal member.
- [Pooled histogram CSV](pooled_histograms.csv) / [JSON](pooled_histograms.json): actual operation counts, pooled across repetitions/directions per case, used for quantized CCDF tails. Percentiles are never averaged to form a pooled percentile.
- [Gen2 archive](raw/gen2-run.tar.gz) / [Gen3 archive](raw/gen3-run.tar.gz): each contains `manifest.json`, `progress.json`, 1,800 original trial JSON files and their 1,800 original `.ok` markers. Raw bytes remain exact. Tar/gzip timestamps and ownership metadata are normalized; no raw health/boot/config files are included.
- [Gen2 validation](gen2-validation.json) / [Gen3 validation](gen3-validation.json), [provenance](provenance.json), [representative 16 GiB cases](representative-16GiB.json), and [descriptive workload-shape summaries](shape-summary.json).

## Reproduce the offline validation and analysis

Run from the repository root with Python and the optional analysis requirements installed. No benchmark, peer connection or hardware setting change is performed by these commands.

```sh
cd reports/2026-10-27-rpi5-pcie
shasum -a 256 -c SHA256SUMS
mkdir -p /tmp/knurl-reproduce/gen2/run /tmp/knurl-reproduce/gen3/run
tar -xzf raw/gen2-run.tar.gz -C /tmp/knurl-reproduce/gen2/run
tar -xzf raw/gen3-run.tar.gz -C /tmp/knurl-reproduce/gen3/run
cd ../..
python analysis/validate_campaign.py /tmp/knurl-reproduce/gen2/run --output /tmp/knurl-reproduce/gen2-validation.json
python analysis/validate_campaign.py /tmp/knurl-reproduce/gen3/run --output /tmp/knurl-reproduce/gen3-validation.json
python -m pip install -r requirements-analysis.txt
python analysis/analyze.py /tmp/knurl-reproduce/gen2/run/trials /tmp/knurl-reproduce/gen3/run/trials --output /tmp/knurl-reproduce/analysis --baseline gen2 --candidate gen3 --check-gen-campaign
python reports/2026-10-27-rpi5-pcie/reproduce_comparison.py /tmp/knurl-reproduce/analysis/per_case.json --output /tmp/knurl-reproduce/analysis
```

`sha256sum -c SHA256SUMS` is the Linux alternative. The engine's `.ok` checksum uses initial value **1469598103934665603**, not the standard FNV offset; the validator implements its exact byte algorithm. These markers detect integrity/commit mismatches, not malicious tampering or source authenticity. Generated data-source paths reflect the extraction directory; published source references use the archive notation above. PNG bytes may differ with plotting-library/font versions. See [statistics methodology](../../docs/statistics.md) and [campaign validation](../../docs/campaign-validation.md).

Reported P95 case values summarize trial histogram-upper-bound P95s. Pooled tail charts use operation-count histograms instead. Exact I/O minima/maxima and across-trial extrema are different quantities. Student-t intervals describe within-campaign repeated-trial variation and do not cover systematic bias, serial SSD state, chronological confounding, or variation across independent mode sessions. Retain negative results and read throughput and tail shapes together.
