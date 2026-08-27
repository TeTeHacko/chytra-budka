# Native test harness for Chytrá Budka firmware core.
#
#   make           # build all unit tests + integration runner
#   make test      # build + run unit tests
#   make stream-to-relay  # just the integration runner
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS  ?=

CORE_DIR := ../../components/cb_core
CORE_SRC := $(CORE_DIR)/src
CORE_INC := $(CORE_DIR)/include
BUILD    := build

# libFLAC: tests/native links against the system libFLAC. The cb_core
# wrapper is gated by CB_HAVE_LIBFLAC; without it, the encoder is a stub
# (used by ESP-IDF builds before the firmware-side libFLAC component is
# vendored — see firmware/components/cb_core/README-flac.md).
FLAC_CFLAGS ?= $(shell pkg-config --cflags flac 2>/dev/null)
FLAC_LIBS   ?= $(shell pkg-config --libs flac 2>/dev/null || echo -lFLAC)
CORE_DEFS   := -DCB_HAVE_LIBFLAC

CORE_SRCS := $(CORE_SRC)/vad.cpp $(CORE_SRC)/chunked_poster.cpp \
             $(CORE_SRC)/mode_fsm.cpp $(CORE_SRC)/flac_encoder.cpp
CORE_OBJS := $(patsubst $(CORE_SRC)/%.cpp,$(BUILD)/core_%.o,$(CORE_SRCS))

UNIT_BINS := $(BUILD)/test_vad $(BUILD)/test_vad_wrap $(BUILD)/test_mode_fsm \
             $(BUILD)/test_power_mode \
             $(BUILD)/test_chunked_post $(BUILD)/test_chunked_errors \
             $(BUILD)/test_flac_encoder $(BUILD)/test_sd_layout \
             $(BUILD)/test_ble_parse $(BUILD)/test_exif \
             $(BUILD)/test_device_url $(BUILD)/test_melody \
             $(BUILD)/test_flat_json

# mbedtls-dependent tests (CSR build + SAN canonicalisation). IDF v6.0.1
# ships mbedtls 4.0.0, and the firmware (tls_enroll.c / tls_store.c) is
# written against the 4.x API (5-arg mbedtls_pk_parse_key, 3-arg
# mbedtls_x509write_csr_pem). They therefore only compile on a host whose
# system mbedtls is ALSO >= 4.x. Many distros (Arch as of 2026-05) still
# ship 3.6.x, where those symbols take extra RNG args — so we DETECT the
# host major version and only build these two tests when it's >= 4,
# otherwise skip them with a notice (rather than fail the whole suite).
FW_MAIN_DIR := ../../main
MBEDTLS_LIBS ?= -lmbedtls -lmbedx509 -lmbedcrypto
MBEDTLS_MAJOR := $(shell echo '\#include <mbedtls/build_info.h>' | $(CC) -E -dM - 2>/dev/null | sed -n 's/^\#define MBEDTLS_VERSION_MAJOR \([0-9]*\).*/\1/p')
MBEDTLS_OK := $(shell test "$(MBEDTLS_MAJOR)" -ge 4 2>/dev/null && echo yes)
ifeq ($(MBEDTLS_OK),yes)
UNIT_BINS += $(BUILD)/test_csr $(BUILD)/test_san_fp
endif
ALL_BINS  := $(UNIT_BINS) $(BUILD)/stream_to_relay

.PHONY: all test clean stream-to-relay
all: $(ALL_BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/core_%.o: $(CORE_SRC)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CORE_DEFS) $(FLAC_CFLAGS) -I$(CORE_INC) -c $< -o $@

$(BUILD)/test_vad: test_vad.cpp $(BUILD)/core_vad.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_vad_wrap: test_vad_wrap.cpp $(BUILD)/core_vad.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_mode_fsm: test_mode_fsm.cpp $(BUILD)/core_mode_fsm.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_power_mode: test_power_mode.cpp $(BUILD)/core_power_mode.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS)

