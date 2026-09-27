# Testing RSPduo and RSPdx

The native SDRplay backend supports RSP1B, RSPduo **single tuner**, and RSPdx
at 1090 MHz through the official API 3.15. RSP1B has been tested on a Pi 3;
RSPduo/RSPdx have automated API configuration tests but still need hardware
validation. Other models (including RSPdxR2) are not enabled by this change.

| Model | Input selection | LNA state at 1090 MHz | Bias-T |
| --- | --- | --- | --- |
| RSP1B | Fixed input | 0–8 | Available |
| RSPduo | `--sdrplay-tuner 1` (default) or `2`, 50-ohm SMA input | 0–8 | Only tuner 2 |
| RSPdx | `--sdrplay-antenna A` (default) or `B` | 0–18 | Only antenna B |

Bias-T is off unless `--bias-tee` is supplied. Unsupported combinations are
rejected before starting reception. RSPdx antenna C is not supported at
1090 MHz; HDR is disabled. RSPduo dual-tuner, master/slave operation and
sharing the device with another application are not supported. Close other
applications using the receiver before testing. Only one selected IQ stream
is decoded, even on the duo.

The LNA index is not a gain in dB and the same index can mean different
attenuation on different models. IF gain reduction remains 20–59 dB;
automatic gain control is not implemented. Tune gain for your own antenna,
amplifier and local signal conditions.

## Build or update

Install the vendor API, library and headers as described in the
[setup guide](sdrplay-pi-quickstart.md). In your existing checkout:

```sh
git switch feature/sdrplay-rsp1b
git pull --ff-only
cmake -S . -B build -DENABLE_SDRPLAY=ON
cmake --build build -j1
```

These commands build the local binary; if a service uses an installed copy,
install the new binary and restart that service as described in the setup guide.
Use `--serial SERIAL` when more than one supported receiver is connected.

## First reception test

For RSPduo, connect the antenna to tuner 1's 50-ohm SMA port:

```sh
./build/stream1090 --device sdrplay --sdrplay-tuner 1 \
  -s 4 -u 8 --sdrplay-if-gr 40 --sdrplay-lna-state 2 \
  --sdrplay-usb-mode bulk --metrics 127.0.0.1:9109
```

Use `--sdrplay-tuner 2` for tuner 2. For RSPdx, connect to antenna A:

```sh
./build/stream1090 --device sdrplay --sdrplay-antenna A \
  -s 4 -u 8 --sdrplay-if-gr 40 --sdrplay-lna-state 2 \
  --sdrplay-usb-mode bulk --metrics 127.0.0.1:9109
```

Use `--sdrplay-antenna B` for antenna B. The commands print decoded AVR
messages to stdout and startup diagnostics to stderr. Gain values are starting
points, not optimized values for these untested models.

To feed the readsb configuration from the setup guide, append:

```sh
--net-bind-address 127.0.0.1 --net-beast-port 30007 --no-stdout
```

Stop any existing stream1090 service first so it does not hold the receiver
or the same TCP ports. Confirm the startup log names the correct model and
input, then check `http://127.0.0.1:9109/readyz` and `/metrics`.
The [IQ telemetry guide](sdrplay-telemetry.md) explains the signal metrics;
zero sampled clipping does not prove absence of analog overload.

Please report the model, host/OS, API version, commit, complete command,
startup log, gain settings, received message counts, gap/recovery counts and
whether stopping and starting again works. Test both inputs separately if
possible. Share results in the
[community discussion](https://github.com/mgrone/stream1090/discussions/85).

## Implementation reference

Model parameter blocks and 1090 MHz LNA limits follow the
[SDRplay API specification](https://sdrplay.com/hardware-api/).
The [vendor single-tuner example](https://github.com/SDRplay/examples/blob/master/sdrplay_api_example.c)
selects the appropriate A/B parameter block but receives single-tuner samples
through Stream A; Stream B is reserved for dual-tuner operation. The backend
rejects an unexpected second stream instead of mixing it into the selected IQ.

## Continuous integration

The `build` GitHub Actions workflow runs on pushes to `feature/sdrplay-rsp1b`,
`enrico-dev` and `main`, on pull requests, and by manual dispatch. Its SDRplay
matrix builds the complete executable with **`-Werror`**, GCC and Clang, on
Linux x86-64 and ARM64, with metrics both enabled and disabled.

These jobs download the official Linux API 3.15 installer (v2) from SDRplay,
verify a pinned SHA-256, and extract the real headers and shared library into
the temporary runner directory. They verify dynamic linkage and executable
startup, then run CTest. The SDK is not committed, cached or uploaded as an
artifact; its vendor licence applies. A changed vendor download deliberately
fails checksum verification and requires review before updating the pin.

Other jobs explicitly disable SDRplay and retain the hardware-free backend
mock tests. Neither those tests nor real-SDK compilation validate reception,
USB behaviour or model-specific hardware: RSPduo/RSPdx still need live tests.
