# Knurl

Knurl measures storage I/O on dedicated regular files with a C11/POSIX-thread engine and an optional Python analysis pipeline. Build and run it locally on macOS or Linux, including a Raspberry Pi, or build and run the same source on a remote host over SSH. The engine does not require a particular disk, hostname, PCIe link, or privileged system configuration.

## Build and quick check

Install a C compiler and Make (Apple Command Line Tools on macOS; GCC or Clang and development headers on Linux). Python 3 is needed for tests and analysis, and CMake is an optional build alternative.

```sh
make
make test
# Alternative:
cmake -S . -B build
cmake --build build
```

CI builds on Linux and macOS. See the dated [portability validation record](docs/portability.md) for observed native smoke and integrity checks. The portable engine uses POSIX file/thread APIs; Windows is not supported. Linux direct I/O requires filesystem support for `O_DIRECT`; macOS uses `F_NOCACHE`. Other operating systems and 32-bit targets are not validated. The tests use tiny buffered files; they do not establish direct I/O support or SSD performance on a particular host.

## Local run

Choose a dedicated new scratch directory on the filesystem to measure and a separate output directory. The engine only uses owned regular files. Use separate work and output directories for independent concurrent runs. A small functional run is:

```sh
./knurl --work-dir /tmp/knurl-demo-work --output-dir /tmp/knurl-demo-output \
  --mode local --buffered --file-sizes 1048576 --block-sizes 4096 \
  --concurrency 1 --patterns seq-read,rand-mix70read30write \
  --repetitions 1 --duration 1 --warmup 0
```

`--mode` is an optional descriptive setup label stored in results (default `baseline`); it does not configure the hardware. Labels accept 1–64 ASCII letters/digits plus `_`, `.`, and `-`, starting with a letter or digit. For measurements, replace `/tmp` with scratch storage on the target filesystem and omit `--buffered` to request the platform's default cache bypass. `/tmp` can be memory-backed on Linux, and buffered runs can measure page-cache behavior. Use a fresh output directory when changing configuration.

The default matrix combines three file sizes (256 MiB, 4 GiB, 16 GiB), four block sizes (4 KiB, 16 KiB, 64 KiB, 1 MiB), three worker counts (1, 8, 32), five patterns (sequential read/write, random read/write, and 70% read/30% write), and ten repetitions. This is 1,800 trials per setup label. At five measured seconds plus one warmup second, the timed portions alone take three hours per setup. Defaults require 20.25 GiB for prepared files plus raw output and filesystem overhead. Override the matrix for the available storage and question being studied; defaults do not assume a particular device or link mode.

## Remote run over SSH

Compile on the target so the binary matches its OS and architecture. The following uses `paco` as an example configured SSH alias and generic paths; substitute the target scratch mount. SSH is transport only: all I/O and timing run on the remote host. `/mnt/test-volume` is a disk mounted on that remote host; this command does not test the local Mac disk or the SSH connection. The transfer excludes the locally built `knurl` binary, and `make` builds the target binary remotely.

```sh
rsync -a --exclude .git --exclude knurl --exclude build --exclude results \
  --exclude work --exclude private ./ paco:knurl/
ssh paco 'cd knurl && make && make test'
ssh paco 'cd knurl && ./knurl \
  --work-dir /mnt/test-volume/knurl-work --output-dir results/remote \
  --mode remote --file-sizes 268435456 --block-sizes 4096,65536 \
  --concurrency 1,8 --patterns seq-read,seq-write \
  --repetitions 3 --duration 5 --warmup 1'
rsync -a paco:knurl/results/remote/ results/remote/
```

Use a persistent remote terminal/session for long runs and retain remote output until transfer is verified. No SSH alias or credential is built into Knurl. The repository does not store private hardware/network credentials.

## Record the host and filesystem

Run these read-only commands on the machine executing Knurl, using the actual work directory or its existing parent mount. Save the output with the run metadata. They avoid serial numbers and credential files.

