# SMP / memory / APIC matrix

Status: **offline-validated** boot timing only (2026-11-07). This is not a
Qt/llvmpipe rendering benchmark; use it to choose safe harness defaults, not
to claim UI performance.

Host: 16-thread AMD, KVM, QEMU 11.1.2, vendor kernel `4.1.27-abl` with the
SDHCI gdbstub patch. Each row is a managed `boot` scenario; `boot_seconds`
comes from `result.json`.

```sh
python3 -B launch.py run --profile conformance --scenario boot \
  --smp N --memory MiB [--apic]
```

## CPU / APIC (cold cache)

| Guest CPUs | noapic | boot seconds (3 runs) | Verdict |
|---|---|---|---|
| 1 | yes | 5.46, 5.65, 5.74 | **keep** |
| 2 | yes | 5.58, 5.78, 5.79 | no win |
| 4 | yes | 5.83, 25.98, 5.69 | unstable outlier |
| 1 | no  | 5.59 | no win |
| 2 | no  | 5.75 | no win |

## Memory (cold cache)

| Memory | boot seconds (2 runs) | Verdict |
|---|---|---|
| 2048 MiB | 5.56, 5.60 | works; no gain |
| 3072 MiB | 5.58, 5.83 | no gain |
| 4096 MiB | 5.64, 5.74 | **keep** (Qt/llvmpipe headroom) |
| 8192 MiB | 5.53, 5.50 | within noise; not worth the host footprint |

## qcow2 cache (cold cache)

| Cache | boot seconds (2 runs) | Verdict |
|---|---|---|
| `writeback` | 5.92, 6.15 | **keep** |
| `writethrough` | 7.23, 5.77 | variable; only forced for persistent state |
| `directsync` | 46.26, 44.93 | avoid on this host (O_DIRECT/overlay cost) |

`--cache` selects the non-persistent drive mode; persistent `--state`
overlays always force `writethrough` so writes survive teardown. `unsafe`
exists for experiments only.

## CPU pinning (warm cache)

`--pin-cpu N` (one-shot runs and `daemon start`) pins all QEMU threads to an
allowed host CPU. Three managed `boot` runs each, after repeated warm boots:

| Pinning | boot seconds (3 runs) | Mean |
|---|---|---|
| unpinned | 4.234, 4.214, 4.271 | 4.240 |
| `--pin-cpu 0` | 4.209, 4.124, 4.194 | 4.176 |

The ~1.5% difference was consistent across all three pairs (every pinned run
beat every unpinned run), but the sample is small (n=3). It is too small to
justify changing the default, so pinning is **opt-in**. CPU 0 is often the
IRQ/housekeeping CPU; for isolation experiments prefer a non-housekeeping CPU
such as 12. `--smp >1 --pin-cpu N` squeezes all vCPUs plus I/O threads onto
one host CPU, which is usually slower. Invalid CPU numbers are rejected
against the process affinity mask.

The cold-cache tables above and this warm-cache table are not directly
comparable; compare rows within a table, not across tables.

## Automated benchmark

`bench.py` regenerates these timings without hand-parsing result files:

```sh
python3 -B bench.py --scenario boot --repeats 3 --cache writeback
python3 -B bench.py --scenario boot --repeats 3 --pin-cpu 0
python3 -B bench.py --scenario boot --repeats 3 --matrix --matrix-cpu 0
python3 -B bench.py --scenario boot --repeats 3 --kernel compat
```

It serializes on the global boot lock, writes
`runs/bench-<id>/bench.json` (`run`, `source`, `mean`, `median`, `min`, `max`),
retains each child log, and exits nonzero if any run fails. `--warmup N`
discards N unrecorded boots per scenario/variant first; warmup verdicts are
retained and must PASS. Compare a baseline and a
candidate with:

```sh
python3 -B bench.py --compare runs/bench-<base>/bench.json \
  runs/bench-<new>/bench.json --threshold 0.05
```

The comparison marks relative mean growth as `REGRESSED` and exits 1.
`bench.json` also records `metrics` for boot, readiness and scenario time;
pass `--metric readiness` or `--metric scenario` to compare those. `--matrix` compares cache
modes and optional pinned CPUs; its `summary` keys are grouped by scenario,
cache and pin. A four-cell matrix smoke (one run each) passed at
5.223/5.404/5.465/5.376 s; that is a smoke result, not a performance
conclusion — use `--repeats` for decisions.

## Decision

Keep `-smp 1`, `-m 4096`, `noapic` and `cache=writeback` as safe defaults.
SMP 4 showed a large boot-stall outlier; neither SMP 2/4 nor APIC removal
improved boot, and 8 GiB was within run-to-run noise. CPU pinning showed a
small (~1.5%) consistent gain at n=3, but not enough to justify a default; it
stays opt-in. The `--smp`, `--memory`, `--apic`, `--cache`
and `--pin-cpu` flags remain available for future Qt/rendering
measurements.
