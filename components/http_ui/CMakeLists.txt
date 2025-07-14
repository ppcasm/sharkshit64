idf_component_register(SRCS "http_ui.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server nvs_flash esp_netif esp_wifi modem)
