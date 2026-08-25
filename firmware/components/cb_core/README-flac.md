# Vendoring libFLAC into the firmware build

> **STATUS: DONE.** libFLAC 1.5.0 is vendored at
> `firmware/components/flac/flac-upstream/` (trimmed to `src/libFLAC/` +
> `include/` + licences) with the IDF wrapper in
> `firmware/components/flac/CMakeLists.txt` and a hand-written
> `firmware/components/flac/config.h`. `CB_HAVE_LIBFLAC` is auto-defined, the
> real encoder builds (bench, ~150 KB flash). **Four corrections to the
> original recipe below — it was written as a plan and was wrong on each:**
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

## How to vendor (one-time setup, do this when bench HW arrives)

```bash
cd firmware/components
git clone --depth 1 --branch 1.5.0 https://github.com/xiph/flac.git flac-upstream
mkdir flac && cd flac
```

Then write a thin `CMakeLists.txt` that registers selected libFLAC sources
as an IDF component (libFLAC itself uses autoconf/CMake configured for
hosted builds; we cherry-pick the encoder sources we need):

```cmake
# firmware/components/flac/CMakeLists.txt
set(LIBFLAC_DIR "${CMAKE_CURRENT_LIST_DIR}/../flac-upstream")

set(FLAC_SRCS
    "${LIBFLAC_DIR}/src/libFLAC/bitmath.c"
    "${LIBFLAC_DIR}/src/libFLAC/bitreader.c"
    "${LIBFLAC_DIR}/src/libFLAC/bitwriter.c"
    "${LIBFLAC_DIR}/src/libFLAC/cpu.c"
    "${LIBFLAC_DIR}/src/libFLAC/crc.c"
    "${LIBFLAC_DIR}/src/libFLAC/fixed.c"
    "${LIBFLAC_DIR}/src/libFLAC/float.c"
    "${LIBFLAC_DIR}/src/libFLAC/format.c"
    "${LIBFLAC_DIR}/src/libFLAC/lpc.c"
    "${LIBFLAC_DIR}/src/libFLAC/md5.c"
    "${LIBFLAC_DIR}/src/libFLAC/memory.c"
    "${LIBFLAC_DIR}/src/libFLAC/metadata_iterators.c"
    "${LIBFLAC_DIR}/src/libFLAC/metadata_object.c"
    "${LIBFLAC_DIR}/src/libFLAC/stream_encoder.c"
    "${LIBFLAC_DIR}/src/libFLAC/stream_encoder_framing.c"
    "${LIBFLAC_DIR}/src/libFLAC/window.c"
)

idf_component_register(
    SRCS ${FLAC_SRCS}
    INCLUDE_DIRS "${LIBFLAC_DIR}/include" "${LIBFLAC_DIR}/src/libFLAC/include"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H=0
    PACKAGE_VERSION="1.5.0"
    FLAC__HAS_OGG=0
    FLAC__NO_DLL=1
    FLAC__INTEGER_ONLY_LIBRARY=0
    FLAC__CPU_UNKNOWN=1
    FLAC__NO_ASM=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-shadow
)
```

Notes / gotchas:

- **No OGG**: `FLAC__HAS_OGG=0` skips libogg. Native FLAC framing is what
  the firmware uses anyway.
- **No CPU intrinsics**: `FLAC__NO_ASM=1` + `FLAC__CPU_UNKNOWN=1` keeps
  it portable. ESP32-S3 doesn't share libFLAC's x86/ARM intrinsic paths.
- **Decoder sources omitted**: encoder-only build saves ~80 KB flash.
  If you later want on-device verification or OTA-side FLAC ingest, add
  `stream_decoder.c` + `bitreader.c` (already listed).
- **Memory**: at the configured `blocksize=4096` and `compression_level=5`,
  the encoder allocates roughly 30–40 KB working memory. Allocate from
  PSRAM if internal heap is tight — libFLAC uses `malloc()` so a custom
  allocator wrapper isn't required, but consider `heap_caps_malloc_extmem_enable`
  before instantiating the encoder.
- **First-run sanity check**: build firmware, watch for
  `cb_core: libFLAC component present → real encoder` in the configure
  output, and `[audio] codec=flac (libFLAC 1.5.0)` at boot.

## Without vendoring

The firmware still builds. `flac_enabled` MQTT switch will toggle, but
`audio.cpp` logs `flac requested but encoder is a stub, staying on PCM`
and continues to send `audio/L16`. Native tests are unaffected — they use
the system libFLAC.
