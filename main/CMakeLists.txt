idf_component_register(
    SRCS "main.cpp" 
    "clock_settings.cpp" 
    "clock_buttons.cpp" 
    "clock_button_logic.cpp"
    "clock_display.cpp" 
    "clock_menu.cpp" 
    "clock_ethernet.cpp" 
    "clock_wifi.cpp"
    "clock_network.cpp"
    "clock_network_core.cpp"
    "clock_alarm.cpp" 
    "clock_protocol.cpp" 
    "clock_protocol_stream.cpp"
    "clock_modes.cpp"
    "clock_mdns.cpp"
    "clock_sd_card.cpp"
    "clock_logo_manager.cpp"
    "clock_palette.cpp"
    "clock_ota.cpp"
    "clock_ota_core.cpp"
    "logo_upload_server.cpp"
    INCLUDE_DIRS "."
    REQUIRES led_panel
        DS3231
        DS18B20
        nvs_flash
        esp_event
        esp_netif
        esp_eth
        esp_wifi
        driver
        lwip
        espressif__w5500
        esp_app_format
        esp_http_client
        esp_https_ota
        esp-tls
        app_update
        mbedtls
        mdns
        fatfs
        sdmmc
        esp_driver_sdspi
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
