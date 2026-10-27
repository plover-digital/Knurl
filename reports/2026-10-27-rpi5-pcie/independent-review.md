# Independent offline campaign review

Status: **PASS**. Reviewed 3,600 raw trials (1,800/mode), 180 cells/mode, ten distinct repetitions/cell, all committed checksums, counters/histograms, min/mean/max latency consistency, and matched seeded cell order. All workers are synchronous threads, not asynchronous queue depth.

Cross-checked 32 selected metric summaries against per_case.json (min/max/mean/median and Student-t mean confidence intervals). No discrepancies found.

## Selected 16 GiB cases

| Pattern | Block | Workers | Gen2 mean MiB/s | Gen3 mean MiB/s | BW ratio [95% paired bootstrap CI] | Gen2 / Gen3 mean latency µs |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| seq-read | 1048576 | 1 | 427.561 | 731.221 | 1.7102 [1.6727, 1.7400] | 2345.364 / 1377.298 |
| seq-write | 1048576 | 1 | 238.325 | 281.408 | 1.1808 [1.0199, 1.3848] | 4047.992 / 3361.469 |
| rand-read | 4096 | 32 | 307.594 | 310.830 | 1.0105 [1.0029, 1.0173] | 406.286 / 401.986 |
| rand-write | 4096 | 32 | 182.058 | 226.219 | 1.2426 [1.0525, 1.5156] | 764.844 / 564.583 |

The JSON retains BW, IOPS, mean-latency and trial-P99 ratios with intervals, individual-mode min/max/mean/median/sample SD and Student-t confidence intervals. Ratio CIs use 20,000 paired repetition-index bootstrap samples (seed 20261027). They are ratios of means, not mean ratios.

## Link and health

- gen2: 736 telemetry records; observed link [5.0] GT/s × [1]; throttling values [0]; CPU [45.5, 51.0] °C. AER counters unchanged and all zero; SMART error/unsafe-shutdown/thermal-transition/time increments all zero; kernel error scan empty.
- gen3: 735 telemetry records; observed link [8.0] GT/s × [1]; throttling values [0]; CPU [45.0, 52.7] °C. AER counters unchanged and all zero; SMART error/unsafe-shutdown/thermal-transition/time increments all zero; kernel error scan empty.

## Supervisor completion evidence

- gen2: supervisor logs full integrity verification complete, exit 0, and original/restored governor both `schedutil`.
- gen3: supervisor logs full integrity verification complete, exit 0, and original/restored governor both `schedutil`.

## Scientific limits

- Gen2 campaign preceded Gen3: link mode is confounded with chronological SSD state/aging/temperature and no randomized crossover campaign was performed.
- Confidence intervals quantify within-campaign repeated-trial variation; not uncertainty across independent boards, SSDs, boots, or randomized link-mode campaigns.
- Five-second windows and short file-based workloads do not establish long-term sustained write performance/endurance.
- Synchronous worker concurrency is not native asynchronous queue depth. Throughput includes generator overhead; latency measures transfer calls, not fsync durability.
- P99 values are histogram-bin upper bounds; comparison summarizes trial P99s rather than recomputing a pooled percentile.
- Bootstrap intervals are exploratory selected-case intervals without multiple-comparison adjustment.
- Bootstrap and Student-t intervals assume exchangeable/independent repetition pairs or trials; serial drift and shared SSD state are not modeled.
