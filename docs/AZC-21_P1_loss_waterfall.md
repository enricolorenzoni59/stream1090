# AZC-21 P1: stage-attributed loss waterfall on `main`

> **`enrico-dev` adaptation:** the measurements below are the historical
> `origin/main` results from the original commit, not current `enrico-dev`
> results. On `enrico-dev`, the instrumentation also reports
> `WATERFALL_DF17_FIRST_RELEASED`, because this branch already holds and emits a
> first DF17 after confirmation. Its remaining first-frame loss ceiling is the
> number held minus the number released. Re-run the replay before using any of
> the historical ceilings below to choose a change for this branch.

## `enrico-dev` replay with John's 27-tap filter

Replay of `sample20260713_pi51-g20-t4-20M-5min` at 10 -> 24 Msps with
`repo_au27_numeric.txt`, `END_STATS=ON`, and ORBGRAND disabled:

- baseline and instrumented output are byte-identical: 99,998 AVR lines,
  SHA-256 `ab3b200e5f32bcb7e07b84a4737d871deb641f7905c2b757e0d50c6e46fc3227`;
- 1,310 altitude candidates and 730 squawk candidates failed both the
  plausibility check and exact-payload confirmation; only one altitude
  candidate was rescued by exact-payload confirmation;
- 1,652 candidates were rejected by the noise-floor/preamble gate;
- 84 first CRC-clean DF17 sightings were held and 37 were later released, so
  the residual first-sighting ceiling is at most 47 frames.

These are rejection counts, not safe-recovery counts. In particular, emitting
all 2,040 AP validation rejects or all 1,652 noise-floor rejects would admit
fabricated frames. They rank where to add stronger confirmation: the largest
low-CPU target is confirmation of rejected AP altitude/squawk replies, while
the preamble statistic targets the second-largest bucket.

Every queued lever (first-frame release, AP-confirmation-style recovery)
targets a different failure stage, but until now no number existed for
how many frames actually die at each stage on `main`. This instruments
one replayed pass over the pi51 sample to count candidates, CRC/address
outcomes, and validation-gate rejections, per DF type, and turns that
into a ranked, priced list instead of a backlog of "promising" levers.

