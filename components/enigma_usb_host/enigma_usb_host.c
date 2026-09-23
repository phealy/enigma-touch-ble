#include "enigma_usb_host.h"

#include <stdbool.h>
#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/hid_host.h"
#include "usb/hid.h"
#include "usb/usb_host.h"

#define USB_DAEMON_PRIORITY 20
#define USB_OPEN_PRIORITY 5
#define USB_TASK_STACK_SIZE 4096
#define EVENT_QUEUE_LENGTH 32
#define CDC_BUFFER_SIZE 512
#define CDC_OPEN_TIMEOUT_MS 1000
#define CDC_WRITE_TIMEOUT_MS 1000

static const char *TAG = "enigma_usb";

static QueueHandle_t event_queue;
static SemaphoreHandle_t cdc_mutex;
static cdc_acm_dev_hdl_t cdc_handle;
static uint16_t target_vid;
static uint16_t target_pid;
static uint8_t target_cdc_interface;
static volatile int active_modes;
static bool started;
static portMUX_TYPE mode_lock = portMUX_INITIALIZER_UNLOCKED;

static void queue_event(uint8_t type, const uint8_t *data, size_t length)
{
    if (event_queue == NULL) {
        return;
    }

    enigma_usb_event_t event = {
        .type = type,
        .length = length > ENIGMA_USB_EVENT_DATA_MAX ? ENIGMA_USB_EVENT_DATA_MAX : length,
    };
    if (data != NULL && event.length > 0) {
        memcpy(event.data, data, event.length);
    }
    if (xQueueSend(event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full; dropping event %u", type);
    }
}

static void set_mode(int mode, bool enabled)
{
    bool changed;
    portENTER_CRITICAL(&mode_lock);
    int previous_modes = active_modes;
    if (enabled) {
        active_modes |= mode;
    } else {
        active_modes &= ~mode;
    }
    changed = previous_modes != active_modes;
    portEXIT_CRITICAL(&mode_lock);

    if (changed) {
        queue_event(
            enabled ? ENIGMA_USB_EVENT_CONNECTED : ENIGMA_USB_EVENT_DISCONNECTED,
            (const uint8_t *)&mode,
            1);
    }
}

static void usb_daemon_task(void *arg)
{
    while (true) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            uint8_t code = (uint8_t)err;
            queue_event(ENIGMA_USB_EVENT_ERROR, &code, 1);
            continue;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static bool cdc_data_callback(const uint8_t *data, size_t data_len, void *arg)
{
    while (data_len > 0) {
        size_t chunk = data_len > ENIGMA_USB_EVENT_DATA_MAX ? ENIGMA_USB_EVENT_DATA_MAX : data_len;
        queue_event(ENIGMA_USB_EVENT_SERIAL_DATA, data, chunk);
        data += chunk;
        data_len -= chunk;
    }
    return true;
}

static void cdc_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        if (xSemaphoreTake(cdc_mutex, pdMS_TO_TICKS(CDC_WRITE_TIMEOUT_MS)) == pdTRUE) {
            cdc_acm_host_close(event->data.cdc_hdl);
            if (cdc_handle == event->data.cdc_hdl) {
                cdc_handle = NULL;
            }
            xSemaphoreGive(cdc_mutex);
        }
        set_mode(ENIGMA_USB_MODE_SERIAL, false);
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        uint8_t code = (uint8_t)event->data.error;
        queue_event(ENIGMA_USB_EVENT_ERROR, &code, 1);
    }
}

