idf_component_register(SRCS "gpio_keyboard.c" "esp_hid_host.c" "esp_hid_gap.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_hid nvs_flash)
