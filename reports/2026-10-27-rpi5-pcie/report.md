# Knurl benchmark analysis

Analyzed 3600 trials across 360 cases. Experiment labels: gen2, gen3. Repetitions per case: 10–10. Full gen2/gen3 campaign check: PASS.

Each operation is analyzed separately. Latency percentile summaries describe trial percentile variation; they are not a combined operation percentile. Histogram-derived percentiles use bin upper bounds.

Mixed combined bandwidth sums read and write traffic. On a full-duplex link, this bidirectional total can exceed a unidirectional payload ceiling. Compare like patterns and operation directions; this sum does not establish a higher link rate. Separate directional statistics remain available.

[Per-trial CSV](per_trial.csv) · [Per-trial JSON](per_trial.json) · [Per-case CSV](per_case.csv) · [Per-case JSON](per_case.json) · [Pooled histogram CSV](pooled_histograms.csv) · [Pooled histogram JSON](pooled_histograms.json)

Sample standard deviation and Student-t 95% mean confidence intervals use independent trial values. Inspect variation and campaign completeness before drawing conclusions.

## Observations

Highest observed case mean for gen2: 450.50 MiB/s (seq-read, read, file 17179869184 bytes, block 1048576 bytes, concurrency 8, n=10). This identifies a measured case, not a universal hardware ceiling.

Highest observed case mean for gen3: 889.19 MiB/s (rand-read, read, file 268435456 bytes, block 65536 bytes, concurrency 32, n=10). This identifies a measured case, not a universal hardware ceiling.

The overview uses combined operations for mixed cases and the active direction otherwise. P95 overview curves summarize trial P95 values; the separate pooled CCDF weights each operation equally across repetitions and uses histogram boundaries, not averaged percentiles.

![bandwidth_latency](charts/bandwidth_latency.png)
![block_bytes_sweep](charts/block_bytes_sweep.png)
![concurrency_sweep](charts/concurrency_sweep.png)
![file_bytes_sweep](charts/file_bytes_sweep.png)
![variation_ci](charts/variation_ci.png)
![comparison_gain](charts/comparison_gain.png)
![overview_bandwidth_mib_s_001](charts/overview_bandwidth_mib_s_001.png)
![overview_bandwidth_mib_s_002](charts/overview_bandwidth_mib_s_002.png)
![overview_bandwidth_mib_s_003](charts/overview_bandwidth_mib_s_003.png)
![overview_bandwidth_mib_s_004](charts/overview_bandwidth_mib_s_004.png)
![overview_p95_latency_us_001](charts/overview_p95_latency_us_001.png)
![overview_p95_latency_us_002](charts/overview_p95_latency_us_002.png)
![overview_p95_latency_us_003](charts/overview_p95_latency_us_003.png)
![overview_p95_latency_us_004](charts/overview_p95_latency_us_004.png)
![pooled_ccdf_001](charts/pooled_ccdf_001.png)
![pooled_ccdf_002](charts/pooled_ccdf_002.png)
![pooled_ccdf_003](charts/pooled_ccdf_003.png)
![pooled_ccdf_004](charts/pooled_ccdf_004.png)
![pooled_ccdf_005](charts/pooled_ccdf_005.png)
![pooled_ccdf_006](charts/pooled_ccdf_006.png)
![pooled_ccdf_007](charts/pooled_ccdf_007.png)
![pooled_ccdf_008](charts/pooled_ccdf_008.png)
![pooled_ccdf_009](charts/pooled_ccdf_009.png)
![pooled_ccdf_010](charts/pooled_ccdf_010.png)
![pooled_ccdf_011](charts/pooled_ccdf_011.png)
![pooled_ccdf_012](charts/pooled_ccdf_012.png)
![pooled_ccdf_013](charts/pooled_ccdf_013.png)
![pooled_ccdf_014](charts/pooled_ccdf_014.png)
![pooled_ccdf_015](charts/pooled_ccdf_015.png)
![pooled_ccdf_016](charts/pooled_ccdf_016.png)
![pooled_ccdf_017](charts/pooled_ccdf_017.png)
![pooled_ccdf_018](charts/pooled_ccdf_018.png)
![pooled_ccdf_019](charts/pooled_ccdf_019.png)
![pooled_ccdf_020](charts/pooled_ccdf_020.png)