# sd_layout.c is plain libc (no FATFS / ESP-IDF) — compile + test on host.
$(BUILD)/sd_layout.o: $(FW_MAIN_DIR)/sd_layout.c $(FW_MAIN_DIR)/sd_layout.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) -c $< -o $@

$(BUILD)/test_sd_layout: test_sd_layout.c $(BUILD)/sd_layout.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) $^ -o $@ $(LDFLAGS)

# secrets.h shim: config.h does `#include "secrets.h"`, which resolves to the
# operator's real (gitignored) main/secrets.h when present — quoted includes
# search the includer's directory first. On a clean clone (CI, a fresh
# contributor) that file doesn't exist, so provide the placeholder example on
# the -I path instead; the tests never read the values.
SECRETS_SHIM := $(BUILD)/secrets_shim
$(SECRETS_SHIM)/secrets.h: $(FW_MAIN_DIR)/secrets.h.example | $(BUILD)
	mkdir -p $(SECRETS_SHIM)
	cp $< $@

# device_id.c on host: esp_mac.h stubbed locally (fixed MAC), config.h pulls
# secrets.h from the firmware tree (or the shim above on a clean clone).
# -I. is BEFORE -I$(FW_MAIN_DIR) so the stub esp_mac.h wins (the firmware dir
# has none — it's an ESP-IDF header on device).
$(BUILD)/device_id.o: $(FW_MAIN_DIR)/device_id.c $(FW_MAIN_DIR)/device_id.h esp_shim.h esp_mac.h $(SECRETS_SHIM)/secrets.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I. -I$(FW_MAIN_DIR) -I$(SECRETS_SHIM) \
	      -include esp_shim.h -c $< -o $@

$(BUILD)/test_device_url: test_device_url.c $(BUILD)/device_id.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I. -I$(FW_MAIN_DIR) -I$(SECRETS_SHIM) $^ -o $@ $(LDFLAGS)

# ble_parse.c is plain libc (no NimBLE / ESP-IDF) — compile + test on host.
$(BUILD)/ble_parse.o: $(FW_MAIN_DIR)/ble_parse.c $(FW_MAIN_DIR)/ble_parse.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) -c $< -o $@

$(BUILD)/test_ble_parse: test_ble_parse.c $(BUILD)/ble_parse.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) $^ -o $@ $(LDFLAGS) -lm

# exif_read.c is plain libc (no ESP-IDF) — compile + test on host.
$(BUILD)/exif_read.o: $(FW_MAIN_DIR)/exif_read.c $(FW_MAIN_DIR)/exif_read.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) -c $< -o $@

$(BUILD)/test_exif: test_exif.c $(BUILD)/exif_read.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) $^ -o $@ $(LDFLAGS) -lm

# melody.c is plain libc + speaker.h (stdint only, no ESP-IDF) — host-testable.
$(BUILD)/melody.o: $(FW_MAIN_DIR)/melody.c $(FW_MAIN_DIR)/melody.h $(FW_MAIN_DIR)/speaker.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) -c $< -o $@

$(BUILD)/test_melody: test_melody.c $(BUILD)/melody.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) $^ -o $@ $(LDFLAGS)

# flat_json.c is plain libc — host-testable (feeds net_store's cmd/endpoint parsing).
$(BUILD)/flat_json.o: $(FW_MAIN_DIR)/flat_json.c $(FW_MAIN_DIR)/flat_json.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) -c $< -o $@

$(BUILD)/test_flat_json: test_flat_json.c $(BUILD)/flat_json.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -I$(FW_MAIN_DIR) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_chunked_post: test_chunked_post.cpp $(BUILD)/core_chunked_poster.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_chunked_errors: test_chunked_errors.cpp $(BUILD)/core_chunked_poster.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_flac_encoder: test_flac_encoder.cpp $(BUILD)/core_flac_encoder.o | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CORE_DEFS) $(FLAC_CFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS) $(FLAC_LIBS)

