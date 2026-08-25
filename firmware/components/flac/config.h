/* Minimal libFLAC config.h for the ESP32-S3 (Xtensa LX7, little-endian, GCC
 * newlib). Hand-written from flac-upstream/config.cmake.h.in — the IDF build
 * doesn't run libFLAC's autotools/CMake configure step, but every libFLAC .c
 * does `#ifdef HAVE_CONFIG_H #include <config.h>`, so we provide one. Scope:
 * encoder-only, no Ogg, no SIMD intrinsics (the portable C paths). */
#ifndef CB_FLAC_CONFIG_H
#define CB_FLAC_CONFIG_H

/* Byte order. Xtensa LX7 is little-endian. The template does
 * `#define WORDS_BIGENDIAN CPU_IS_BIG_ENDIAN`, so both follow from this. */
#define CPU_IS_BIG_ENDIAN    0
#define CPU_IS_LITTLE_ENDIAN 1
#define WORDS_BIGENDIAN      0

/* 32-bit core → keep 32-bit bitreader/bitwriter words (64-bit words are an
 * optimisation for 64-bit hosts). Align libFLAC's working buffers. */
#define ENABLE_64_BIT_WORDS  0
#define FLAC__ALIGN_MALLOC_DATA

/* No SIMD, no Ogg — matches the FLAC__NO_ASM / encoder-only component build. */
#define FLAC__HAS_X86INTRIN    0
#define FLAC__HAS_NEONINTRIN   0
#define FLAC__HAS_A64NEONINTRIN 0
#define OGG_FOUND              0
#define FLAC__HAS_OGG          OGG_FOUND

/* Toolchain features present in the ESP-IDF GCC + newlib environment. */
#define HAVE_LROUND     1
#define HAVE_STDINT_H   1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H   1
#define HAVE_STRING_H   1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TYPEOF     1
/* HAVE_BSWAP16 / HAVE_BSWAP32 / HAVE_BYTESWAP_H intentionally NOT defined:
 * newlib has no <byteswap.h>, so libFLAC's endswap.h uses portable shifts. */

#define SIZEOF_VOIDP 4
#define SIZEOF_OFF_T 4

#define PACKAGE_VERSION "1.5.0"

#endif /* CB_FLAC_CONFIG_H */
