idf_component_register(
    SRCS
        "main.cpp"
        "app_config.c"
        "device_id.c"
        "diag.c"
        "wifi_mgr.c"
        "mqtt.c"
        "http_server.c"
        "dns_hijack.c"
        "i18n.c"
        "i2c_bus.c"
        "i2c_bb.c"
        "i2c_xport.c"
        "battery.c"
        "max17048.c"
        "sht41.c"
        "ina226.c"
        "bmp388.c"
        "sensors.c"
        "oled.c"
        "audio.cpp"
        "camera.c"
        "photo_queue.c"
        "jpeg_stamp.c"
        "exif_read.c"
        "sd_storage.c"
        "sd_layout.c"
        "ble_parse.c"
        "ble.c"
        "ble_store.c"
        "ota.c"
        "pir.c"
        "reed.c"
        "sonar.c"
        "soil.c"
        "uart_servo.c"
        "speaker.c"
        "pcm.c"
        "audiofx.c"
        "melody.c"
        "glitchtip.c"
        "status_led.c"
        "tls_enroll.c"
        "tls_store.c"
        "wifi_store.c"
        "net_store.c"
        "flat_json.c"
        "auth_store.c"
        "cb_pm.c"
        "cb_ds.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        cb_core
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_http_server
        esp_https_ota
        esp_https_server
        app_update
        espcoredump
        espressif__mqtt
        driver
        esp_adc
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_ledc
        esp_driver_tsens
        esp_driver_uart
        fatfs
        sdmmc
        vfs
        espressif__esp32-camera
        mbedtls
    PRIV_REQUIRES
        spi_flash
        esp_timer
        esp_pm
        bt
        esp_coex
    EMBED_FILES
        # SMB "coin" SFX — 16-bit/16 kHz mono s16le PCM (from smb_coin.wav),
        # streamed by pcm.c over I2S PDM TX. Symbols: _binary_coin_pcm_{start,end}.
        "coin.pcm"
        # Web favicon (binary .ico). Served by /favicon.ico.
        # Symbols: _binary_favicon_ico_{start,end}.
        "assets/favicon.ico"
    EMBED_TXTFILES
        # Issuing sub-CA cert (PEM). NUL-terminated by EMBED_TXTFILES so
        # mbedtls_x509_crt_parse can consume it directly. Refresh on
        # sub-CA rotation (every ~10 y) by re-running budka_ca.yml in
        # the Ansible repo and copying the resulting
        # ssl/sub_ca_budka.pem here.
        "budka_sub_ca.pem"
        # Monochrome brand mark shown in every page header. NUL-terminated
        # (EMBED_TXTFILES) so it serves with HTTPD_RESP_USE_STRLEN. Fill is a
        # fixed light colour (dark-only UI) since currentColor can't cascade
        # into an <img>-referenced SVG. Symbol: _binary_logo_svg_start.
        "assets/logo.svg"
)
