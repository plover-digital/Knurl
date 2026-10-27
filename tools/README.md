# Generator diagnostic

```sh
cc -O2 -std=c11 -pthread tools/generator-benchmark.c -lm -o /tmp/knurl-generator
/tmp/knurl-generator > /tmp/knurl-generator.csv
```

Run this on the target Pi using the same compiler flags and CPU frequency policy as the storage collection. The default generates 1 GiB per case; an optional numeric argument changes bytes per case (a positive multiple of 1 MiB). Repeat at least three times, recording CPU model, frequency, temperature, compiler, and revision.

The helper compares the current engine, the original byte-loop generator, and a byte-equivalent little-endian memcpy generator. It checks multiple lengths (including partial words), absolute offsets with a fixed seed before timing. Pure-generation rates show a single-worker CPU ceiling. Buffered cyclic `pwrite` and prefilled controls use an unlinked regular file in `/tmp`, with at most 64 MiB file space. These controls include page-cache and syscall effects; they are **not SSD or durable-write measurements**. An empty prefilled generation loop is intentionally omitted.

CPU time close to wall time suggests a busy core, but these diagnostics alone cannot prove that a particular storage trial is CPU-limited. Compare them with target storage trial throughput and CPU utilization. Keep diagnostic output separate from experimental raw trials.
