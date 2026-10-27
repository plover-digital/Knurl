# Portable system checkout

`knurl-check` is a separate, opt-in system checkout executable. The existing `knurl` disk engine and CLI remain independent. Build both with `make` or CMake. Run on the local machine:

```sh
./knurl-check inventory --target . > inventory.json
./knurl-check check --duration .25 --repetitions 3 > checkout.json
# Individual modules:
./knurl-check cpu --threads 2 --duration .1
./knurl-check memory --memory-bytes 16777216 --duration .1
./knurl-check network --repetitions 3
```

`check` runs inventory, CPU, memory, and a verified TCP loopback selftest. Disk is reported `not_run` unless both scratch/output paths are explicit. The network selftest does not use an external host or measure a physical NIC. No platform tuning, privileged settings, or background services are installed. These checks consume CPU, memory bandwidth, and loopback resources while running; choose an idle interval when comparing setups.

The default per-case duration is 0.25 seconds and three repetitions. Limits are duration 0.001–5 seconds, 1–30 repetitions, 1–64 threads, and a total memory allocation budget of at most 1 GiB. Memory needs two 4 KiB buffers per thread at minimum. Many cases run, so total elapsed time exceeds the per-case duration. CPU/memory outputs retain measured repetitions and summaries. Network uses explicitly bounded transfers rather than the CPU/memory duration setting.

## Disk opt-in

```sh
./knurl-check disk --target /Volumes/test-volume \
  --work-dir /Volumes/test-volume/knurl-check-work \
  --output-dir results/check-disk --disk-bytes 16777216 \
  --duration .1 --repetitions 3
```

Use scratch paths on the disk actually being tested. Disk invokes the existing `knurl` engine with a small fixed matrix, retains raw disk trials/logs, and checks full-file integrity before and after. It defaults to platform cache bypass. It requires filesystem direct-I/O support on Linux. Disk file size is bounded to 64 MiB and must be a 64 KiB multiple. This is a quick functional/short-workload check, not sustained SSD performance. The default engine path is the sibling `knurl` when `knurl-check` is invoked with a path, otherwise `./knurl`; `--disk-binary PATH` overrides it.

## Output and interpretation

The `inventory`/`cpu`/`memory`/`network`/`disk`/`check` commands use a JSON envelope with `schema_version`, `suite`, `command`, `complete`, `config`, and `modules`. Diagnostics use stderr. A nonzero exit and `complete:false` indicate a module error. `not_run` describes an intentionally omitted disk check; it does not claim that disk passed. See [checkout schema](check-schema.json), [CPU/memory details](cpu-memory.md), and [network details](network.md).

Summaries report count, unit, min/max, arithmetic mean, median, sample standard deviation (`n-1`), sample coefficient of variation (`sample_std / mean`, null for a zero mean or fewer than two samples), exact mode(s), and nearest-rank p5/p95/p99 across repetitions. `sample_std` is an alias of `sd`. An exact mode requires a repeated value; all tied most-frequent values are retained. Unique samples report `exact_mode:[]` and `exact_mode_status:"none/all unique"`. Mean 95% confidence intervals use the Student-t 0.975 critical value with `n-1` degrees of freedom through 31 samples; a normal 1.96 approximation is explicitly labeled for larger sample counts. A single sample has neither a sample standard deviation nor a confidence interval. The default three repetitions give a wide interval (critical value about 4.303), and normality/independence assumptions are not established by a short checkout. A p95 from only three values is the maximum; it is not a well-resolved tail estimate. Repetition summaries do not describe the distribution of individual CPU instructions or memory loads.

Inventory reads OS/kernel/architecture, safe CPU description, logical CPU count, RAM, and filesystem capacity/read-only status. It omits hostname, serial numbers, user identity, and network addresses. Linux available RAM uses `MemAvailable`; unavailable platform fields are null with a reason. Storage health requires platform/device-specific tooling and is reported unavailable rather than inferred from filesystem statistics. Inventory does not identify the underlying disk through an abstraction layer or run SMART tools. The target path is retained exactly as supplied; private path names are not anonymized. Explicit network server/client commands use their own module JSON records, as described in the network documentation.

Short selftests and synthetic arithmetic/pointer checks establish functional behavior and rough application-level baselines. They do not replace sustained load testing, a memory fault diagnostic, hardware certification, or physical-network measurements. Record power/frequency policy, thermals, OS, compiler, and working-set sizes when comparing outputs. See [portability validation](portability.md) for the separately recorded validation scope.
