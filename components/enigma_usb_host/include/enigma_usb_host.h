#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ENIGMA_USB_EVENT_HID_REPORT 1
#define ENIGMA_USB_EVENT_SERIAL_DATA 2
#define ENIGMA_USB_EVENT_CONNECTED 3
#define ENIGMA_USB_EVENT_DISCONNECTED 4
#define ENIGMA_USB_EVENT_ERROR 5

#define ENIGMA_USB_MODE_HID 0x01
#define ENIGMA_USB_MODE_SERIAL 0x02

#define ENIGMA_USB_EVENT_DATA_MAX 64

typedef struct {
    uint8_t type;
    uint8_t length;
    uint8_t data[ENIGMA_USB_EVENT_DATA_MAX];
} enigma_usb_event_t;

esp_err_t enigma_usb_host_start(uint16_t vid, uint16_t pid, uint8_t cdc_interface);
int enigma_usb_host_mode(void);
esp_err_t enigma_usb_host_poll(enigma_usb_event_t *event);
esp_err_t enigma_usb_host_write(const uint8_t *data, size_t length);
