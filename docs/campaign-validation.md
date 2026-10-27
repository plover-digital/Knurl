# Offline campaign acceptance

Before analyzing a collected run, validate its manifest and committed trial files:

```sh
python analysis/validate_campaign.py results/run --output results/validation.json
```

The validator uses only Python's standard library and starts no workload or connection. It supports the C engine's schema 1 / engine 1.0 contract with arbitrary experiment labels and manifest dimensions. It reproduces balanced repetition blocks and exact rejection-sampled SplitMix64/Fisher–Yates ordering, then checks every filename/ID/configuration/repetition, complete/verified/error flags, timing/rates/bytes, histogram counts and quantiles, optional exact extrema, case totals, and optional progress state. Missing, extra, duplicate or uncommitted artifacts fail. Unsupported versions/order/histogram semantics fail rather than being guessed.

Each `.ok` marker must match the exact raw JSON bytes, including whitespace: initial hash **1469598103934665603** (not the standard FNV offset), XOR each byte then multiply by **1099511628211**, modulo 2^64, formatted as 16 lowercase hex digits plus newline. This is an integrity/commit marker, not authentication or proof of source provenance.

The JSON report has `PASS` for a complete valid campaign or `FAIL` with issues and missing/extra ID lists. Exit status is zero for accepted output and one for failure. Explicit `--allow-partial` permits absent expected trials and emits `PASS_PARTIAL` with `complete: false`; every present JSON still needs a matching `.ok` and valid content. Missing markers, orphan markers and invalid records remain failures. Validate a stable offline snapshot: a live writer can temporarily have JSON without its marker or progress may lag.

These checks establish committed measurement consistency and the engine's per-trial sampled verification flags. They do **not** establish final full-file verification, SMART/AER health, measured PCIe settings, independence of repetitions, or authenticity of hardware results. Retain those separate supervisor, health and source-provenance artifacts. `complete: true` describes this one manifest, not an uncollected comparison label.

Software guard validation can use one supplied real trial as input to temporary, explicitly marked synthetic fixtures:

```sh
python analysis/validate_campaign_fixtures.py results/run
```

This checks corruption, seeded-order mismatch, missing markers, duplicate configurations, partial completeness, extra IDs and unsupported versions without retaining fixtures or treating them as benchmark evidence. It performs no hardware work.
