# RTL-SDR adaptive-gain volunteer test

This branch always uses its bundled RTL-SDR Blog library and always runs the
experimental gain controller. It ignores `gain`, `agc`, `adaptive_gain`,
`lna_gain`, `mixer_gain`, and `vga_gain` in the INI file. Please use a single
RTL-SDR and keep the antenna, filter, LNA, and its power supply unchanged
throughout the run. Stop any other receiver that is using the dongle first.

On Debian, Ubuntu, or Raspberry Pi OS, install the build dependencies:

```sh
sudo apt update
sudo apt install git cmake g++ pkg-config libusb-1.0-0-dev
```

On macOS, install `cmake`, `pkgconf`, and `libusb` with Homebrew instead.
Then, on either platform, clone the test branch and build it:

```sh
git clone https://github.com/enricolorenzoni59/stream1090.git
cd stream1090
git switch --track origin/rtlsdr-autogain
git log -1 --oneline
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/stream1090 -h
```

If you already have a checkout, update it and rebuild before each run:

```sh
cd stream1090
git fetch origin
git switch rtlsdr-autogain
git pull --ff-only origin rtlsdr-autogain
cmake --build build --parallel 4
```

The help output should list **RTL-SDR Blog (advanced)** as a native device.
Check `configs/rtlsdr.ini` before starting: it tunes to 1090 MHz and pins the
R82xx tuner bandwidth to 3 MHz at the 2.4 Msps sample rate below. Its
`bias_tee` is `false`; set it to `true` only if your LNA needs power from the
dongle. If you have more than one dongle, set `serial` to the intended one.

Run for **at least 10 minutes**, then stop it with Ctrl-C. The output files
are created in the current directory:

```sh
./build/stream1090 -s 2.4 -u 12 -q -v -d ./configs/rtlsdr.ini \
  > autogain.avr 2> autogain.log
wc -l autogain.avr
```

`-v` records every five-second gain decision. Every 30 seconds the log also
contains decoded-frame counts, ADC level and clipping metrics, and a 16-bin
ADC histogram. The AVR file contains the decoded messages. Please send us:

- `autogain.log` and the result of `wc -l autogain.avr`;
- the commit shown by `git log -1 --oneline`, RTL-SDR model, tuner if known,
  and whether the LNA is powered by the bias tee;
- a brief description of the antenna, LNA, filters, and interference, plus
  whether reception changed during the test.

If practical, also send `autogain.avr` (compressed if large). The log alone
is enough for an initial gain-control review. The histogram can show ADC
clipping, but it cannot identify which frequency caused interference or
detect overload that happened before the ADC.
