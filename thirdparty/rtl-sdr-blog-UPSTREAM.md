# RTL-SDR Blog subtree

`thirdparty/rtl-sdr-blog` is maintained as a Git subtree of:

```text
https://github.com/rtlsdrblog/rtl-sdr-blog.git
```

The subtree was initialized from upstream commit:

```text
aed0ea19f3a273370a13c9009b96313c75d54c7b
```

Stream1090 keeps these changes as normal commits on top of that import:

| Change | Upstream |
|---|---|
| CMake integration for building inside stream1090 (version and config-package steps disabled, include paths) | local only |
| macOS libusb link fix (`target_link_directories`) and the `rtlsdr_static` target linking libusb | rtlsdrblog/rtl-sdr-blog#80 (same fix via the pkg-config imported target) |
| R82xx per-stage gain by register index: `rtlsdr_r82xx_set_lna_gain/_mixer_gain/_vga_gain` | rtlsdrblog/rtl-sdr-blog#79 proposes a different API (dB values, stage enum); stream1090 keeps the index form the adaptive gain loop needs |
| Logging shim: `rtlsdr_set_log_callback` in `include/rtl-sdr.h`, `src/rtlsdr_log.h`; every `fprintf(stderr, ...)` goes through it, and still to stderr with no callback installed | local only |
| `rtlsdr_mark_dev_lost()`, used by stream1090's sample watchdog | local only |
| `rtlsdr_close()` asks the device once and skips the deinit when it is gone | rtlsdrblog/rtl-sdr-blog#81 |
| `rtlsdr_read_async()` asks the device after two event-loop wakeups without a completed transfer and ends the stream when it is gone (macOS never reports an unplug otherwise) | rtlsdrblog/rtl-sdr-blog#83 |
| `rtl_test -p` on macOS prints the `CLOCK_MONOTONIC_RAW` estimate beside the wall-clock one (a diagnostic) | rtlsdrblog/rtl-sdr-blog#82 proposes the fix instead: `CLOCK_MONOTONIC` |

With no log callback installed, messages still go to stderr. Do not replace
the directory with a manually copied checkout.

To update from the upstream `master` branch, start with a clean working tree
and run:

```sh
git subtree pull \
  --prefix=thirdparty/rtl-sdr-blog \
  https://github.com/rtlsdrblog/rtl-sdr-blog.git master \
  --squash
```

Resolve any conflicts in the local integration, then build both the normal
system-library configuration and the vendored configuration before committing
the update.
