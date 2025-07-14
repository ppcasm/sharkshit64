idf_component_register(SRCS "modem.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_netif esp_wifi nvs_flash esp_http_server esp_timer http_ui)
