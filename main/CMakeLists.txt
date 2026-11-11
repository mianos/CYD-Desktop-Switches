set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(SRCS "main.cpp" "Config.cpp" "Display.cpp" "Ui.cpp" "PanelWebServer.cpp"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "default_config.json"
                       REQUIRES
                         esp_lcd
                         esp_driver_spi
                         esp_driver_gpio
                         esp_driver_ledc
                         esp_wifi
                         esp_netif
                         esp_event
                         nvs_flash
                         app_update
                         webserver
                         nvsstoragemanager
                         wifimanager
                         settingsbase
                         mqttwrapper
                         jsonwrapper
                         espressif__mqtt
                         espressif__network_provisioning)
