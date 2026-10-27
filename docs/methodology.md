# Collection methodology

Knurl runs the same file-based workload on supported macOS/Linux hosts locally or over SSH. Setup labels identify configurations; they do not select hardware settings. Design the matrix for the research question, record deviations from defaults, and retain the full configuration.

1. Record repository revision, compiler/version, OS/kernel, CPU model, storage model/firmware/capacity, connection/adapter, filesystem/mount options, free space, and I/O mode. Keep private identifiers and credentials out of public artifacts.
2. Fix file sizes, block sizes, worker counts, patterns, repetitions, duration, and warmup before collection. The default matrix uses 256 MiB/4 GiB/16 GiB files; 4/16/64/1024 KiB blocks; 1/8/32 workers; five read/write patterns; ten repetitions; five measured seconds and one warmup second. Smaller custom matrices are valid when explicitly reported.
3. Prepare dedicated regular files on the filesystem being measured. Use the same seed and randomized order across compared setups. Record manifest and integrity verification. Keep raw trial data separate from diagnostics or synthetic validation output.
4. Control hardware, power, cooling, firmware, free-space level, filesystem, software, and workload where possible. Record temperature/throttling, CPU frequency policy, and storage health when available. For remote runs, compile on the target and record that host's metadata; SSH itself is outside the timed I/O path.
5. If changing CPU governors or other privileged settings externally, save and restore their original values, including on interruption. The benchmark does not make those changes.
6. Retain trial-level data, operation latency histograms, separate read/write counters, errors, and incomplete status. Do not discard failures to improve summaries.
7. Analyze repetitions as experimental units. Report throughput and latency distributions with uncertainty, aggregation rule, pairing/order strategy, and exclusions. An arithmetic mean of percentile values is not a pooled latency percentile.
8. Label incomplete runs partial. Resume only against a matching manifest. Declare a comparison complete only when both setups satisfy its stated matrix and validation requirements.

Throughput includes generator and measurement overhead, while operation latency covers transfer calls. `fsync` is outside the measured window, so write latency is not a durability measurement. Audit CPU utilization and the offset-based generator on the target platform; a generator-limited plateau cannot establish peak NVMe throughput.

Worker concurrency differs from asynchronous queue depth. Linux direct I/O differs from macOS uncached file I/O, and buffered runs can reflect page-cache behavior. Short samples do not establish sustained SSD speed or lifetime. Results apply to their recorded configuration.

## Raspberry Pi Gen 2/Gen 3 campaign

This optional campaign uses the default matrix and labels `gen2`/`gen3`. Confirm actual negotiated PCIe speed and width after each external configuration/reboot; do not infer them from requested settings. Gen 3 is experimental and not officially certified by Raspberry Pi. Record Pi model, power supply, SSD firmware, adapter, cooling, CPU governor, temperature, and throttling. If using the performance governor, restrict it to the test interval and restore the previous value. The default two-mode campaign has 3,600 measured trials; preparation and control collection add overhead to six hours of timed windows.
