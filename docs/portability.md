# Portability validation

Validation record: **2026-10-27**. These are functional smoke and integrity checks, not a completed performance campaign or published device comparison. Temporary smoke trials must remain separate from research results.

| Platform | Build | Validation observed |
| --- | --- | --- |
| Raspberry Pi, Linux aarch64 | Native GCC 15.3 | 20 direct-I/O trials completed: all five patterns, 4 KiB/1 MiB blocks, 1/8 workers, 16 MiB file. Full-file byte integrity verification passed. |
| Linux 6.12, x86_64 | Native GCC 15.3.0 (SUSE Linux) | The same 20 direct-I/O trials completed. Full-file byte integrity verification passed. |
| macOS, arm64 | AppleClang 17 | Make and CMake builds passed. All 13 CLI contract tests passed. The same 20-trial `F_NOCACHE` smoke completed; full-file byte integrity verification passed. |

The 13 contract tests use disposable buffered regular files. They cover default and generic setup labels, unsafe label rejection, help, device/symlink/unowned-directory rejection, configuration preflight, byte corruption detection, resume/configuration drift, SIGINT partial-result handling, and operation/byte/histogram consistency. These contracts establish software behavior; they do not measure sustained storage speed.

Linux `O_DIRECT` and macOS `F_NOCACHE` have different semantics. Direct I/O support depends on the target filesystem. Worker concurrency is synchronous thread count rather than native asynchronous queue depth. No full default campaign is claimed by this record. Windows, other operating systems, and 32-bit targets have not been validated.

## System checkout integration

The separately added `knurl-check` executable built cleanly with Make and CMake on macOS arm64/AppleClang 17 on the same date. The combined 25-test suite passed: 13 legacy disk contracts plus 12 checkout contracts covering inventory, CPU known answers and accounting, memory integrity/budgets/traffic, loopback verification and malformed protocol rejection, explicit disk opt-in/full integrity/log preservation, bounded arguments, and a known Student-t summary reference. The network robustness update also passed its three targeted contract checks. These short checks do not validate the new checkout modules on other operating systems or constitute performance results.
