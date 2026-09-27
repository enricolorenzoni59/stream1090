# SDRplay IQ telemetry for manual gain evaluation


With the metrics endpoint active, SDRplay also samples one complex I/Q pair in
64 before host FIR/Q14 processing. Sampling phase is preserved across callback
boundaries. A fixed 128 KiB absolute-value histogram and integer accumulators
avoid allocation, locks and atomics per observed scalar. Publication occurs
after one second of contiguous received IQ; gaps discard partial windows.
The signal levels describe the API's signed-16 output, not raw ADC codes or
effective ADC resolution. No automatic gain adjustments are performed.

| Prometheus series (prefix `stream1090_`) | Meaning |
| --- | --- |
| `sdrplay_iq_level_dbfs{statistic="rms"}` | Scalar RMS, pooled I and Q |
| `sdrplay_iq_level_dbfs{statistic="abs_p50"}` | Median absolute scalar amplitude |
| `sdrplay_iq_level_dbfs{statistic="abs_p999"}` | 99.9th percentile absolute scalar amplitude |
| `sdrplay_iq_level_dbfs{statistic="sampled_peak"}` | Maximum among observed scalars |
| `sdrplay_iq_noise_sigma_estimate_dbfs` | Median absolute value / 0.67449, a zero-centred Gaussian-noise proxy |
| `sdrplay_iq_sampled_headroom_db` | Distance of the sampled peak below API full scale |
| `sdrplay_iq_scalar_fraction{condition="rail"}` | Fraction exactly −32768 or +32767 |
| `sdrplay_iq_scalar_fraction{condition="abs_ge_32000"}` | Fraction near full scale |
| `sdrplay_iq_scalar_fraction{condition="abs_le_1"}` | Fraction near zero |
| `sdrplay_iq_window_sampled_scalars` | Number of observed I/Q scalars in the completed window |
| `sdrplay_iq_window_valid` | Complete window available since startup/latest gap; cleared on failure/stop |
| `sdrplay_iq_window_age_seconds` | Age since publication; −1 before the first window of a session |
| `sdrplay_iq_windows_total` | Completed-window counter, cumulative across sessions |
| `sdrplay_overload_active` | SDK state: 1 detected, 0 corrected, −1 unknown/stopped |

Amplitude dBFS uses 32768 as scalar full scale, with silence floored at −120
dBFS. The noise estimate assumes a zero-centred Gaussian population; DC,
interference and sufficiently busy signals can bias it. Fractions are 0..1,
not percentages. At 4 MS/s a full window observes 125,000 scalars. The stride
is deterministic, not random, so periodic signals can bias the sample. Rare
peaks can be missed: zero rail fraction or positive sampled headroom does not
prove absence of analog overload. The SDK overload state is independent and
starts unknown until an event arrives; the existing overload counter counts
detected notifications only, not corrected notifications.

Level gauges retain the last published values across gaps/stops and until a
new session produces a window. Always combine them with validity, age and
device status. Example PromQL for a current noise estimate:

```promql
stream1090_sdrplay_iq_noise_sigma_estimate_dbfs
  and (stream1090_sdrplay_iq_window_valid == 1)
  and (stream1090_sdrplay_iq_window_age_seconds >= 0)
  and (stream1090_sdrplay_iq_window_age_seconds < 3)
  and (stream1090_device_up == 1)
```

A gap followed by less than one continuous second intentionally leaves the
window invalid. Compare the gauges with IF GR/LNA settings and SDK overloads;
do not interpret a gain change from message counts alone. This is observation
for a future gain controller; SDRplay AGC remains disabled.

## Gap sizes and missing IQ

`stream1090_sdrplay_events_total{event="gap"}` counts all SDK sample-sequence
discontinuities. The generic `sample_drop_*` metrics are not the SDRplay gap
counters: they may stay zero while the SDRplay gap counter increases.

The additional metrics distinguish frequency from severity:

- `stream1090_sdrplay_missing_samples_total`: known missing **complex IQ pairs**.
- `stream1090_sdrplay_gap_duration_seconds`: histogram of forward gap lengths
  (`_bucket`, `_sum`, `_count`), calculated using the configured input rate.
- `stream1090_sdrplay_largest_gap_seconds`: largest forward gap since process start.
- `stream1090_sdrplay_sequence_errors_total`: backward/ambiguous jumps requiring
  recovery; their size is unknown and excluded from the metrics above.

Ordinary 32-bit counter rollover is not a gap. A forward gap crossing rollover
is counted by its modular size. All counters and the maximum survive device
recovery within the same process, but reset on process restart.

Missing IQ time as a fraction of wall time over five minutes:

```promql
rate(stream1090_sdrplay_gap_duration_seconds_sum[5m])
```

Multiply by 100 for percent. This describes missing API samples, **not the
fraction of lost ADS-B messages**: discarding partial DSP blocks, filter/decoder
restart transients and device recovery can lose additional useful data. A gap
counter alone cannot quantify the effect on decoding or validate MLAT.
