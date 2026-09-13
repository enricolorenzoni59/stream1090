> [!IMPORTANT]
> `enrico-dev` is an experimental integration branch containing changes not yet
> available in upstream Stream1090. It is intended for testing and real-world
> feedback; individual features may still be revised before being proposed or
> merged upstream.

## Changes compared with upstream `main`

### RTL-SDR reception

- Added continuous automatic PPM calibration using the RTL-SDR sample clock
  measured against the host monotonic clock.
- Calibration uses a median of clean measurement windows, rejects windows with
  sample loss, and applies bounded corrections with configurable warm-up,
  deadband, maximum step and absolute safety limit.
- The configured RTL-SDR centre frequency is now applied during device startup,
  before sample-rate configuration can trigger an invalid zero-frequency retune.
- RTL-SDR handles are closed correctly when post-open tuner configuration fails.
- Added explicit tuner, backend and bandwidth diagnostics at startup.
- Added detection and accounting of USB/FIFO sample loss, including a recovery
  window that distinguishes transient callback backlog from persistent loss and
  a configurable watchdog limit.
- Added 3.2 Msps RTL-SDR input paths with 8, 12, 16 and 24 MHz output rates.
- Added an experimentally selected 3.2 → 24 MHz preset with a narrow tuner state
  and refitted FIR taps, plus guardrails for unsupported backend configurations.
- Sharpening cubic interpolation replaces linear interpolation on the supported
  RTL-SDR resampling paths; the original kernel remains selectable at build time.
- The RTL-SDR Blog fork is maintained as a Git subtree, with the local adaptations
  and upstream revision documented separately.
- Fixed static libusb linkage for the vendored RTL-SDR Blog backend on macOS.
- The vendored `rtl_test -p` can compare its traditional wall-clock estimate with
  `CLOCK_MONOTONIC_RAW` on macOS.

### Airspy reception

- Corrected the half-complex-sample timing mismatch between the Airspy I and Q
  branches inside the IQ FIR, improving message yield without an additional pass.
- Added startup diagnostics for the Airspy board, firmware, sample format, sample
  rate and transfer settings.

### Decoder correctness and recovery

- Added airborne CPR decoding utilities.
- ICAO cache capability updates no longer discard existing aircraft state.
- New aircraft must be observed a second time before their address becomes
  trusted, without delaying valid output frames.
- DF11 address-parity fallback now requires adequate SNR and preamble evidence.
- Repaired airborne positions are accepted only when consistent with a clean CPR
  position pair.
- Confirmed output timestamps are kept monotonic.
- CRC error-table misses use a compact occupancy bitmap before the more expensive
  lookup.
- Added optional, guess-budgeted ORBGRAND recovery as a last-resort DF17 repair
  path. It is disabled by default for controlled A/B testing.
- Added stage-attributed loss-waterfall instrumentation to show where candidate
  frames are rejected or recovered.

### Native network output

- Added native multi-client AVR/raw and Beast TCP servers.
- AVR and Beast listeners can run simultaneously on separate ports.
- Network delivery is isolated from the DSP thread through a bounded,
  allocation-free queue.
- Slow clients have bounded buffering and are disconnected without blocking other
  clients or sample processing.
- Legacy AVR output on stdout remains available and can run alongside TCP output.
- AVR stdout writes are batched on a cadence instead of being flushed once per
  frame.
- Shutdown performs a bounded drain and correctly handles partial or failed writes.
- Native TCP output removes the need for `socat` in the usual readsb integration.

### Prometheus observability

- Added an optional Prometheus endpoint with `/metrics`, `/healthz` and `/readyz`.
  It listens on loopback by default and remains inactive until requested.
- Exposes process identity and resources, device health, watchdog activity,
  configuration reloads and the applied receiver settings.
- Exposes effective receiver gain, including RTL-SDR LNA, mixer and VGA stages.
- Exposes demodulator events, messages by downlink format, repairs and rejected
  repairs, deduplication, extended-squitter groups and DF18 control fields.
- Exposes per-frame RSSI, signal, noise, SNR and preamble-score histograms, plus
  tracked and trusted aircraft counts.