static void cdc_open_task(void *arg)
{
    const cdc_acm_host_device_config_t config = {
        .connection_timeout_ms = CDC_OPEN_TIMEOUT_MS,
        .out_buffer_size = CDC_BUFFER_SIZE,
        .in_buffer_size = CDC_BUFFER_SIZE,
        .event_cb = cdc_event_callback,
        .data_cb = cdc_data_callback,
        .user_arg = NULL,
    };

    while (true) {
        if (cdc_handle == NULL && !(enigma_usb_host_mode() & ENIGMA_USB_MODE_HID)) {
            cdc_acm_dev_hdl_t new_handle = NULL;
            esp_err_t err = cdc_acm_host_open(
                target_vid,
                target_pid,
                target_cdc_interface,
                &config,
                &new_handle);
            if (err == ESP_OK) {
                if (xSemaphoreTake(cdc_mutex, portMAX_DELAY) == pdTRUE) {
                    cdc_handle = new_handle;
                    xSemaphoreGive(cdc_mutex);
                }
                cdc_acm_host_set_control_line_state(new_handle, true, false);
                set_mode(ENIGMA_USB_MODE_SERIAL, true);
            } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_TIMEOUT) {
                uint8_t code = (uint8_t)err;
                queue_event(ENIGMA_USB_EVENT_ERROR, &code, 1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void hid_interface_callback(
    hid_host_device_handle_t device,
    const hid_host_interface_event_t event,
    void *arg)
{
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[ENIGMA_USB_EVENT_DATA_MAX];
        size_t data_length = 0;
        if (hid_host_device_get_raw_input_report_data(
                device, data, sizeof(data), &data_length) == ESP_OK) {
            queue_event(ENIGMA_USB_EVENT_HID_REPORT, data, data_length);
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        hid_host_device_close(device);
        set_mode(ENIGMA_USB_MODE_HID, false);
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        uint8_t code = (uint8_t)ESP_FAIL;
        queue_event(ENIGMA_USB_EVENT_ERROR, &code, 1);
    }
}

static void hid_driver_callback(
    hid_host_device_handle_t device,
    const hid_host_driver_event_t event,
    void *arg)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    hid_host_dev_params_t params;
    if (hid_host_device_get_params(device, &params) != ESP_OK ||
            params.proto != HID_PROTOCOL_KEYBOARD) {
        return;
    }

    const hid_host_device_config_t config = {
        .callback = hid_interface_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(device, &config);
    if (err == ESP_OK) {
        err = hid_host_device_start(device);
    }
    if (err == ESP_OK) {
        set_mode(ENIGMA_USB_MODE_HID, true);
    } else {
        hid_host_device_close(device);
        uint8_t code = (uint8_t)err;
        queue_event(ENIGMA_USB_EVENT_ERROR, &code, 1);
    }
}

esp_err_t enigma_usb_host_start(uint16_t vid, uint16_t pid, uint8_t cdc_interface)
{
    if (started) {
        return ESP_OK;
    }

    event_queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(enigma_usb_event_t));
    cdc_mutex = xSemaphoreCreateMutex();
    if (event_queue == NULL || cdc_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    target_vid = vid;
    target_pid = pid;
    target_cdc_interface = cdc_interface;
    esp_log_level_set("cdc_acm", ESP_LOG_NONE);

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(
            usb_daemon_task,
            "enigma_usb_daemon",
            USB_TASK_STACK_SIZE,
            NULL,
            USB_DAEMON_PRIORITY,
            NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = USB_OPEN_PRIORITY,
        .stack_size = USB_TASK_STACK_SIZE,
        .core_id = tskNO_AFFINITY,
        .callback = hid_driver_callback,
        .callback_arg = NULL,
    };
    err = hid_host_install(&hid_config);
    if (err != ESP_OK) {
        return err;
    }

    err = cdc_acm_host_install(NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(
            cdc_open_task,
            "enigma_cdc_open",
            USB_TASK_STACK_SIZE,
            NULL,
            USB_OPEN_PRIORITY,
            NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    started = true;
    return ESP_OK;
}

int enigma_usb_host_mode(void)
{
    portENTER_CRITICAL(&mode_lock);
    int modes = active_modes;
    portEXIT_CRITICAL(&mode_lock);
    return modes;
}

esp_err_t enigma_usb_host_poll(enigma_usb_event_t *event)
{
    if (!started || event == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueReceive(event_queue, event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t enigma_usb_host_write(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(cdc_mutex, pdMS_TO_TICKS(CDC_WRITE_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = cdc_handle == NULL
        ? ESP_ERR_INVALID_STATE
        : cdc_acm_host_data_tx_blocking(
            cdc_handle, data, length, CDC_WRITE_TIMEOUT_MS);
    xSemaphoreGive(cdc_mutex);
    return err;
}
