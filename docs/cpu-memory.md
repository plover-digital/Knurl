# CPU and memory checkout

These modules run locally on POSIX macOS and Linux with C11, pthreads, libc,
and libm. They need no instruction-set extension, privileged device access,
network access, affinity changes, or CPU governor changes. They do not invoke
the disk engine or run anything on another machine.

The basic defaults are a 0.25-second measured window, three repetitions, one
worker, and a 16 MiB total memory allocation budget. CPU normally takes a few
seconds including full reference verification; memory's sixteen default cases
have twelve seconds of requested measured work plus setup and verification.
Other work on the machine, scheduling, power management, and thermal state can
change results. Run longer or repeat separately when the question requires it.
A short checkout is a functional and performance baseline, not a stability
certification or a hardware peak-performance claim.

## CPU

`integer_hash_chain` applies a dependent xorshift/multiply recurrence:

```
x ^= x >> 12;
x ^= x << 25;
x ^= x >> 27;
x *= 2685821657736338717;  // unsigned 64-bit arithmetic
```

Each recurrence counts as one hash. This is a scalar integer-workload rate in
hashes/s, not a cryptographic-hash or CPU instruction-throughput claim. A known
answer checks seed 1 after 64 recurrences: `43fc3d1de5ef95df`. Its initial value
is read through volatile storage to prevent compile-time elimination.

`exact_fraction_dot_product` uses 256 pairs of seeded 16-bit integers divided
by 65536 to form doubles. Each dot product performs 256 multiplications and
256 accumulations, counted as 512 algorithmic floating-point operations. Its
output selects the next rotation of one input vector and updates a retained
checksum, so successive work depends on the preceding result. The rate includes
that indexing, checksum, and control overhead. It is not a SIMD, matrix,
accelerator, or hardware FLOPS-peak measurement.

All products and partial sums are binary fractions with at most 40 significant
bits. They are exactly representable in a C double with the usual IEEE binary64
format on supported macOS/Linux targets. A separate unsigned-integer dot-product
reference computes the exact numerator and checks the complete timed sequence,
final rotation, and checksum. This avoids using a tolerance that could conceal a
wrong result or relying on matching floating-point contraction choices. Neither
fast-math nor reduced-precision arithmetic is required.

Each timed integer recurrence is also replayed outside the measured window and
checked against the retained final checksum. Worker inputs are deterministic
from the requested seed and worker index. Duration-based iteration counts and
checksums naturally vary with machine speed; their verification remains exact
for the actual recorded work count.

## Memory

`memory_bytes` is a **total allocation budget across workers**, including two
buffers per worker. It is not the size of each buffer. The minimum is 8 KiB per
worker and the module rejects budgets over 1 GiB. Allocation and size arithmetic
are bounded before allocation; any allocation failure produces a failed JSON
module. No deliberate memory exhaustion or page locking is performed.

Working sets per worker are the distinct supported sizes among 4 KiB, 64 KiB,
1 MiB, and the largest 64-byte-aligned size fitting the budget divided by twice
the worker count. Sizes larger than that limit are skipped. For the default
16 MiB budget and one worker, the largest working set is 8 MiB; two 8 MiB buffers
fit within the total budget. This covers several working-set scales without
assuming particular cache sizes. Setup prefaults and initializes the buffers;
the repeated windows predominantly describe steady-state access.

Each size runs these cases:

- `scalar_sequential_read`: volatile 64-bit loads and an unsigned sum. Read
  bytes count once. The accumulated sum is checked against the initialized
  contents multiplied by the actual number of complete sweeps.
- `scalar_sequential_write`: volatile 64-bit stores of a simple seeded,
  sweep-dependent pattern. Written bytes count once. Every word in the last
  retained sweep is checked after timing.
- `memcpy`: the platform libc implementation copies one buffer to another.
  A changing source word and a volatile function pointer prevent elimination
  of repeated copies. Both retained buffers are checked word by word. Reported
  application traffic counts **read plus write**, twice the copied payload.
  It does not estimate physical memory-bus traffic, write allocation, or cache
  writeback. The copy need not reach DRAM when its working set fits in caches.
- `random_pointer_chase`: a seeded Fisher–Yates permutation links one cycle
  through 64-byte nodes. This node size is a declared stride, not a discovered
  hardware cache-line size. Each volatile dependent load selects the next node;
  there are no independent chains within a worker. The final position is
  checked against the exact access count modulo cycle length, and every link
  is checked for retained integrity.

Sequential read/write are intentionally scalar C baselines. They are not
STREAM-equivalent or maximum vectorized memory bandwidth. Pointer-chase
`ns/access` includes the dependent load, loop control, periodic clock checks,
and scheduling. It is not raw DRAM latency. With multiple workers, the value is
the sum of their measured chain times divided by their combined accesses;
each worker owns its own chain. It does not turn parallel access throughput
into a claim about one hardware load's latency.

## Measurement and results

Workers prepare their data before a common start gate. They launch batches or
complete sweeps until a common monotonic deadline, and count terminal batches
that complete after that deadline. Throughput divides aggregate work by the
actual common-start-to-last-completion interval. Pointer latency instead uses
pooled worker chain time per access. Scheduler delays and terminal work remain
in these intervals. Data initialization, allocation, and verification are outside
the measured window. A second gate ensures that verification does not compete
with another worker's timed work.

Both modules always emit one JSON object. Each has `module`, `cases`, and
`complete`; configuration/allocation failures instead include an error object
and return nonzero. Every measured repetition records elapsed `seconds`, a
retained hexadecimal `checksum`, `verified`, and `error_code`. CPU additionally
records algorithmic `operations` and `rate`; memory records `iterations`,
`accesses`, `traffic_bytes`, and `value`. Read/write/copy iterations are complete
buffer sweeps summed across workers; pointer-chase iterations are accesses.

Case `summary` objects use the framework's repetition statistics, units, and
confidence-interval method. These summarize the repetitions actually run; three
short repetitions do not establish long-term stability or eliminate system
noise. Empty/failed cases do not contribute zero values as successful samples.