- Exposes AVR/TCP output totals, connected clients, slow-client disconnections,
  queue drops and log counts.
- Exposes sample-loss events, cumulative missing IQ pairs and the worst observed
  deficit.
- Exposes the complete automatic PPM controller state: phase, effective
  configuration, applied correction, observed sample rate, residual and estimated
  crystal error, measurement progress and age, discarded windows and controller
  decisions.
- Includes the source commit in `build_info` and provides initial Prometheus
  recording and alerting rules.
- Prometheus support can be compiled out with `-DENABLE_METRICS=OFF`.

### Performance and build options

- Added host CPU tuning, optional explicit CPU selection, link-time optimization
  and DSP loop unrolling.
- Added a complete GCC/Clang profile-guided optimization workflow.
- Removed the per-bit statistics clock check from the 1 MHz demodulation loop.
- Replaced sorting of the SNR noise window with a linear-time calculation.
- The filter optimiser can evaluate candidates in parallel, stop when further
  runs are no longer productive, and keeps seeded values within their bounds.
- FIR coefficients are saturated safely to Q15.
- Built-in and file-provided tap sets are rejected when they exceed the accumulator
  contract instead of being silently reshaped.
- Fixed GCC warning-as-error failures not detected by the macOS build.

### Development and test coverage

- Added pinned clang-format 22.1.7 configuration and check/write scripts for
  first-party sources.
- CI now treats compiler warnings as errors and tests the vendored RTL-SDR Blog
  backend on macOS.
- CI checkout support was updated for Node.js 24.
- CMake unit-test declarations were consolidated into a reusable helper.
- Added regression tests for CPR position handling, first-frame trust, repaired
  positions, sharpening interpolation, RTL-SDR serial selection, noise false
  positives, Prometheus exposition, AVR/Beast encoding and TCP server behaviour.
- TCP tests cover multiple clients, ordering, reconnection, queue overflow,
  slow-client isolation and shutdown using an in-tree decoder.

-----------------------------------------------------------------------------------------

# Stream1090
Mode-S demodulator written in C++ with CRC-based message framing.

Stream1090 is a proof of concept implementation taking a different approach in order to identify mode-s messages in an SDR signal stream.
Most implementations look for the so-called preamble (a sequence of pulses anounncing a message). Stream1090 skips this step and maintains
directly a set of shift registers. Based on the CRC sum and other criteria, messages are being identified. The hope is that in high traffic
situations, a higher overall message rate can be achieved compared to a preamble based approach.

## Features
- CRC-based message framing: Cannot miss a message, because it missed the preamble.
- Error correction: The CRC sum is computed regardless of the data, so why not use it for error correction whenever possible.
- Not output sensitive: The majority of the computational work does not depend on the message rate.
- MLAT support.
- Support for Airspy and RTL-SDR dongles.
- Support for 6 or 10 Msps for Airspy and 2.4 or 2.56 Msps for RTL-SDR devices.
- IQ Low-pass filtering including customization and optimization (optional).
- Seamless integration into readsb/dump1090-fa based stacks.

## Requirements
- RTL-SDR based dongle or Airspy with antenna etc.
- Debian-based Linux (Ubuntu, Raspberry Pi OS, ...) or macOS
- Optional: RaspberryPi 5 or 4 should work for most settings. 
  This depends on your settings
- Optional: For RTL-SDR (not airspy), a RaspberryPi 3B and Zero 2 W seems to work without cooling.


