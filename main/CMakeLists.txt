idf_component_register(
    SRCS
        "enigma_ble.c"
        "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        driver
        enigma_usb_host
        esp_timer
        led_strip
        nvs_flash
)
