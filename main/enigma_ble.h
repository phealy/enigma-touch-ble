#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t enigma_ble_start(void);
bool enigma_ble_connected(void);
bool enigma_ble_secured(void);
void enigma_ble_set_usb_mode(uint8_t mode);
esp_err_t enigma_ble_send_keyboard_report(const uint8_t *data, size_t length);
esp_err_t enigma_ble_send_serial(const uint8_t *data, size_t length);
esp_err_t enigma_ble_receive_serial(
    uint8_t *data,
    size_t capacity,
    size_t *length);
