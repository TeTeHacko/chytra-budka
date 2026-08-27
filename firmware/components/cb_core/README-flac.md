# Vendoring libFLAC into the firmware build

> **STATUS: DONE.** libFLAC 1.5.0 is vendored at
> `firmware/components/flac/flac-upstream/` (trimmed to `src/libFLAC/` +
> `include/` + licences) with the IDF wrapper in
> `firmware/components/flac/CMakeLists.txt` and a hand-written
> `firmware/components/flac/config.h`. `CB_HAVE_LIBFLAC` is auto-defined, the
> real encoder builds (bench, ~150 KB flash). **Four gotchas to know if you
> ever re-vendor or upgrade libFLAC as an IDF component:**
> 1. **Don't put `flac-upstream` directly under `components/`** — IDF treats
>    every direct child of `components/` as a component and chokes on libFLAC's
>    own root `CMakeLists.txt` (`project command is not scriptable`). Nest it
>    *inside* the component: `components/flac/flac-upstream/`.
> 2. **`HAVE_CONFIG_H=0` is a trap** — every libFLAC `.c` does
>    `#ifdef HAVE_CONFIG_H #include <config.h>`, and `#ifdef` tests *defined*,
>    not the value, so `=0` still pulls a (missing) `config.h`. Provide a real
>    `config.h` (see `components/flac/config.h`) and set `HAVE_CONFIG_H=1`.
> 3. **`stream_decoder.c` is required** — the encoder unconditionally links the
>    decoder for its optional encode-then-verify path, so an "encoder-only"
>    source list fails to link. Include `stream_decoder.c`.
> 4. **`-Wno-error` on the flac component** — GCC 15 promotes benign upstream
>    warnings (e.g. `-Wincompatible-pointer-types`) to errors under the
>    project's global `-Werror`; we don't patch upstream.

`cb::FlacEncoder` (in `cb_core`) wraps libFLAC's `stream_encoder` API. On
the **host** (`firmware/tests/native`) we link against the system libFLAC
package (`pacman -S flac` on Arch). On the **ESP32-S3 target** there is
no upstream-managed component for libFLAC in the ESP Component Registry,
so it is vendored manually (above).

`cb_core/CMakeLists.txt` looks for `firmware/components/flac/CMakeLists.txt`
at configure time:

- Present → defines `CB_HAVE_LIBFLAC`, adds `flac` to `REQUIRES`, and the
  real encoder is built.
- Absent → `FlacEncoder` builds as a stub. `audio.cpp` falls back to PCM
  when the runtime config tries to enable FLAC, with a `WARN` log line.


## Without vendoring

The firmware still builds. `flac_enabled` MQTT switch will toggle, but
`audio.cpp` logs `flac requested but encoder is a stub, staying on PCM`
and continues to send `audio/L16`. Native tests are unaffected — they use
the system libFLAC.
