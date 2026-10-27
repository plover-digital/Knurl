# Portable TCP network checkout

`./knurl-check network` runs a verified TCP self-test against a child server bound only to `127.0.0.1` on an ephemeral port. It performs no discovery and contacts no remote host. Its JSON says `scope: loopback_selftest` and `physical_link_test: false`: these numbers describe the local TCP/software path, memory copies, scheduling and payload verification, **not physical NIC, Wi-Fi, cable or link capacity**. CPU load and other checkout modules can affect results. The default tests 1 KiB, 64 KiB and 1 MiB payloads, one MiB sent per throughput trial, 20 ping-pong samples per trial, and the configured number of repetitions (three by default).

A separate peer is explicit and opt-in. Start a single-session receiver in one terminal:

```sh
./knurl-check server --bind 127.0.0.1 --port 46039 --timeout 10
```

Then run a sender in another terminal:

```sh
./knurl-check client --host 127.0.0.1 --port 46039 \
  --sizes 1024,65536,1048576 --bytes 1048576 \
  --repetitions 3 --rtt-iterations 20 --timeout 10
```

For an actual two-host experiment, explicitly bind the receiver to its intended interface address and pass that address using the sender's required `--host`. Repeat with roles exchanged to measure the opposite direction. Both IPv4 and IPv6 are resolved using `getaddrinfo`; for an IPv6 loopback check use `--bind ::1` and `--host ::1`. No bracket syntax is needed because the port is separate. The server defaults to loopback; a wildcard or remote-interface bind is never selected implicitly. `--port 0` on the server requests an ephemeral port, printed to stderr. The protocol is an unauthenticated test service intended for an explicitly selected trusted peer; it does not establish that a selected endpoint traverses a physical interface. Even explicit-peer JSON therefore does not assert `physical_link_test: true`.

The receiver exits after one client session. The sender emits a single JSON object to stdout, suitable for redirection; the server emits a verified status object after completion. Transport, bounds or payload failures return nonzero and emit `verified: false`. Argument errors return status 2. No endpoint addresses are embedded in result JSON.

Each throughput trial measures **client-to-server application payload bytes divided by elapsed send-to-verified-ACK time**. The receiver verifies every payload byte before acknowledging. Payload generation and its checksum are computed before the sender's timer; receiver setup, generation of expected bytes, verification, protocol overhead, scheduling, TCP processing and the return ACK can contribute to elapsed time. This is useful end-to-end verified application throughput, not raw wire speed. ACK/header bytes are excluded from the numerator. Actual sent bytes round the requested byte budget up to a whole payload and are recorded. `TCP_NODELAY` is requested on both sides.

Latency samples are **application ping-pong round-trip times**, including sending one whole payload and receiving its verified echo. They are not one-way delays, and dividing them by two does not establish one-way latency. Every raw RTT is exported in `trials[].rtt_us`. JSON retains per-trial payload size, repetition, actual bytes, elapsed seconds and throughput; `summary_by_payload` summarizes repetitions separately for each size, including throughput and trial-mean RTT. It never combines differing payload sizes into one performance estimate. The framework summary records its confidence interval method; inspect raw observations and sample count before interpreting a small-sample interval.

The version-1 protocol uses eight network-byte-order 32-bit header words: magic `0x4b4e4e31`, version, kind (0=finish, 1=send/ACK, 2=echo), payload length, iteration count, nonce high/low, and FNV-1a-32 checksum. Expected bytes are generated from the nonce using a fixed unsigned 64-bit xorshift sequence; the receiver checks the checksum and every byte, and the sender checks echoed bytes. This detects accidental corruption and mismatch; the nonce/checksum are not cryptographic authentication. The final finish message receives an ACK so successful client completion includes receiver acceptance.

Bounds: 1–1,048,576 bytes per payload, at most 12 configured sizes, 1–30 repetitions, 1–1,000 client RTT samples per trial, at most 65,536 protocol messages and 64 MiB payload per trial, 1–60 seconds transport timeout (default five), and 120 seconds per accepted server session. Size/count combinations exceeding bounds fail. Each header/data exchange uses a monotonic absolute deadline; partial sends/receives, EINTR, EAGAIN, orderly disconnect and SIGPIPE are handled. Connect is nonblocking; the operating system's DNS resolution can take its own resolver timeout. These are deliberately short checkouts; use a dedicated longer-duration network benchmark for saturation studies.

Validation performed locally on macOS: default verified loopback self-test, explicit IPv4/IPv6 loopback sessions, required-target rejection, malformed-header rejection, partial-header disconnect, and a valid protocol transferred in three-byte fragments. These are software correctness checks, not recorded machine-performance claims. The C11/POSIX implementation targets macOS/Linux; no external network or Pi campaign was used during module development.