# tls_enroll.c is a plain C source; build it as C, not C++. Use -include
# esp_shim.h so the file's "esp_err.h" / "esp_log.h" symbols resolve.
$(BUILD)/tls_enroll.o: $(FW_MAIN_DIR)/tls_enroll.c $(FW_MAIN_DIR)/tls_enroll.h esp_shim.h $(SECRETS_SHIM)/secrets.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Werror -I$(FW_MAIN_DIR) -I. -I$(SECRETS_SHIM) \
	      -include esp_shim.h \
	      -c $< -o $@

$(BUILD)/test_csr: test_csr.c $(BUILD)/tls_enroll.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -I$(FW_MAIN_DIR) -I. -I$(SECRETS_SHIM) \
	      -include esp_shim.h \
	      $^ -o $@ $(MBEDTLS_LIBS)

# tls_store.c on host: NVS APIs stubbed (nvs_stub.c). The native test
# only exercises the canonicalisation helper which doesn't touch NVS.
$(BUILD)/tls_store.o: $(FW_MAIN_DIR)/tls_store.c $(FW_MAIN_DIR)/tls_store.h esp_shim.h nvs.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -Wextra -Werror -I$(FW_MAIN_DIR) -I. \
	      -include esp_shim.h \
	      -c $< -o $@

$(BUILD)/nvs_stub.o: nvs_stub.c nvs.h | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -I$(FW_MAIN_DIR) -I. -c $< -o $@

$(BUILD)/test_san_fp: test_san_fp.c $(BUILD)/tls_store.o $(BUILD)/nvs_stub.o | $(BUILD)
	$(CC) -std=c17 -O2 -Wall -I$(FW_MAIN_DIR) -I. \
	      -include esp_shim.h \
	      $^ -o $@ $(MBEDTLS_LIBS)

$(BUILD)/transport_posix.o: transport_posix.cpp transport_posix.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) -c $< -o $@

$(BUILD)/stream_to_relay: stream_to_relay.cpp $(BUILD)/transport_posix.o $(CORE_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CORE_DEFS) $(FLAC_CFLAGS) -I$(CORE_INC) $^ -o $@ $(LDFLAGS) -lpthread $(FLAC_LIBS)

stream-to-relay: $(BUILD)/stream_to_relay

# Grep-gate against the pdMS_TO_TICKS(<10ms) → 0-ticks footgun. At
# CONFIG_FREERTOS_HZ=100 a sub-10ms pdMS_TO_TICKS rounds to 0, and
# vTaskDelay(0) does not yield → busy-spin → task_wdt (the v0.4.4 crash).
# All short waits must go through cb_delay_ms() (cb_time.h), which floors at
# one tick. A bare single-digit-ms vTaskDelay(pdMS_TO_TICKS(N)) is forbidden.
MAIN_DIR := $(CURDIR)/../../main
.PHONY: lint-delays
lint-delays:
	@echo "== lint: no vTaskDelay(pdMS_TO_TICKS(<10ms)) (use cb_delay_ms) =="
	@if grep -rnE --exclude=cb_time.h 'vTaskDelay\(pdMS_TO_TICKS\([0-9]\)' $(MAIN_DIR) ; then \
	  echo "ERROR: sub-tick vTaskDelay above rounds to 0 ticks at HZ=100 and"; \
	  echo "       busy-spins instead of yielding. Replace with cb_delay_ms()."; \
	  exit 1; \
	fi
	@echo "  ok — no bare sub-tick vTaskDelay"

test: lint-delays $(UNIT_BINS)
	@set -e; for b in $(UNIT_BINS); do echo "== $$b =="; $$b; done
	@if [ "$(MBEDTLS_OK)" != "yes" ]; then \
	  echo "NOTE: test_csr + test_san_fp SKIPPED — host mbedtls major='$(MBEDTLS_MAJOR)' < 4;"; \
	  echo "      firmware uses the mbedtls 4.x API (IDF v6.0.1). Install mbedtls >= 4 to run them."; \
	fi
	@echo "ALL UNIT TESTS PASSED"

clean:
	rm -rf $(BUILD)
