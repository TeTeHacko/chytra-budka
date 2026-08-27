# cb_core — framework-agnostic C++17 core (no Arduino, no ESP-IDF deps).
#
# Provides: cb::Vad, cb::ChunkedPoster, cb::Mode FSM, cb::Transport
# interface, cb::FlacEncoder.
# Compiles identically for Xtensa firmware target and Linux host (tests/native).
#
# FLAC encoder is gated behind CB_HAVE_LIBFLAC. The wrapper compiles in
# either mode; without libFLAC the methods are no-op stubs so the rest of
# the firmware can be codec-agnostic.
#   * Native tests (firmware/tests/native): linked against system libFLAC,
#     CB_HAVE_LIBFLAC defined in the test Makefile.
#   * ESP-IDF target: requires a "flac" component to be present in the
#     idf component path. See firmware/components/cb_core/README-flac.md
#     for vendor-in instructions. Once vendored, CB_HAVE_LIBFLAC is auto-
#     defined here and `flac` is added to REQUIRES.

set(CB_CORE_SRCS
    "src/vad.cpp"
    "src/chunked_poster.cpp"
    "src/mode_fsm.cpp"
    "src/power_mode.cpp"
    "src/flac_encoder.cpp"
)

# Detect whether a "flac" IDF component is reachable (vendored in
# components/flac or registry-pulled). idf_build_get_property exposes the
# component list after the registration phase, so we use the simpler
# heuristic: existence of components/flac/CMakeLists.txt at config time.
set(_cb_have_flac OFF)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../flac/CMakeLists.txt")
    set(_cb_have_flac ON)
endif()

set(_cb_requires "")
if(_cb_have_flac)
    list(APPEND _cb_requires flac)
endif()

idf_component_register(
    SRCS ${CB_CORE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${_cb_requires}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic)

if(_cb_have_flac)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CB_HAVE_LIBFLAC=1)
    message(STATUS "cb_core: libFLAC component present → real encoder")
else()
    message(STATUS "cb_core: no libFLAC component → FlacEncoder is a stub "
                   "(see README-flac.md to enable)")
endif()
