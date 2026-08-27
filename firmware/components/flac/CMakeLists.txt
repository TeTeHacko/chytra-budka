# Vendored libFLAC 1.5.0 (encoder-only) as an ESP-IDF component.
# See ../cb_core/README-flac.md for rationale. cb_core auto-detects this file's
# existence and defines CB_HAVE_LIBFLAC, so the real cb::FlacEncoder is built.
#
# Sources are the encoder-only subset: no decoder, no Ogg, no SIMD intrinsics
# (FLAC__NO_ASM keeps the portable C paths — the S3 doesn't share libFLAC's
# x86/ARM intrinsic implementations).
# Upstream is nested INSIDE this component dir (not a sibling under
# components/) so ESP-IDF's component scanner — which treats every direct child
# of components/ as a component — doesn't try to process libFLAC's own root
# CMakeLists (a full project()), which fails with "project command is not
# scriptable". Only components/flac/CMakeLists.txt (this file) is an IDF
# component; flac-upstream/ is just source files it points at.
set(LIBFLAC_DIR "${CMAKE_CURRENT_LIST_DIR}/flac-upstream")

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
    # The encoder unconditionally references the decoder (its optional
    # encode-then-verify path), so stream_decoder.c must be linked even though
    # we run with verify off. ~80 KB flash; trivial against a 3.5 MB OTA slot.
    "${LIBFLAC_DIR}/src/libFLAC/stream_decoder.c"
    "${LIBFLAC_DIR}/src/libFLAC/window.c"
)

idf_component_register(
    SRCS ${FLAC_SRCS}
    INCLUDE_DIRS "${LIBFLAC_DIR}/include"
    # ${CMAKE_CURRENT_LIST_DIR} holds our hand-written config.h — every libFLAC
    # .c does `#ifdef HAVE_CONFIG_H #include <config.h>`, so it must be on the
    # private include path.
    PRIV_INCLUDE_DIRS "${LIBFLAC_DIR}/src/libFLAC/include" "${CMAKE_CURRENT_LIST_DIR}"
)

# HAVE_CONFIG_H=1 turns ON the `#include <config.h>` (our config.h supplies
# endianness, HAVE_*, SIZEOF_*, PACKAGE_VERSION, OGG/INTRIN=0, etc.). The
# remaining flags are libFLAC build switches not carried in config.h:
# integer-only off (we have an FPU path), no DLL exports, no SIMD/asm.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H=1
    FLAC__NO_DLL=1
    FLAC__INTEGER_ONLY_LIBRARY=0
    FLAC__CPU_UNKNOWN=1
    FLAC__NO_ASM=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    # Vendored third-party (libFLAC 1.5.0) under a very new GCC (15.2.0) + the
    # project's global -Werror. libFLAC compiles cleanly on its own build but
    # GCC 15 promotes things like -Wincompatible-pointer-types to errors on
    # known-benign typedef/signedness mismatches. We don't patch upstream, so
    # drop -Werror for this component (warnings still print) + quiet the noisy
    # ones. The encoder path itself is unchanged, well-exercised code.
    -Wno-error
    -Wno-incompatible-pointer-types
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-shadow
    -Wno-type-limits
    -Wno-unused-but-set-variable
)
