# Accepted Gen2 versus Gen3 storage results

**Both campaigns complete and accepted: 3,600 verified trials, 360 label/case combinations, ten repetitions per case.** Same manifest dimensions, seed, 5 s measurement/1 s warmup, strict cache bypass and deterministic randomized repetition order; only experiment label differs in the manifest. Gen2 telemetry confirms 5.0 GT/s ×1; Gen3 boot and campaign telemetry confirm 8.0 GT/s ×1.

Benchmark source commit: `d259b583df9bf2788c6c0b0c7b60b3d8c1e4924c`. Full matched manifest/seed/order settings and checksum validation are retained with the accepted raw runs.

## Broad workload shapes

These are unweighted descriptive summaries over the predefined cases, not estimates of a universal speedup. Mixed workloads use combined read+write here; directional rows remain separately available.

| Pattern (36 cases each) | Median BW ratio | Range | Lower Gen3 means |
|---|---:|---:|---:|
| rand-mix70read30write | 1.073× | 0.820–1.555× | 10/36 |
| rand-read | 1.265× | 1.011–1.980× | 0/36 |
| rand-write | 1.136× | 0.975–1.325× | 3/36 |
| seq-read | 1.631× | 1.186–1.982× | 0/36 |
| seq-write | 1.160× | 1.026–1.493× | 0/36 |

Read cases improved most, approaching 2× in some saturated cases. Write and mixed results improve less consistently. Thirteen of 180 active/combined cases have lower Gen3 mean bandwidth: ten mixed and three random-write cases. This is retained rather than hidden. All thirteen lower-mean cases have overlapping individual 95% mean CIs; that is not proof of equivalence. Individual mean confidence intervals and CVs are in every matched comparison row; ratio CIs are not computed. CI overlap/nonoverlap is descriptive, not a multiple-comparison significance test.

## Representative 16 GiB cases

All rows n=10 per label. P95 is the mean of each trial’s histogram-upper-bound P95; it is not a percentile averaged into a pooled distribution. Actual operation-count-pooled tail shapes appear in the CCDF charts.

| 16 GiB pattern | Block | Workers | Operation | BW Gen2→Gen3 MiB/s | BW ratio | IOPS Gen2→Gen3 | Mean trial P95 Gen2→Gen3 µs |
|---|---:|---:|---|---:|---:|---:|---:|
| seq-read | 1024 KiB | 1 | read | 427.56→731.22 | 1.710× | 428→731 | 2,490.37→2,018.51 |
| seq-write | 1024 KiB | 1 | write | 238.33→281.41 | 1.181× | 238→281 | 10,262.94→4,168.09 |
| rand-read | 4 KiB | 1 | read | 39.84→41.14 | 1.033× | 10,199→10,533 | 111.41→105.68 |
| rand-read | 4 KiB | 32 | read | 307.59→310.83 | 1.011× | 78,744→79,573 | 760.22→776.60 |
| rand-write | 4 KiB | 1 | write | 121.04→133.54 | 1.103× | 30,986→34,187 | 21.50→15.51 |
| rand-write | 4 KiB | 32 | write | 182.06→226.22 | 1.243× | 46,607→57,912 | 3,060.53→3,604.48 |
| rand-mix70read30write | 4 KiB | 32 | combined | 214.37→194.97 | 0.909× | 54,879→49,912 | 1,333.66→1,625.29 |
| rand-mix70read30write | 16 KiB | 8 | combined | 258.09→221.06 | 0.857× | 16,518→14,148 | 1,261.57→1,540.09 |

The lowest mean bandwidth ratio is 0.820× (0.25 GiB, 4 KiB, 8 workers, rand-mix70read30write). Its Gen2/Gen3 bandwidth CVs are 25.52%/6.03%; mean CIs overlap=True. Several low mixed ratios coincide with highly variable Gen2 trials. This does not erase the lower observed Gen3 means, and it does not justify attributing the difference solely to link rate.

## Acceptance and health

Each mode passed 1,800 JSON/OK checksums, exact seeded ID/config/order matching, 180 cases × ten repetitions, and 3,600 directional histogram checks with zero trial errors. Both supervisors completed final full-byte file verification and exited zero; schedutil was restored. Gen3 preflight/boot guards confirm 8 GT/s ×1 and no throttling.

Across both campaigns: SMART critical warning/media errors/error-log entries remained zero, unsafe shutdowns remained seven, and no thermal-warning time or management-transition counters increased. Each mode captured 117 AER counters, all zero before/after (including root-port counters); Gen3 boot AER was also zero. Gen2 CPU telemetry 45.5–51.0°C (736 samples); Gen3 45.0–52.7°C (735 samples); all throttled=0x0 and measurement governor performance. SSD SMART temperature: Gen2 314→331 K; Gen3 318→330 K. Expected reboot increased power cycles 10→11 without unsafe-shutdown/error increments. Captured campaign kernel logs contain no storage/PCIe error lines.

## Interpretation limits

This is one before/after campaign pair on one device, not repeated independent randomized boot sessions; time, SSD state, temperature, background activity and order remain possible confounders. Do not claim causality solely from the negotiated link change. Workers are synchronous threads, not native NVMe queue depth. Bandwidth includes deterministic write-buffer-generation work; latency covers pread/pwrite completion including terminal operations, not durable-media/fsync latency. Mixed combined bandwidth sums both directions and can exceed a unidirectional bus payload ceiling; compare like patterns and directions. Verification/health evidence supports this observed run, not future hardware certification.

[Full statistical report/charts](report.md) · [Matched comparison CSV](comparison.csv) · [Matched comparison JSON](comparison.json) · [Per-case full statistics](per_case.json) · [Per-trial observations](per_trial.json) · [Pooled histograms](pooled_histograms.json) · [Health audit](health-comparison.json) · [Gen3 artifact validation](gen3-validation.json)

All analysis was performed offline on retrieved accepted copies; no measurements or hardware settings were changed by analysis.

[Independent raw-data/statistical cross-check](independent-review.md) · [Gen2 reusable artifact validation](gen2-validation.json). Exploratory bootstrap ratios in the independent technical review are conditional on within-campaign matched repetition assumptions; they do not add independent boot-session replication and are not used for the main point-ratio summary.

Bandwidth gains did not uniformly improve tail latency: the 16 GiB random-write case at 4 KiB/32 workers rose from 182.06 to 226.22 MiB/s while its mean trial P95 increased from 3,060.53 to 3,604.48 µs. Read the throughput and tail plots together.
