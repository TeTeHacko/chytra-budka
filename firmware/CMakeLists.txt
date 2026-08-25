# Top-level ESP-IDF project for Chytrá Budka firmware.
#
# Build:
#   . ~/.espressif/v6.0.1/esp-idf/export.sh    # PRODUCTION toolchain (mbedtls 4.0.0)
#   idf.py set-target esp32s3
#   idf.py build
# NOTE: do NOT source ~/esp/esp-idf/export.sh — that legacy v5.5 checkout is
# gone (mbedtls 3.6.6 had a TLS 1.3 server-side OOM bug; see firmware/HTTPS.md).
#
# Linux host target (no WiFi/I²S; runs FSM/MQTT against host):
#   idf.py --preview set-target linux
#   idf.py build
#
# Native unit tests (cb_core, no IDF needed):
#   make -C tests/native test
cmake_minimum_required(VERSION 3.16)

# Path to v6.0.1 IDF SDK — eim-managed.
set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(chytra-budka)

# Bridge for an upstream ESP-IDF bug (PR: https://github.com/espressif/esp-idf/pull/18668):
# components/lwip/port/include/lwip_default_hooks.h uses `struct dhcp` in the
# lwip_dhcp_on_extra_option() prototype *before* it is forward-declared
# (the `struct dhcp;` decl is stranded lower down, inside #ifdef
# CONFIG_LWIP_IPV4). With CONFIG_LWIP_HOOK_DHCP_EXTRA_OPTION_CUSTOM=y — which
# we need (wifi_mgr.c implements the hook for DHCP domain discovery) — every
# lwip TU including that header fails with:
#   error: 'struct dhcp' declared inside parameter list ... [-Werror]
# The diagnostic has NO named -W sub-flag, so it cannot be waived with any
# -Wno-error=<name>. Instead, force <lwip/dhcp.h> ahead of the hooks header
# in the lwip component build so `struct dhcp` is already complete. This
# touches no SDK file (survives an IDF re-install) and is applied after
# project() once the __idf_lwip target exists. Remove once a fixed IDF
# release is in use.
target_compile_options(__idf_lwip PRIVATE -include lwip/dhcp.h)
