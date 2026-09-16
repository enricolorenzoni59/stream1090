# Bounded DF11 correction: transfer from the ML investigation

2026-09-16. Base: `origin/enrico-dev` at
`17bfd4179639a594c5ae46cc06a05c7a5d03b712` (freshly fetched).
Branch: `codex/ml-precision-transfer`. This is a precision tradeoff, not a
claim of increased sensitivity or zero false positives.

## Problem and change

Previously, when a DF11 syndrome was neither a legal interrogator overlay
(0..79) nor correctable by the existing single-bit table, the decoder could
accept a trusted ICAO/CA and replace **all 24 parity bits** with computed
parity. The preamble/noise condition did not establish that these bits had
actually been received. It also refreshed cache liveness.

Remove that fallback. Retain clean DF11, the existing II/SI normalization,
and the bounded single-bit repair table. No neural network, GPU, new runtime
option or new per-candidate computation is introduced. The existing
normalization of valid interrogator parity to CRC zero is unchanged; output
is therefore still not always a byte-for-byte copy of transmitted DF11 PI.
Single-bit correction and existing AP/ES paths can still produce false outputs.

The new regression establishes an identity with two clean DF17 messages,
then injects an intact DF11 header with three corrupted parity bits. It
fails with exit 3 on the original implementation, passes after the fix,
and also verifies that single-bit repair and legal PI remain accepted.
All 14 CTest tests pass on both base and modified builds.

## Same-IQ results, 2.4 MS/s

Both binaries use `-s 2.4 -u 12 -q`, default Release CMake options with
`BUILD_TESTING=ON`. Binary hashes are in `protocol.json`.

| Labeled controls | Base true | Modified true | Base false | Modified false |
| --- | ---: | ---: | ---: | ---: |
| N53 + N73, 16 cases | 72,859 | 72,855 | 1,405 | 1,287 |
| New seeds 9101 + 9103, 24 seconds | 3,183 | 3,168 | 192 | 152 |

On the existing cases this removes 118 false outputs (8.40%) at a cost of
4 true outputs (0.0055%). All 118 removed false outputs are DF11: DF11
false outputs fall from 119 to 1. Existing labeled DF11 true outputs fall
from 60 to 56. The overall denominator is dominated by other formats.

Two old near-address mixed fixtures use `bcd000..bcd013`, excluded by this
branch's pre-existing ICAO range filter: both binaries emit zero. They are
retained explicitly in the per-case report but provide no sensitivity
validation. Do not compare these totals with the overnight frozen decoder,
which used different acceptance policies.

The two new mixed fixtures were created after the patch was fixed, using
new seeds and the existing waveform generator with near addresses changed
to `abc000..abc013`, which pass the range filter. They show a stronger
tradeoff: 40 fewer false outputs (20.83%), but 15 fewer true outputs
(0.47% overall; DF11 true 220 -> 205). DF11 false outputs fall 40 -> 0.
Other payload fields are synthetic and sometimes operationally unrealistic;
these are controlled stress tests, not an estimate of field false-positive
rates. More independent RF validation is needed before default deployment.

Scoring uses the existing research helpers: exact payload and one-to-one
reception-time matching within 24 samples, deduplication within 12 samples,
with clock registration from common payloads. DF11 PI is canonicalized for
both decoder and truth. This scores canonical message correctness, not
preservation of every transmitted PI bit. Registration details and per-DF
results are included in the JSON files.

## Real RF sanity check

Two 60-second splitter captures (`N42/0530UTC`, radios A and B):

| Radio | Base outputs | Modified outputs | Base DF11 | Modified DF11 |
| --- | ---: | ---: | ---: | ---: |
| A | 30,843 | 29,590 | 9,004 | 7,763 |
| B | 30,603 | 29,337 | 8,966 | 7,708 |

Modified outputs are subsets of base outputs in both replays. We cannot
label all removed RF messages false: these captures have no complete truth.
Small reductions outside DF11 also occur because acceptance changes cache
and phase processing state. Both variants replayed each 60-second input in
about 6.8–7.0 seconds; this is only a realtime sanity check under concurrent
research workload, not a controlled speed comparison.

## Reproduction and artifacts

This directory contains aggregate JSON, the exact research replay scripts,
and the fresh-fixture generation script. They depend on helpers in the
companion ML research checkout; they are not standalone project tests.
From that checkout, place frozen executables at
`runs/stream1090-precision/{baseline,bounded}`. Run `replay.py`, and
`PYTHONPATH=. python fresh.py` followed by `replay-fresh.py`; run
`replay-rf.py` for the RF check. Use the checkout's `.venv/bin/python`.
Existing IQ inputs are under `data/overnight/{N53,N73,N42}`. Scripts reuse
existing AVR files; use a fresh artifact directory or remove those outputs
before testing different binaries. Local logs, AVR and new IQ remain under
`runs/stream1090-precision`. Synthetic wall times were omitted from the
committed JSON because resumed runs reused some outputs.

Build and project tests:

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 3
ctest --test-dir build --output-on-failure
```

## Further small changes worth separate experiments

1. **Clean evidence should control trust lifetime.** Several ES repair paths
   call `markAsTrustedSeen`, extending trusted lifetime using corrected
   observations. Compare refreshing only ordinary liveness; test long gaps
   between clean anchors and repeated wrong-codeword corrections.
2. **Independent trust confirmation.** Audit the known-but-untrusted clean ES
   path, which can bypass the explicit candidate confirmation interval.
   Multiple nearby phase hypotheses should not count as independent evidence.
3. **Recover good DF11 with bounded PI-aware correction.** Search legal II/SI
   overlays and low-confidence bit corrections, rejecting ambiguous results.
   A small lookup or existing bit-confidence callback can do this without ML.
   This could recover some lost true frames, but has not been implemented or
   validated here; never reconstruct arbitrary parity from identity alone.
4. **AP is still the main residual precision problem.** A cached ICAO and a
   plausible altitude/squawk do not independently authenticate address-parity
   messages. Nearby unknown addresses can masquerade as known ones. Assess
   causal cache validation, bounded soft evidence and false-positive controls;
   downstream readsb is not a substitute for this validation.

The earlier ML experiments also found that a simple minimum bit-confidence
threshold can lose true messages without removing false ones. Do not port
that threshold, or claim a confidence margin proves authenticity, without
new measurements.
