# Preset 3.2 Msps RTL-SDR: scoreboard e storia (2026-09-04)

Metodo: due dongle sullo stesso antenna via splitter, un processo stream1090
ciascuno, un round = 120 s, dongle scambiati a meta` round (`--swap`) per
cancellare l'asimmetria dei receiver (fase0: 0.17% frame). Metrica: frame AVR
e payload distinti. Tutto live salvo indicazione (replay su catture).

## Il preset

```
stream1090 -s 3.2 -u 24 -q -d ini
```
con ini: `agc = false`, `gain = 49.6`, `tuner_bandwidth = 2430000`
e binario compilato con `-DENABLE_RTLSDR_BLOG=ON` (librtlsdr vendored).

## Verdetto (live, 3x120s con swap, build-32v = lib vendored)

| B contro A | frame | unici | CPU B/A |
|---|---:|---:|---:|
| 3.2->24 narrow+q  vs  2.4->12 narrow | **+1.9%** | **+1.8%** | 1.50 |
| 3.2->24 narrow+q  vs  2.56->12 6MHz (fase5.7, tap -f) | **+2.3%** | **+2.3%** | 1.39 |
| 3.2->24 narrow+q  vs  2.56->12 6MHz (fase5.12, -q compilato) | **+1.0%** | +0.6% | 1.37 |
| 3.2->24 narrow+q  vs  2.56->12 narrow (fase5.11) | **+10.3%** | **+9.7%** | 1.38 |

Il 3.2 vince su 2.4 e 2.56, ma di poco (~1-2%, costo 1.4-1.5x CPU).

## Le tre scoperte che contano

1. **La librtlsdr cambia tutto.** stream1090 compilato con la lib di sistema
   (Homebrew 2.0.2) e quello con il fork vendored (`ENABLE_RTLSDR_BLOG=ON`)
   producono stati di tuner diversi a parita` di settings: misurati in RMS sul
   rumore, `tuner_bandwidth = 2430000` vale 2.28 con la vendored e 1.29 con la
   di sistema. Sul 2.4-auto la vendored decodifica **~+25-35%** di frame rispetto
   alla di sistema (fase5.3 vs 5.4, braccio A). Il fork gestisce il tracking
   filter a 1090 MHz e l'IF meglio. Tutte le conclusioni del diario tarate con
   la lib di sistema vanno rilette con la vendored.
2. **A 3.2 il default auto e` lo stato 6 MHz e fa male.** Auto = `set_bw(rate)`;
   a 3.2 cadono nello stato 6 MHz il cui rumore si aliasa dentro Nyquist
   (1.6 MHz) dove il FIR IQ non puo` piu` toglierlo: ~-15% di frame (fase5.0).
   Con `tuner_bandwidth = 2430000` il ladder stretto passa ±1.2 MHz, dentro
   Nyquist, zero aliasing. Su 2.56 invece il narrow fa male (-15%, fase5.11):
   il suo ottimo resta il 6 MHz. La scelta dello stato e` per-rate.
3. **up=24 e i tap del narrow.** A parita` di tutto, 24 > 16 > 12 > 8 sia
   offline (replay) sia live. Il refit DE dei 27 tap sui dati narrow a up=24
   vale +2.1% sul training, +0.2/+0.5% su due finestre held-out; compilato in
   `getTaps<Rate_3_2_Mhz>()` (custom_filters/enrico_20260904_3_2_narrow_up24_taps_27.txt).

## Cosa NON ha funzionato

- Stati IF piu` larghi a 3.2 (7 MHz: -2.0%, 8 MHz: -3.6% contro il 6 MHz).
- bw piu` stretti del ladder 2.43M: 2.0M -10%, 1.4M -29% (taglia nel segnale).
- Refit DE da seed generico a up=16 wide: +0.6% training. I tap sono vicino a
  un ottimo locale; la leva tap e` da sola del 1-2%, non di piu`.
- Gain 44.5 contro 49.6 a 3.2: -16% (fase3.4). Il gain resta al massimo.

## Riproduzione

- fit: `filter_utils/filter_opt.py --data <capture.cu8> --fs 3200000
  --fs-up 24000000 --num-taps 27 --num-gain-points 13 --workers -1
  --resume <log> --margin 0.25` con `STREAM1090_EXE` puntato al binario
  build-32v. Catture: `bench/rtl32/captures/t{4,5,6}_3200k_narrow_30s.cu8`
  (15 s effettivi: capbw conta byte, 2 per campione IQ).
- catture duali con bw esplicito: `bench/rtl32/capbw` (compila con
  `cc -I thirdparty/rtl-sdr-blog/include ... -L thirdparty/rtl-sdr-blog/build/src -lrtlsdr`).
- A/B live: `bench/rtl32/ab.py --a ... --b ... --swap` con `,build=build-32v`.
