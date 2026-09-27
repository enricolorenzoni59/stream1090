# Fresh Raspberry Pi: RSP1B, stream1090, readsb and a map

Use Raspberry Pi OS Lite **64-bit**, with SSH and networking configured.
These instructions target a fresh system, a Pi 3 or newer, and an RSP1B with
a 1090 MHz antenna. The branch is experimental. The reference Pi 3 showed
undervoltage events, so use a suitable power supply and cable.

The signal path is:

```text
RSP1B → stream1090 → Beast TCP 127.0.0.1:30007 → readsb → tar1090 web map
```

Only stream1090 opens the radio. readsb receives already demodulated messages;
this setup does not run its IQ demodulator or require IQ resampling for readsb.

## 1. Install build tools

```sh
sudo apt update
sudo apt install -y git build-essential cmake pkg-config \
  libusb-1.0-0-dev curl wget ca-certificates python3
uname -m
```

`uname -m` should print `aarch64` for these 64-bit instructions.

## 2. Install the SDRplay API

Download the Linux Hardware API from the
[official SDRplay page](https://sdrplay.com/hardware-api/).
Use API 3.15, which was used for our tests, or compatible newer headers/runtime.
Copy its `.run` installer to the Pi. In the directory containing that file,
run it and follow its license and installation prompts. For example, for the
3.15.2 installer:

```sh
sudo sh ./SDRplay_RSP_API-Linux-3.15.2.run
sudo ldconfig
sudo systemctl enable --now sdrplay.service
systemctl is-active sdrplay.service
```

Use the actual downloaded filename if it differs. The service should report
`active`. The installer must install the library, headers, service and USB
rules. Reconnect the RSP1B after installation. No SDRconnect application or
SoapySDR layer is needed for this backend.

## 3. Build stream1090

```sh
cd ~
git clone --branch feature/sdrplay-rsp1b --single-branch \
  https://github.com/enricolorenzoni59/stream1090.git
cd ~/stream1090
cmake -S . -B build \
  -DENABLE_SDRPLAY=ON \
  -DENABLE_RTLSDR_BLOG=OFF \
  -DENABLE_NATIVE_ARCH=OFF \
  -DSTREAM1090_CPU=cortex-a53 \
  -DENABLE_LTO=OFF \
  -DBUILD_TESTING=ON
cmake --build build -j1
ctest --test-dir build --output-on-failure
sudo install -m 755 build/stream1090 /usr/local/bin/stream1090
```

The Cortex-A53 target is suitable for the Pi 3 and runs on newer 64-bit Pis.
Build with `-j1` on a 1 GB Pi to limit compiler memory use. Wait for each command
to succeed before proceeding. SDRplay is already ON by default in this branch;
the explicit flag also overrides an older CMake cache. Keep statistics enabled
for the Prometheus decoder counters.

## 4. Install readsb and tar1090

Use the [readsb maintainer's installer](https://github.com/wiedehopf/adsb-scripts/wiki/Automatic-installation-for-readsb),
which also installs the web map on a fresh system:

```sh
cd ~
curl -fL https://raw.githubusercontent.com/wiedehopf/adsb-scripts/master/readsb-install.sh \
  -o readsb-install.sh
sudo bash ./readsb-install.sh
```

Configure readsb to receive stream1090's Beast stream instead of opening an
RTL-SDR. Preserve the other settings and the installer's normal output ports:

```sh
sudo cp -n /etc/default/readsb /etc/default/readsb.before-stream1090
sudo sed -i 's|^RECEIVER_OPTIONS=.*|RECEIVER_OPTIONS="--net-only --net-connector=127.0.0.1,30007,beast_in"|' /etc/default/readsb
sudo systemctl restart readsb
```

For the map centre, set your actual receiver location with
`sudo readsb-set-location LATITUDE LONGITUDE`, replacing both placeholders
with decimal coordinates. Do not use `readsb-gain` to adjust the RSP1B: gain
belongs to stream1090 in this setup.

These installer instructions are for a fresh OS: the installer also changes
existing decoder/feeder configurations when they are present.

## 5. Start receiving

Run this in your SSH terminal:

```sh
stream1090 --device sdrplay -s 4 -u 8 \
  --sdrplay-if-gr 25 --sdrplay-lna-state 2 \
  --sdrplay-adsb-mode 1 --sdrplay-usb-mode bulk \
  --net-bind-address 127.0.0.1 --net-beast-port 30007 --no-stdout \
  --metrics 127.0.0.1:9109
```

Leave it running. From a browser on the same network, open
`http://PI_IP_ADDRESS/tar1090/`, replacing `PI_IP_ADDRESS` with the Pi's IP.
readsb reconnects automatically; allow several seconds for connection and traffic.
The terminal shows logs rather than raw frames because output goes over TCP.
Ctrl-C stops stream1090; readsb can stay running while waiting for it.

4→8 MS/s and GR25/LNA2 are our site's tested starting settings. Gain depends
on antenna and RF environment. If overloads occur, increase IF gain reduction
(for example, from 25 to 40). Larger reduction means less gain. Other inputs
and settings are described in the [full RSP1B guide](sdrplay-rsp1b.md).

## 6. Check reception and monitoring

From a second SSH terminal:

```sh
systemctl is-active sdrplay.service readsb.service
curl -fsS http://127.0.0.1:9109/readyz
curl -fsS http://127.0.0.1:9109/metrics
journalctl -u readsb -n 30 --no-pager
vcgencmd get_throttled
```

Look for `ready`, `stream1090_device_up 1`, increasing message counters and
aircraft in the map. The metrics also expose gap/reset/overload counters.
This starts an exporter, not a Prometheus server. For an existing central
Prometheus, change the metrics bind to the Pi's reachable LAN/VPN address on
port 9109 and scrape that address. The default command above binds locally.

If the radio does not open, inspect `journalctl -u sdrplay -n 50 --no-pager`,
check the antenna/USB connection and make sure another application is not using
the RSP1B. If stream1090 receives frames but the map stays empty, inspect readsb's
log and `/etc/default/readsb`; the input connector must point to port **30007**.

## Optional: start stream1090 automatically

After the foreground test works, stop it with Ctrl-C. The following command
creates a service running as your current login user; there is no assumption
that the account is named `pi`:

```sh
sudo tee /etc/systemd/system/stream1090-rsp1b.service >/dev/null <<EOF
[Unit]
Description=stream1090 RSP1B receiver
Wants=sdrplay.service
After=sdrplay.service network.target

[Service]
User=$(id -un)
ExecStart=/usr/local/bin/stream1090 --device sdrplay -s 4 -u 8 --sdrplay-if-gr 25 --sdrplay-lna-state 2 --sdrplay-adsb-mode 1 --sdrplay-usb-mode bulk --net-bind-address 127.0.0.1 --net-beast-port 30007 --no-stdout --metrics 127.0.0.1:9109
Restart=on-failure
RestartSec=5
KillSignal=SIGINT
TimeoutStopSec=20

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now stream1090-rsp1b.service
systemctl status stream1090-rsp1b.service --no-pager
```

Use `journalctl -u stream1090-rsp1b -f` for logs and
`sudo systemctl stop stream1090-rsp1b` before running another receiver/capture.
To disable automatic startup: `sudo systemctl disable --now stream1090-rsp1b`.

The guide's commands were checked against the source and existing Pi setup;
the entire installation has not been repeated on a freshly flashed SD card.
The branch still has unresolved acquisition gaps and physical USB reconnect
recovery is not qualified. Include OS, API version, commit, gain, gap counts
and power-supply observations when reporting community results.