## Table of Contents
- [Compiling](#compiling-stream1090)
- [Code Formatting](#code-formatting)
- [First Steps](#first-steps)
- [Upsampling](#Upsampling)
- [Low-Pass Filter](#low-pass-filter)
- [Stack Integration](#stack-integration)
- [Advanced Usage](./README_ADV.md)
- [Experimental Features](#experimental-features)

This is a first draft of the new README. The old complicated one is [here](./OLD_README.md)

## Compiling Stream1090
Regardless of your hardware, you will need
- cmake (3.10 or higher)
- C++ compiler that supports C++20

Stream1090 has native device support for Airspy and RTL-SDR based dongles.
For both, you will need the dev version of the corresponding libraries. 

- For Airspy ```sudo apt install libairspy-dev``` 
- For RTL-SDR ```sudo apt install librtlsdr-dev``` 

On macOS, install the build tools and device libraries with Homebrew:

```brew install cmake pkgconf airspy rtl-sdr```

We are ready to compile. Switch to the stream1090 folder and do the usual cmake thing.

1. Create a build folder ```mkdir build && cd build```
2. Run CMake there with ```cmake ../``` 
3. Build the project using ```make```

Run ```./stream1090 -h``` in the build directory to bring up the help screen. Verify that the second line starting with ```Native device support``` has your device type listed (```Airspy``` and/or ```RTL-SDR```). 

To build and run the unit tests, enable `BUILD_TESTING` during configuration:

```
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build
```

## Code Formatting

Stream1090 uses clang-format 22.1.7 for its first-party C and C++ sources. The RTL-SDR Blog subtree under `thirdparty/` is deliberately excluded. Check or update the formatting with:

```sh
./scripts/clang-format.sh --check
./scripts/clang-format.sh --write
```

Set `CLANG_FORMAT` to an explicit executable path if it is not available as `clang-format-22` or `clang-format`.

## First Steps
Before we start, you have to understand how stream1090 works. The rough principle is:
```
    stream1090
        |
        v
     decoder 
(readsb or dump1090-fa)
```
**Important:** Stream1090 is a demodulator, **NOT** a decoder. It looks for messages in the signal stream, it does **NOT** extract information.
You will need a decoder like readsb or dump1090-fa. More on this later.

First thing to do is to make sure that no other software is using your SDR hardware. Stream1090 requires exclusive access to the SDR device.

As a next step, we configure stream1090 for a test run without any decoder. The configuration is split into two parts. 

- Device specific parameters
- General parameters

The latter ones are passed via command line, while the device specific ones are located in a config file. 

### Device specific configuration
The stream1090 directory contains a folder named ```./configs```. There you can find two device specific files, ```rtlsdr.ini``` and ```airspy.ini```. 
Edit the corresponding file for your device and read the comments. For now you may only want to adjust the gain settings with one exception:

For RTL-SDR devices, the startup diagnostics identify the linked librtlsdr backend, the detected tuner, and whether the tuner bandwidth was explicitly configured. An `auto` bandwidth setting means librtlsdr derives it from the selected sample rate.

**Important:** If you are powering an LNA via bias-tee, you have to turn that on by setting ```bias_tee = true```. It is off by default.

For common R820T/R820T2 receivers, the supplied `rtlsdr.ini` pins
`tuner_bandwidth` to 3 MHz. At the commonly used 2.4 and 2.56 Msps sample rates,
librtlsdr maps this request to the 6 MHz IF filter state. Setting it explicitly
keeps the tuner state consistent across librtlsdr implementations. Stream1090
warns at startup when the setting is missing on these tuners. Other tuner types
or sample rates may need a different value.

RTL-SDR users can opt into continuous crystal calibration with
`auto_ppm = true`. Stream1090 measures the shared tuner/ADC clock against the
host monotonic clock, takes the median of several clean windows, and applies
the bounded correction through librtlsdr. See `configs/rtlsdr.ini` for the
warm-up, window count, deadband, step, and safety-limit controls. Long-running
measurements are deliberate; carrier offsets from individual aircraft are not
used as a reference.

### Minimal running example
For the sake of a first try, we will focus only on parameters that are necessary to get things up and running. You may have noticed that the sample rate is not part of the ini file. There are reasons for that. 

So we have to tell stream1090 two things to get going:

- The sample rate via ```-s <rate>``` in MHz
- The location of the device configuration file via ```-d <file.ini>```

However, all messages found by stream1090 will be written to stdout. Since we cannot use them right now, we will suppress them by sending them to ```/dev/null```. 

Switch to the stream1090 directory. In case of RTL-SDR, we will select 2.4 MHz as sampling rate.

```./build/stream1090 -s 2.4 -d ./configs/rtlsdr.ini > /dev/null```

For Airspy the minimum supported sample rate is 6 MHz

```./build/stream1090 -s 6 -d ./configs/airspy.ini > /dev/null```

In both cases you should see stream1090 starting up and after around 5 seconds some statistics similar to this.
```
-------------------------------------------------------------
|     Type |  #Msgs |  %Total |    Dups |   Fixed |   Msg/s | 
-------------------------------------------------------------
|    ADS-B |    875 |   16.4% |   20.9% |   36.3% |     175 | 
|   Comm-B |   1112 |   20.9% |      0% |         |   222.4 | 
|     ACAS |    289 |    5.4% |      0% |         |    57.8 | 
|     Surv |    757 |   14.2% |      0% |         |   151.4 | 
|    DF-11 |   2298 |   43.1% |   23.7% |   15.9% |   459.6 | 
-------------------------------------------------------------
|  112-bit |   2005 |   37.6% |   10.3% |   17.9% |     401 | 
|   56-bit |   3326 |   62.4% |   17.7% |   11.9% |   665.2 | 
-------------------------------------------------------------
|    Total |   5331 |    100% |   15.1% |     14% |    1066 | 
-------------------------------------------------------------
(Max. msgs/s 1066)
Messages Total 30483
DF 0 : 271
DF 4 : 642
DF 5 : 115
DF 11 : 2298
DF 16 : 18
DF 17 : 875
DF 20 : 861
DF 21 : 251
5000000 iterations @1MHz
```
If stream1090 does not start up, you may want to add the ```-v``` flag which enables verbose output.
If it works, your statistics will probably show a much lower message rate. However, the goal was to get stream1090 up and running. Now it is time to make use of its features.

## Upsampling
Stream1090 is build around the idea to take the samples from the SDR that come in at a rate specified via
```
-s <rate>   Input sample rate in MHz
```
and upsample them to a higher rate before processing them. This upsample rate can be specified via
```
-u <rate>   Upsample rate in MHz
```

However, this rate cannot be chosen arbitrarily. There is a certain set of allowed combinations which can be obtained by running ```./stream1090 -h```.
In a much nicer table, they look like this:
| Input | Upsample | Input type | Device |
|------|--------|------| ------|
|  2.4  |  8 | uint8 IQ | RTL-SDR |
|  2.4  |  12 | uint8 IQ | RTL-SDR |
|  2.56  |  8 | uint8 IQ | RTL-SDR |
|  2.56  |  12 | uint8 IQ | RTL-SDR |
|  6  |  6 | uint16 IQ | Airspy |
|  6  |  12 | uint16 IQ | Airspy |
|  6  |  24 | uint16 IQ | Airspy |
|  10  |  10 | uint16 IQ | Airspy |
|  10  |  24 | uint16 IQ | Airspy |

The 2.4 → 12, 2.56 → 8 and 2.56 → 12 MHz RTL-SDR paths use a sharpening cubic
interpolation kernel by default. Configure with `-DINTERPOLATION_KERNEL=1` to
restore linear interpolation. Other rate combinations are unaffected.

The rule of thumb here is simple: The higher the upsample rate, the more messages will be found, but at the cost of higher CPU usage.
If you do not care about CPU usage, then use the highest upsample rate. For RTL-SDR this would be something like
```
./build/stream1090 -s 2.56 -u 12 -d ./configs/rtlsdr.ini > /dev/null
```
There is no higher upsampling rate in this case. For Airspy you can do
```
./build/stream1090 -s 6 -u 24 -d ./configs/airspy.ini  > /dev/null
```
or if your hardware supports 10 Msps sample rate
```
./build/stream1090 -s 10 -u 24 -d ./configs/airspy.ini > /dev/null
```

#### A note on RTL-SDR devices and 2.56 MHz
In general it is assumed that 2.4 MHz is the highest reliable sample rate for an RTL-SDR device. However, after experiments with different sticks, it turned out that at 2.56 MHz, samples are dropped only once at the beginning. Afterwards, no sample loss has been observed.

On macOS, the vendored `rtl_test -p` prints PPM estimates from both the wall clock used by upstream `rtl_test` and `CLOCK_MONOTONIC_RAW`. This makes clock corrections applied by macOS visible without changing the Homebrew-installed `rtl_test` executable. Configure with `-DBUILD_UTILS_RTLSDR=ON` to build the vendored command-line tools.


## Low-Pass Filter
Stream1090 offers the option to apply a low-pass filter to the IQ-pairs coming from the SDR device. It turned out that this increases the message output significantly in many cases at the cost of higher CPU usage.
To enable filtering use
```
-q    Enables IQ FIR filter with built-in taps
```
Stream1090 comes with a single filter for each sample rate combination. These have been optimized for a set of pre-recorded sample data provided by people from around the world with different setups.

### Airspy I/Q branch alignment

The Airspy's `U16_REAL` stream is one real ADC stream, and stream1090 pairs it
as `I = x[2k]`, `Q = x[2k+1]`. Those two samples sit half a complex sample
apart in time, so without a correction the pair the magnitude is formed from is
not an analytic one, and the two branches disagree about which instant they
describe. libairspy's own converter closes that gap with a half-band filter on
one branch and a matching delay on the other.

The `-q` filter does it inside the FIR instead: the two branches get different
symmetric coefficients, chosen so their group delays are half a sample apart
and both land on the same instant. It stays one FIR traversal, and both tap
sets stay symmetric, so the folding below still applies. Across five capture
windows from three sites, at 6 and 10 MS/s, this is worth 0.8-2.8% more
messages for about 1.7% more CPU on a Raspberry Pi 4.

This applies to the Airspy `U16_REAL` path only. An RTL-SDR hands over a pair
its own downconverter produced, with the branches already on the same instant,
and that path is untouched.

On ARM64/NEON builds, symmetric fixed-point FIR taps are evaluated four output
samples at a time. Opposing input samples are widened and added before their
shared tap is multiplied, which nearly halves the multiply-accumulate count.
The portable path remains in use on other architectures. The optimized and
plain kernels are covered by an exact-output equivalence test.

## Stack Integration
In order to utilize the output of Stream1090, we will send it to a decoder like readsb or dump1090-fa via TCP. This has one big advantage: You do not have to modify anything in the remaining stack.
```
   stream1090
        |
        v 
  readsb/dump1090-fa
        | 
        v
 bells and whistles
```

In the following, we will take readsb as an example. If you are using dump1090-fa, please read the readsb part first. It is just about where their settings are located/passed. 

### Readsb
Currently you probably have something like this
```
  Native device
        |
        v 
     readsb
        | 
        v
 bells and whistles
```
Detach the SDR device from readsb and let readsb connect directly to
stream1090's native TCP output. Network output runs independently of the DSP,
so a slow or disconnected client cannot block sample processing.

Readsb can ingest either AVR/raw text or Beast binary data. For testing, start
stream1090's loopback-only Beast server:

```
./build/stream1090 -s 2.4 -d ./configs/rtlsdr.ini \
    --net-bind-address 127.0.0.1 --net-beast-port 30007 --no-stdout
```

For Airspy, replace the sample rate and INI path as usual. Omit `--no-stdout`
to retain the legacy AVR stdout feed alongside TCP. Then start readsb as the
TCP client:

```
readsb --net --net-connector=127.0.0.1,30007,beast_in --interactive
```

To use AVR/raw instead:

```
./build/stream1090 -s 2.4 -d ./configs/rtlsdr.ini \
    --net-bind-address 127.0.0.1 --net-avr-port 30006 --no-stdout
readsb --net --net-connector=127.0.0.1,30006,raw_in --interactive
```

You should now see the readsb table filling up with planes.

### Readsb as a service
If you have readsb running as a service by for example using the install script. 
You may have to edit the config file ```/etc/default/readsb```. Especially when readsb has been compiled with native RTL-SDR support. So if you want readsb to not use the dongle, you have to get rid of this
```
RECEIVER_OPTIONS="--device 0 --device-type rtlsdr --gain auto --ppm 0"
```
by setting it to nothing
```
RECEIVER_OPTIONS=""
```
Make sure that `NET_OPTIONS="..."` contains the matching connector, for example
`--net --net-connector=127.0.0.1,30007,beast_in`. Do not configure readsb's
input listener on the same port: in this arrangement stream1090 is the server
and readsb is the reconnecting client.


### Dump1090-fa
If you want to use dump1090-fa instead, you have to basically follow the same steps as with readsb. However, there is a slight difference about the options. You will have to edit ```/etc/default/dump1090-fa``` and do the following:
- Detach the device from dump1090-fa 
```RECEIVER=none```
- Enable dump1090-fa to receive raw messages
```NET_RAW_INPUT_PORTS=30001```

Do not forget to reload the service to make the changes come into effect.
dump1090-fa normally exposes a raw input listener rather than connecting to an
upstream server. The legacy stdout output remains available for that deployment.


#### Disable stream1090 statistics
If you want to run stream1090 as a service, it makes sense to disable the statistics. You can do so by setting the corresponding option for cmake and rebuild the project:
```
cmake .. -DENABLE_STATS=OFF && cmake --build .
```

## Experimental Features

### ORBGRAND DF17 repair

ORBGRAND is an optional last-resort repair for damaged DF17 frames. It runs
only after the existing catalogue and erasure repair paths have failed, and it
still requires the repaired aircraft address to be trusted. The default guess
budget of 1024 bounds the random-syndrome match probability to approximately
`1024 / 2^24` per attempted frame. It is disabled by default for controlled
A/B testing:

```
cmake -S . -B build-orb-off -DENABLE_ORBGRAND=OFF
cmake -S . -B build-orb-on -DENABLE_ORBGRAND=ON -DORBGRAND_GUESSES=1024
cmake --build build-orb-off
cmake --build build-orb-on
```

The two binaries can then be swapped without reconfiguring during a live A/B.

### SIGHUP support

There is now basic experimental support for the SIGHUP signal. This signal can be send via ```kill -HUP <process id of stream1090>``` telling stream1090 to reload the device specific ini file. You can figure out the PID via ```ps```or ```pidof stream1090``` when it is running.

Clearly, there are some things you will not be able to change like serial (and sample rate which is not part of the ini anyways). The purpose is to not have to restart for adjusting gain settings. For airspy, make sure you know what you are doing when switching between manual and simple gain controls.

### Advanced RTL-SDR gain controls

If you have an RTL-SDR device and still not happy, you can push things further. Stream1090 includes the [RTL-SDR-BLOG](https://github.com/rtlsdrblog/rtl-sdr-blog) fork of librtlsdr as a Git subtree, with local adaptations documented in [thirdparty/rtl-sdr-blog-UPSTREAM.md](thirdparty/rtl-sdr-blog-UPSTREAM.md). There are two aspects here.
- This lib behaves differently in terms of results. Might be in your favour.
- If you have an R82xx tuner, this version gives you gain control over the LNA, MIX and VGA stages.

If you want to use it, there is no need to download anything nor building and such. Stream1090's CMake project will take care of it. Go to the build folder and rebuild with
```
cmake .. -DENABLE_RTLSDR_BLOG=1 && cmake --build .
``` 
Check if everything has worked out by running ```./stream1090 -h```. The native device support section should now list ```RTL-SDR Blog (advanced)```.

Regarding the manual gain control of the stages: I am not taking any responsibility here. It might work, it might not. I added these functions to the lib. There are usually good reasons that these functions are not exposed.


However, in the ini file you can now add something like
```
lna_gain = 14
mixer_gain = 13
vga_gain = 10
``` 

## FAQ & Troubleshooting
- Why is my message rate 0-2 messages per second?

  You most likely feed in the wrong format or at a wrong sampling speed.

- Why is my message rate not that good? 

  Make sure your gain setting is right and matches your setup. 

- Why are there only so few configurations in the table?
The compiler will create for each configuration a separate pipeline.

## Acknowledgments 
I would like to thank several people that provided sample data, tried it in their setups and also did some early tests with airspy.

rhodan76, wiedehopf, caius, abcd567, cnuver, jrg1956, jimmerk2. 

I would also thank Airspy and Nooelec for their support!

Thank you all very much!
