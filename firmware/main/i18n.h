/* i18n.h — runtime-switchable UI translations for the local web pages.
 *
 * Single source of truth: an enum of string IDs (i18n_str_t) and a 2D table
 * MSG[lang][id] in flash (.rodata). Handlers call tr(id) (current language) or
 * tr_l(lang, id) (explicit, for the /config ?lang= preview). A NULL cell falls
 * back to English; i18n_init() logs any NULL cell at boot so a missing
 * translation is caught on the first dev boot.
 *
 * FORMAT-STRING CONTRACT (important): some IDs are printf templates whose name
 * ends in _FMT. ESP picolibc's default IO does NOT support positional
 * specifiers (%1$s), so every language's template MUST keep the SAME set and
 * SAME ORDER of %-specifiers as the English one. A translator who reorders or
 * drops a specifier introduces a silent stack-read bug. Phrase around the
 * specifier; never reorder it.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_CS = 0,
    LANG_EN = 1,
    LANG_COUNT
} i18n_lang_t;

typedef enum {
    /* ── /config chrome + actions ─────────────────────────────────────── */
    STR_NAV_PHOTO, STR_NAV_CAPTURE, STR_NAV_STREAM, STR_NAV_SELFTEST, STR_NAV_HOME,
    STR_ACTIONS, STR_BTN_OTA, STR_BTN_REBOOT, STR_BTN_RESET_CFG, STR_BTN_FACTORY,
    STR_CONFIRM_REBOOT, STR_CONFIRM_RESET_CFG, STR_CONFIRM_FACTORY,
    STR_SETTINGS, STR_BTN_SAVE_SETTINGS,
    /* config group titles (replace CFG_GROUPS[]) */
    STR_GRP_CAMERA, STR_GRP_AUDIO, STR_GRP_SENSORS, STR_GRP_MODE,
    STR_GRP_PINS, STR_GRP_LED, STR_GRP_OTHER,
    /* WiFi (STA) + AP sections on /config */
    STR_WIFI_STA, STR_LBL_SSID, STR_LBL_PASS,
    STR_BTN_STA_SAVE, STR_CONFIRM_STA_SAVE,
    STR_AP_SECTION, STR_LBL_AP_SSID, STR_LBL_AP_PASS,
    STR_BTN_AP_SAVE, STR_CONFIRM_AP_SAVE,
    /* Web-admin (HTTP basic-auth) runtime credentials section on /config */
    STR_AUTH_SECTION, STR_LBL_AUTH_USER, STR_LBL_AUTH_PASS, STR_LBL_AUTH_PASS2,
    STR_BTN_AUTH_SAVE, STR_CONFIRM_AUTH_SAVE,
    /* WiFi scan picker (STA SSID) */
    STR_BTN_WIFI_SCAN, STR_SCAN_TITLE, STR_SCAN_NONE, STR_SCAN_UNAVAIL, STR_SCAN_SIGNAL,
    /* Homepage live-mic browser gain (Web Audio) */
    STR_LBL_MIC_GAIN,
    STR_WARN_AP_DEFAULT, STR_WARN_AUTH_DEFAULT,  /* top security banner */
    /* config_post / cfg_reply messages */
    STR_R_OTA_STARTED, STR_R_REBOOTING, STR_R_CFG_RESET, STR_R_FACTORY,
    STR_R_UNKNOWN_CMD, STR_R_WIFI_SAVED, STR_R_WIFI_BAD,
    STR_R_AP_SAVED, STR_R_AP_BAD, STR_R_CFG_SAVED, STR_R_CFG_NOCHANGE,
    STR_R_AUTH_SAVED, STR_R_AUTH_BAD, STR_R_AUTH_MISMATCH, STR_R_AUTH_WEAK,
    STR_BACK,
    /* cam quality hint (app_config form row) */
    STR_HINT_QUALITY,

    /* ── /wifi recovery portal ────────────────────────────────────────── */
    STR_WIFI_TITLE, STR_WIFI_SSID_LABEL, STR_WIFI_PASS_LABEL,
    STR_WIFI_SAVE_BTN, STR_WIFI_HELP, STR_WIFI_ADVANCED_LINK,
    STR_WIFI_BAD_CREDS, STR_WIFI_SAVED_REBOOT,

    /* ── /photos ──────────────────────────────────────────────────────── */
    STR_PH_SD_UNMOUNTED, STR_PH_TITLE, STR_PH_BACK, STR_PH_OPENDIR_FAIL,
    STR_PH_EMPTY, STR_PH_COUNT, STR_PH_SHOWING, STR_PH_NEWER, STR_PH_OLDER,
    /* date-tree gallery: day index + the two non-dated buckets */
    STR_PH_ALL_DAYS, STR_PH_ROOT_BUCKET, STR_PH_BOOT_BUCKET,

    /* ── app_config schema friendly names (also HA discovery names) ──────
     * Looked up by key via schema_name(); a key with no ID falls back to the
     * English SCHEMA[].name. */
    STR_CFG_VAD_ENABLED, STR_CFG_VAD_THR, STR_CFG_VAD_BURST, STR_CFG_VAD_REARM,
    STR_CFG_MODE_OVERRIDE, STR_CFG_TLM_CONT, STR_CFG_TLM_TRIG, STR_CFG_CAM_ENABLED,
    STR_CFG_TLAPSE, STR_CFG_PIR_ENABLED, STR_CFG_REED_ENABLED, STR_CFG_REED_DB,
    STR_CFG_CAM_ROTATE, STR_CFG_CAM_FRAMESIZE, STR_CFG_CAM_QUALITY,
    STR_CFG_MJPG_FRAMESIZE, STR_CFG_MJPG_QUALITY, STR_CFG_IR_LED_ENABLED,
    STR_CFG_IR_AGC, STR_CFG_CAP_LED, STR_CFG_OTA_ENABLED, STR_CFG_FLAC_ENABLED,
    STR_CFG_STATUS_LED_EN, STR_CFG_STATUS_LED_DBG,
    STR_CFG_SD_AUTOPRUNE, STR_CFG_SD_MIN_FREE, STR_CFG_SD_KEEP_DAYS,
    STR_CFG_UART_BAUD,
    STR_CFG_PIN_D0, STR_CFG_PIN_D1, STR_CFG_PIN_D2, STR_CFG_PIN_D3,
    STR_CFG_PIN_D4, STR_CFG_PIN_D5, STR_CFG_PIN_D6, STR_CFG_PIN_D7,
    STR_CFG_UI_LANG,

    /* ── homepage: section headers, summary, common statuses ────────────
     * (technical hardware-specific detail strings stay English by design). */
    STR_H_SELFTEST, STR_H_SENSORS, STR_H_DEVICE, STR_H_PINMAP,
    STR_H_ENDPOINTS, STR_H_LIVEMIC, STR_H_READY_ALL, STR_H_DEGRADED,
    STR_H_STATUS, STR_H_DIAGNOSTICS,
    STR_ST_AP_MODE, STR_ST_ASSOC, STR_ST_NO_ASSOC, STR_ST_NA_AP,
    STR_ST_BROKER_OK, STR_ST_DISCONNECTED, STR_ST_OPEN, STR_ST_CLOSED, STR_ST_READY,

    /* ── homepage: buttons, selftest labels + details, sensor table, device,
     * pin map, endpoint descriptions (full translation) ─────────────────── */
    STR_HP_CAPTURE, STR_HP_STREAM, STR_HP_STOP,
    STR_LBL_CAMERA, STR_LBL_MIC, STR_LBL_AUDIO_TASK, STR_LBL_CAM_WORKER,
    STR_LBL_TRH0, STR_LBL_TRH1, STR_LBL_REED, STR_LBL_BATTERY, STR_LBL_SOLAR,
    STR_LBL_SONAR, STR_LBL_SOIL,
    STR_D_SCCB, STR_D_MIC_OK, STR_D_MIC_NO, STR_D_TASK_OK10, STR_D_TASK_OK5,
    STR_D_TASK_FAIL, STR_D_SD_FULL_FMT, STR_D_SD_MOUNTED, STR_D_NOT_MOUNTED,
    STR_D_SHT_OK, STR_D_SHT0_NO, STR_D_SHT1_NO, STR_D_PIR_ARMED_FMT,
    STR_D_PIR_NOPIN, STR_D_PIR_FLOAT_FMT, STR_D_REED_ARMED_FMT, STR_D_REED_OFF,
    STR_D_BAT_OK, STR_D_BAT_NO, STR_D_INA_OK, STR_D_INA_NO,
    STR_S_TEMP0, STR_S_HUM0, STR_S_TEMP1, STR_S_HUM1, STR_S_BATT_SOC, STR_S_BATT,
    STR_S_USB_POWER, STR_S_MOTION, STR_S_BURSTS, STR_S_PHOTOS, STR_S_UPTIME,
    STR_S_MCU_TEMP, STR_S_FREE_HEAP, STR_S_HEAP_MINEVER, STR_S_HTTP_STACK,
    STR_S_SDCARD, STR_S_FREE, STR_S_EVENTS,
    STR_DEV_ID, STR_DEV_VERSION, STR_DEV_BUILT, STR_PM_FUNCTION,
    STR_EP_LASTJPG, STR_EP_CAPTURE, STR_EP_PHOTO, STR_EP_PHOTOS, STR_EP_MIC,
    STR_EP_STREAM, STR_EP_SELFTEST, STR_EP_I2C, STR_EP_SHT1, STR_EP_MAX1, STR_EP_I2CDIAG,
    STR_VIEW_TITLE, STR_EXIF_META, STR_DL_ORIGINAL, STR_NO_EXIF, STR_NO_CLOCK,
    STR_HP_NOPHOTO,

    STR_COUNT
} i18n_str_t;

/* Language display names, each written in its own language (NOT translated). */
extern const char *const I18N_LANG_NAMES[LANG_COUNT];

void         i18n_init(void);                       /* boot: log any NULL cell */
void         i18n_set_lang(i18n_lang_t lang);       /* persisted-knob apply */
i18n_lang_t  i18n_get_lang(void);
const char  *tr(i18n_str_t id);                     /* current language */
const char  *tr_l(i18n_lang_t lang, i18n_str_t id); /* explicit (?lang= preview) */

#ifdef __cplusplus
}
#endif
