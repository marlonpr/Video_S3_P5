idf_component_register(
    SRCS "main.cpp"
    "sd_card_test.cpp"
    "ethernet_file_server.cpp"
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
    	esp_driver_i2s
    	fatfs
        sdmmc
        vfs
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)