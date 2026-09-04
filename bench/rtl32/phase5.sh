#!/bin/sh
# fase5: far vincere il 3.2. Tutte le corsa live di questa fase girano con
# --swap per cancellare l'asimmetria dei dongle (fase0: 0.2%).
#
# ESITO PRINCIPALE (2026-09-04): il preset 3.2 vincente e`
#   stream1090 -s 3.2 -u 24 -q -d ini-con-tuner_bandwidth=2430000
# compilato con -DENABLE_RTLSDR_BLOG=ON. Risultati in RESULTS.md.
#
# Storia della giornata in breve:
# 5.0  3.2->16 -q perde 14.8% contro 2.4->12 -q (lib di sistema)
# 5.1  gli stati IF piu` larghi (7/8 MHz) perdono contro il 6 MHz default
#      -> la larghezza di filtro IF e` esaurita, resta il refit dei tap
# 5.2  3.2-narrow + tap fittati sul narrow: PERDE ancora -14% (lib sistema)
# 5.3  con la lib vendored il 3.2 ribalta (+15.5%) ma il braccio A era sul
#      binario vecchio -> confondente librtlsdr
# 5.4  apples-to-apples vendored: pari (-1.1%) a up=16
# 5.5  a parita` di tap, narrow batte wide del +1.5% (vendored)
# 5.6  2.56 default: +1.1% per il 2.56 (a up=16)
# 5.7  up=24 + tap fittati per up24: +2.3% sul 2.56
# 5.8  bw=2000000: -10% -> 2430000 resta l'ottimo
# 5.9  bw=1400000: -29% -> non tagliare nel segnale
# 5.10 vs 2.4-narrow: +1.9% frames / +1.8% unici
# 5.11 vs 2.56-narrow: +10.3% (il narrow fa male al 2.56, bene al 3.2)
# 5.12 forma shippata (-q compilato): +1.0% frames / +0.6% unici vs 2.56
#
# Le fasi 5.0-5.1 (lib sistema) restano qui per riprodurle.
cd /Users/samknows/stream1090
S=${1:-120}

# 5.0 la domanda da cui si parte: chi vince oggi, 2.4 -q contro 3.2 -q?
python3 bench/rtl32/ab.py --a 2.4:12 --b 3.2:16 --seconds $S \
  --rounds 3 --swap --name "fase5.0 scontro diretto: 2.4->12 -q vs 3.2->16 -q" || exit 1

# 5.1a stato 7MHz vs il default di 3.2 (6MHz, perche' auto = rate > 2.43)
python3 bench/rtl32/ab.py --a 3.2:16 --b 3.2:16,bw=6500000 --seconds $S \
  --rounds 2 --swap --name "fase5.1a tuner bw @3.2->16: default(6MHz) vs 7MHz" || exit 1

# 5.1b stato 8MHz vs il default di 3.2
python3 bench/rtl32/ab.py --a 3.2:16 --b 3.2:16,bw=8000000 --seconds $S \
  --rounds 2 --swap --name "fase5.1b tuner bw @3.2->16: default(6MHz) vs 8MHz" || exit 1