macOS:

```sh
sw_vers
uname -srmp
cc --version
sysctl machdep.cpu.brand_string hw.memsize
df -h /Volumes/test-volume
# Substitute the device shown by df, for example /dev/disk4s1:
diskutil info /dev/disk4s1 | awk '/Device Block Size:|Disk Size:|File System Personality:|Type \(Bundle\):|Protocol:|Solid State:/'
```

Linux (including Raspberry Pi):

```sh
cat /etc/os-release
uname -srmp
cc --version
lscpu
free -h
findmnt -T /mnt/test-volume -o TARGET,SOURCE,FSTYPE
lsblk -o NAME,TYPE,SIZE,MODEL,ROTA,TRAN
```

For remote collection, run the same appropriate commands through `ssh paco` on the target. Record filesystem mount settings relevant to performance (for example compression, encryption, sync, and discard) separately, excluding authentication options, private server addresses, and credentials. Hardware tools vary by platform; unavailable temperature or health fields should be marked unavailable rather than guessed. Keep SSH configuration and keys outside result artifacts.

## Interpretation and controls

Throughput includes write-buffer generation, random offset selection, thread scheduling, and measurement overhead during the timed window. Operation latency times the `pread`/`pwrite` transfer and excludes buffer generation. Write completion is a system-call completion; the final `fsync` is outside the measured window. CPU-limited results must not be described as peak NVMe bandwidth. Use the [generator diagnostic](tools/README.md) and target CPU utilization to investigate a plateau.

Each worker makes synchronous `pread`/`pwrite` calls. Concurrency is the number of worker threads, **not native asynchronous queue depth**.

Linux `O_DIRECT` requires aligned buffers/offsets and filesystem support. macOS `F_NOCACHE` disables ordinary file caching but has different semantics. Buffered runs are explicitly selected with `--buffered`. Record OS, filesystem, mount configuration, and I/O mode; do not attribute differences between unlike platforms solely to storage hardware.

Keep relevant hardware, power, firmware, filesystem, free space, cooling, software revision, and workload fixed for controlled comparisons. Record temperature, throttling, frequency policy, and storage health where the platform exposes them. Knurl does not change governors, PCIe links, or other privileged settings. If you change a setting externally for a test, save and restore its original value.

Use the same seed for the same randomized cell order across compared setups. Repetitions are experimental units; individual I/Os are not independent experimental replicates. Five-second samples describe short workload behavior, not sustained write speed after cache exhaustion, endurance, or SSD lifetime.

Preserve raw trials and manifests. Resume only with the matching configuration accepted by the engine. Mark interrupted or incomplete collections as partial. See [methodology](docs/methodology.md) and [tracking issue](https://github.com/plover-digital/Knurl/issues/1).

## Optional Raspberry Pi PCIe campaign

One application is a controlled Gen 2/Gen 3 comparison using labels `gen2` and `gen3`. Gen 3 operation is experimental and is not officially certified by Raspberry Pi. Configure/reboot the Pi externally, confirm actual negotiated PCIe speed/width, and record the adapter, power supply, cooling, firmware, temperature, and throttling. A two-mode default campaign has 3,600 trials and six hours of timed windows; preparation and other overhead can extend this toward eight hours or longer. [Campaign methodology](docs/methodology.md) describes controls; none are mandatory assumptions of the engine.

## Analysis

```sh
python3 -m pip install -r requirements-analysis.txt
python3 analysis/analyze.py results/remote/trials --output results/analysis
```

For arbitrary setup labels, add `--baseline mac-local --candidate paco-local` when analyzing a collection containing those labels. Gen 2/Gen 3 comparison is detected when both labels occur; `--check-gen-campaign` explicitly checks that optional campaign's fixed matrix. See [statistics](docs/statistics.md) for aggregation and comparison assumptions. Synthetic fixtures validate the pipeline; they are not hardware measurements. Use `--help` for authoritative options in this checkout.
