#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "enigma_ble.h"
#include "enigma_usb_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define ENIGMA_VID 0x0483
#define ENIGMA_PID 0x5740
#define ENIGMA_CDC_INTERFACE 0
#define NEOPIXEL_PIN GPIO_NUM_48
#define NEOPIXEL_DIM 8
#define BOND_RESET_PIN GPIO_NUM_0
#define BOND_RESET_HOLD_US 3000000
#define MAX_EVENTS_PER_ITERATION 32

static const char *TAG = "enigma_bridge";
static led_strip_handle_t status_led;

static void set_status_led(uint8_t red, uint8_t green, uint8_t blue)
{
    if (status_led == NULL) {
        return;
    }
    esp_err_t error = led_strip_set_pixel(status_led, 0, red, green, blue);
    if (error == ESP_OK) {
        error = led_strip_refresh(status_led);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Unable to update status LED: %s", esp_err_to_name(error));
    }
}

static void initialize_status_hardware(void)
{
#if CONFIG_ENIGMA_STATUS_LED
    const led_strip_config_t strip_config = {
        .strip_gpio_num = NEOPIXEL_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t error = led_strip_new_rmt_device(
        &strip_config, &rmt_config, &status_led);
    if (error != ESP_OK) {
        status_led = NULL;
        ESP_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(error));
    } else {
        set_status_led(0, 0, 0);
    }
#endif

    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BOND_RESET_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
}

static void initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
}

static void bridge_task(void *arg)
{
    int previous_mode = -1;
    uint32_t previous_color = UINT32_MAX;
    int64_t reset_started = 0;
    bool reset_blocked = false;
    uint8_t serial_data[512];

    while (true) {
        if (gpio_get_level(BOND_RESET_PIN) == 0) {
            if (enigma_ble_connected()) {
                if (!reset_blocked) {
                    ESP_LOGW(TAG, "Bond reset blocked while BLE is connected");
                }
                reset_blocked = true;
                reset_started = 0;
            } else if (!reset_blocked && reset_started == 0) {
                reset_started = esp_timer_get_time();
                ESP_LOGI(TAG, "BOOT pressed; hold for three seconds to clear bonds");
            } else if (!reset_blocked &&
                       esp_timer_get_time() - reset_started >=
                       BOND_RESET_HOLD_US) {
                ESP_LOGW(TAG, "Clearing BLE bonds and restarting");
                set_status_led(NEOPIXEL_DIM, 0, 0);
                ESP_ERROR_CHECK(nvs_flash_erase());
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_restart();
            }
        } else if (reset_started != 0 || reset_blocked) {
            ESP_LOGI(TAG, "Bond reset cancelled");
            reset_started = 0;
            reset_blocked = false;
        }

        int mode = enigma_usb_host_mode();
        if (mode != previous_mode) {
            enigma_ble_set_usb_mode(mode);
            ESP_LOGI(TAG, "Enigma USB mode: %d", mode);
            previous_mode = mode;
        }

        uint32_t color = 0;
        bool secured = enigma_ble_secured();
        if (secured &&
            (mode & ENIGMA_USB_MODE_SERIAL) != 0) {
            color = NEOPIXEL_DIM;
        } else if (secured &&
                   (mode & ENIGMA_USB_MODE_HID) != 0) {
            color = (uint32_t)NEOPIXEL_DIM << 8;
        }
        if (color != previous_color) {
            set_status_led(
                (color >> 16) & 0xff,
                (color >> 8) & 0xff,
                color & 0xff);
            previous_color = color;
        }

        enigma_usb_event_t event;
        for (size_t processed = 0;
             processed < MAX_EVENTS_PER_ITERATION &&
             enigma_usb_host_poll(&event) == ESP_OK;
             ++processed) {
            switch (event.type) {
            case ENIGMA_USB_EVENT_HID_REPORT:
                enigma_ble_send_keyboard_report(event.data, event.length);
                break;
            case ENIGMA_USB_EVENT_SERIAL_DATA:
                enigma_ble_send_serial(event.data, event.length);
                break;
            case ENIGMA_USB_EVENT_ERROR:
                ESP_LOGW(
                    TAG,
                    "USB host error: %u",
                    event.length > 0 ? event.data[0] : 0);
                break;
            default:
                break;
            }
        }

        size_t serial_length;
        for (size_t processed = 0;
             processed < MAX_EVENTS_PER_ITERATION &&
             enigma_ble_receive_serial(
                 serial_data, sizeof(serial_data), &serial_length) == ESP_OK;
             ++processed) {
            esp_err_t error =
                enigma_usb_host_write(serial_data, serial_length);
            if (error != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "USB serial write failed: %s",
                    esp_err_to_name(error));
            }
        }

        vTaskDelay(1);
    }
}

void app_main(void)
{
    initialize_nvs();
    initialize_status_hardware();
    ESP_ERROR_CHECK(enigma_ble_start());
    ESP_ERROR_CHECK(enigma_usb_host_start(
        ENIGMA_VID, ENIGMA_PID, ENIGMA_CDC_INTERFACE));

    BaseType_t created = xTaskCreate(
        bridge_task, "enigma_bridge", 6144, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Unable to create bridge task");
        abort();
    }
}
