# RTL-SDR Blog subtree

`thirdparty/rtl-sdr-blog` is maintained as a Git subtree of:

```text
https://github.com/rtlsdrblog/rtl-sdr-blog.git
```

The subtree was initialized from upstream commit:

```text
aed0ea19f3a273370a13c9009b96313c75d54c7b
```

Stream1090 keeps its CMake integration and R82xx per-stage gain controls as
normal commits on top of that import. Do not replace the directory with a
manually copied checkout.

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
