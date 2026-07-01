idf_component_register(
    SRCS "main.cpp" 
    "clock_settings.cpp" 
    "clock_buttons.cpp" 
    "clock_display.cpp" 
    "clock_menu.cpp" 
    "clock_ethernet.cpp" 
    "clock_alarm.cpp" 
    "clock_protocol.cpp" 
    "clock_modes.cpp"
    "clock_mdns.cpp"
    "clock_sd_card.cpp"
    "clock_logo_manager.cpp"
    "logo_upload_server.cpp"
    INCLUDE_DIRS "."
    REQUIRES led_panel 
    	DS3231 
    	DS18B20 
    	nvs_flash 
    	esp_event 
    	esp_netif 
    	esp_eth 
    	driver 
    	lwip 
    	espressif__w5500
    	esp_app_format
        mdns
        fatfs
        sdmmc
        esp_driver_sdspi
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