**Note on provenance**: an earlier version of this instrumentation was
built against a stale local ref of `main` that was 41 commits behind
`origin/main` and had 8 local-only commits never pushed anywhere — a
tooling mistake, not a code issue, but it produced wrong numbers and a
PR (#32) that has been closed. This is the redone version, built and
verified against the actual `origin/main` (`329280e`).

## Build and validation

- `origin/main` @ `329280e` + this commit's instrumentation only.
  `STATS_ENABLED` (already the default) gates all counters; no new build
  flag.
- Compiled with `-DEND_STATS=ON` so the stats dump reflects the whole
  5-minute run, not a periodic rolling window.
- **Byte-identical AVR output vs unmodified `origin/main`**: both builds
  produce sha256
  `b6c78ece57ad23bf6b3d1fe88e0e8c16fa6dc4ce210926c8cd6c6a9b8888e55b`
  over `sample20260713_pi51-g20-t4-20M-5min` (`-s 10 -u 24 -q`), 99,184
  frames. Pure-instrumentation change — zero behavioral effect on what
  gets emitted.
- Deterministic: two runs of the instrumented build produce identical
  AVR output *and* identical waterfall counters.

## Final emitted per DF (unchanged from baseline)

| DF | 0 | 4 | 5 | 11 | 16 | 17 | 20 | 21 | total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| count | 12,281 | 1,766 | 878 | 12,540 | 863 | 51,694 | 10,772 | 8,390 | 99,184 |

## What's already on `main` that the earlier (wrong-branch) version missed

`origin/main` already has two loss-relevant mechanisms this waterfall
had to account for rather than treat as pure gaps:

1. **`addressParityRejects()`** — a pre-dispatch gate for DF 0/4/5/16/20/21
   that rejects any candidate whose syndrome doesn't match a cached
   address *before* `handleStream()` even runs. This makes the
   "unknown address" branches inside `handleAcasSurvShortMessage` /
   `handleAcasCommBLongMessage` dead code — the real address-recognition
   stage for these DF types is `addressParityRejects()`, and that's
   where this patch's `SHORT_ADDR_UNKNOWN` / `LONG_CB_ADDR_UNKNOWN`
   counters live.
2. **`ICAOCache::confirmRejectedShort/Long`** — an unconditional (not
   flag-gated) exact-payload two-sighting rescue for frames that fail
   the altitude/squawk plausibility check: a byte-identical repeat
   within a bounded tick window is admitted anyway. This is the same
   mechanism PR #29 (`agent/ap-confirmation-gate`) turned into an opt-in
   flag on a *different* branch lineage — on `origin/main` it's already
   always on. `REJECT_ALTITUDE`/`REJECT_SQUAWK` below count only the
   frames this rescue *still* couldn't save, not the raw check failures.
3. **Noise-floor/preamble gate** (`signalAtNoiseFloor()` +
   `preambleConfirms()`, `MIN_SNR=2.5` and `ENABLE_PREAMBLE_GATE=ON` by
   default) — rejects address-parity and DF11 candidates that are both
   below the SNR floor and have no confirming preamble. Newly counted
   here as `NOISE_FLOOR_REJECTED`; not present at all in the earlier
   wrong-branch version of this analysis.

## Extended squitter (DF 17/18/19) — the only class with an independent CRC

| stage | count | note |
|---|---:|---|
| header/candidate (shift-register DF-code match) | 670,040,174 | dominated by coincidence, see caveat |
| CRC-clean | 50,725 | |
| ├─ known ICAO → sent to output | 50,612 | |
| └─ unknown ICAO → cached, **not emitted** | **113** | **first-frame-release ceiling** |
| CRC-bad | 669,015,351 | the actual noise floor |
| ├─ repaired (error-table + erasure) & emitted | 41,603 | |
| └─ unrecovered | 668,942,720 | |
| **final emitted (DF 17+18+19)** | **51,694** | gap to 50,612+41,603 is the existing same-aircraft dedup window, already tracked by the printed Dups% column |

## DF 0/4/5 (short) and DF 16/20/21 (long) address-parity formats

| stage | short (0/4/5) | long (16/20/21) |
|---|---:|---:|
| header/candidate entering `addressParityRejects()` | 679,597,207 | 670,235,243 |
| address-unknown (rejected here) | 679,461,245 | 670,063,336 |
| → address-recognized, reaches per-DF handler | 135,962 | 171,907 |
| address known but not alive (`isAlive()` false) | 8 | 6 |
| rejected at noise floor, no preamble (`NOISE_FLOOR_REJECTED`, combined w/ DF11) | 1,603 total across DF 0/4/5/11/16/20/21 | |

Validation gate (altitude/squawk), counted only where the existing
exact-payload rescue (`confirmRejectedShort/Long`) *also* failed:

| reject reason | unrescued (still lost) | rescued by existing mechanism | DF types |
|---|---:|---:|---|
| implausible altitude | **1,350** | 1 | 0, 4, 16, 20 |
| implausible/changed squawk | **773** | 0 | 5, 21 |
| **AP-confirmation-style ceiling (sum)** | **2,123** | 1 | |

The existing rescue mechanism saved 1 frame out of 2,124 candidates on
this sample — effectively a no-op here. It requires an exact byte-for-byte
repeat of the payload within a bounded tick window, which a short,
single-pass 5-minute capture rarely produces twice; this ceiling is
therefore close to the true remaining opportunity, not already mostly
captured.

## DF 11

Header/candidate: 223,541,030 (same coincidence caveat). Existing
GOOD_CRC / 1-bit-fix / BAD_CRC breakdown and the noise-floor gate share
the `NOISE_FLOOR_REJECTED` counter with the address-parity formats
above.

## Caveat: what "header" does and doesn't mean here

`main`'s demodulator has no independently-gated preamble stage before
the *first* dispatch check: every one of the 24 phase-shifted
shift-register windows is evaluated on every ~1 MHz tick for whether its
top 5 bits equal a recognized DF code, and any match enters
`addressParityRejects()` or the DF17/DF11 handlers — real signal and
thermal-noise coincidence alike, indistinguishably, at that point. The
header counters run into the hundreds of millions for exactly this
reason; real preamble-aligned headers are a vanishing fraction of them.
The *actual* filtering on this codebase happens in three real stages,
now all counted: `addressParityRejects()`'s cache lookup, the
noise-floor/preamble gate, and the altitude/squawk plausibility check.

## Per-lever ceilings, ranked

1. **AP-confirmation-style validation-gate recovery: ≤2,123 frames**
   (1,350 altitude + 773 squawk unrescued by the existing exact-payload
   confirmation) — ≈+2.1% on this sample's 99,184 total. The bigger
   lever on this capture.
2. **First-frame/ES-new-ICAO release: ≤113 frames** — ≈+0.22% on the
   51,694 emitted ES messages.
3. **Noise-floor/preamble gate relaxation: ≤1,603 frames** — new finding,
   not on the original matrix. This is a deliberate precision safeguard
   (default `MIN_SNR=2.5`, `ENABLE_PREAMBLE_GATE=ON`), so unlike the
   other two this ceiling trades against false positives, not just
   recall; worth a measured look but not a like-for-like comparison with
   1 and 2.

All three are upper bounds on what a gate *rejected*, not on what a
lever would *safely* recover.

## Instrumentation added

`include/Stats.hpp`: 17 new `EventType` values plus a
`WATERFALL_<NAME>: <count>` line per counter appended to the existing
end-of-run stats dump — no new build flag, reuses the existing
`STATS_ENABLED` gate (default on) under `STATS_END_ONLY` accumulation.

`include/DemodCore.hpp`: one `logStats(...)` call at each of 17 points:
function entry for the DF17 and DF11 handlers and `addressParityRejects()`
(header/candidate), the address-unknown branch inside
`addressParityRejects()`, the still-reachable address-known-not-alive
branches in the two per-DF handlers, the three noise-floor-gate call
sites, the DF17 first-sighting-not-emitted branch, and the
altitude/squawk validation outcomes (unrescued reject vs. rescued by the
existing confirmation mechanism) in both `sendFrame*Aligned` functions.
No control-flow change: verified via byte-identical AVR output above.
