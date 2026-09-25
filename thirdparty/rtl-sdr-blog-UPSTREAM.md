# RTL-SDR Blog subtree

`thirdparty/rtl-sdr-blog` is a Git subtree of the `enrico-dev` branch of

```text
https://github.com/enricolorenzoni59/rtl-sdr-blog.git
```

That branch is [rtlsdrblog/rtl-sdr-blog](https://github.com/rtlsdrblog/rtl-sdr-blog)
`master` plus the fixes proposed upstream (rtlsdrblog/rtl-sdr-blog#79 to #83)
and what stream1090 needs on top: the per-stage gain setters by register index
(`rtlsdr_r82xx_set_lna_gain/_mixer_gain/_vga_gain`), the log callback
(`rtlsdr_set_log_callback`), `rtlsdr_mark_dev_lost()`, and a CMake build that
works as a subproject. Its README lists every change, and its CI builds it on
Linux (GCC, Clang) and macOS with `-Werror`, standalone and as a subproject.

**This directory carries no local changes.** Make library changes in the fork,
let its CI pass, then pull them here. The directory must stay identical to the
fork's tree:

```sh
git subtree pull \
  --prefix=thirdparty/rtl-sdr-blog \
  https://github.com/enricolorenzoni59/rtl-sdr-blog.git enrico-dev \
  --squash

# must print nothing
git diff HEAD:thirdparty/rtl-sdr-blog "$(git ls-remote https://github.com/enricolorenzoni59/rtl-sdr-blog.git enrico-dev | cut -f1)^{tree}" --stat
```

The last command needs the fork's commit locally, which the pull fetches. Do not
replace the directory with a manually copied checkout.
