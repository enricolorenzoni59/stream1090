# RTL-SDR Blog subtree

`thirdparty/rtl-sdr-blog` is maintained as a Git subtree of:

```text
https://github.com/rtlsdrblog/rtl-sdr-blog.git
```

The subtree was initialized from upstream commit:

```text
aed0ea19f3a273370a13c9009b96313c75d54c7b
```

Stream1090 keeps its CMake integration, macOS libusb link fix, R82xx
per-stage gain controls, and a logging shim
(`rtlsdr_set_log_callback` in `include/rtl-sdr.h`, used by
`src/rtlsdr_log.h`) as normal commits on top of that import. The shim rewrites
the library's `fprintf(stderr, ...)` sites so a host can route them through its
own logger; with no callback installed they still go to stderr. The import also
marks the device as lost when `libusb_handle_events` reports
`LIBUSB_ERROR_NO_DEVICE`/`LIBUSB_ERROR_NOT_FOUND`, so `rtlsdr_close` skips the
tuner deinit (and its failing register writes) on a device that has already been
unplugged. Do not replace
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
