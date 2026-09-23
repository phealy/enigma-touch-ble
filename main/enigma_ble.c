#include "enigma_ble.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "enigma_usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define DEVICE_NAME "Enigma Touch BLE"
#define MAX_CONNECTIONS 2
#define SERIAL_QUEUE_LENGTH 8
#define SERIAL_PACKET_MAX 512

#define HID_SERVICE_UUID 0x1812
#define HID_INFORMATION_UUID 0x2a4a
#define REPORT_MAP_UUID 0x2a4b
#define HID_CONTROL_POINT_UUID 0x2a4c
#define REPORT_UUID 0x2a4d
#define PROTOCOL_MODE_UUID 0x2a4e
#define BOOT_KEYBOARD_INPUT_UUID 0x2a22
#define BOOT_KEYBOARD_OUTPUT_UUID 0x2a32
#define REPORT_REFERENCE_UUID 0x2908

#define NUS_SERVICE_BYTES \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
#define NUS_RX_BYTES \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
#define NUS_TX_BYTES \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e
#define NUS_STATUS_BYTES \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
    0x93, 0xf3, 0xa3, 0xb5, 0x04, 0x00, 0x40, 0x6e

typedef enum {
    ATTR_HID_INFORMATION,
    ATTR_REPORT_MAP,
    ATTR_HID_CONTROL_POINT,
    ATTR_REPORT,
    ATTR_REPORT_REFERENCE,
    ATTR_PROTOCOL_MODE,
    ATTR_BOOT_INPUT,
    ATTR_BOOT_OUTPUT,
    ATTR_NUS_RX,
    ATTR_NUS_TX,
    ATTR_NUS_STATUS,
} attribute_id_t;

typedef struct {
    uint16_t connection_handle;
    uint16_t mtu;
    uint8_t protocol_mode;
    bool secured;
    bool report_notify;
    bool boot_notify;
    bool nus_notify;
    bool status_notify;
} connection_state_t;

typedef struct {
    size_t length;
    uint8_t data[SERIAL_PACKET_MAX];
} serial_packet_t;

static const char *TAG = "enigma_ble";
static const uint8_t HID_INFORMATION[] = {0x11, 0x01, 0x00, 0x03};
static const uint8_t REPORT_REFERENCE[] = {0x00, 0x01};
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07,
    0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

static const ble_uuid128_t NUS_SERVICE_UUID =
    BLE_UUID128_INIT(NUS_SERVICE_BYTES);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(NUS_RX_BYTES);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(NUS_TX_BYTES);
static const ble_uuid128_t NUS_STATUS_UUID = BLE_UUID128_INIT(NUS_STATUS_BYTES);

static connection_state_t connections[MAX_CONNECTIONS];
static portMUX_TYPE connections_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t serial_queue;
static uint8_t own_address_type;
static uint16_t report_handle;
static uint16_t boot_input_handle;
static uint16_t nus_tx_handle;
static uint16_t nus_status_handle;
static uint8_t usb_mode;
static uint8_t keyboard_report[8];
static uint8_t boot_output;
static uint8_t hid_control_point;

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_advertising(void);

static connection_state_t *find_connection(uint16_t handle)
{
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].connection_handle == handle) {
            return &connections[i];
        }
    }
    return NULL;
}

static size_t connection_count(void)
{
    size_t count = 0;
    portENTER_CRITICAL(&connections_lock);
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].connection_handle != BLE_HS_CONN_HANDLE_NONE) {
            ++count;
        }
    }
    portEXIT_CRITICAL(&connections_lock);
    return count;
}

bool enigma_ble_secured(void)
{
    bool secured = false;
    portENTER_CRITICAL(&connections_lock);
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].connection_handle != BLE_HS_CONN_HANDLE_NONE &&
            connections[i].secured) {
            secured = true;
            break;
        }
    }
    portEXIT_CRITICAL(&connections_lock);
    return secured;
}

static int append_value(struct os_mbuf *output, const void *value, size_t length)
{
    return os_mbuf_append(output, value, length) == 0
        ? 0
        : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int read_attribute(
    uint16_t connection_handle,
    struct ble_gatt_access_ctxt *ctxt,
    attribute_id_t attribute)
{
    switch (attribute) {
    case ATTR_HID_INFORMATION:
        return append_value(ctxt->om, HID_INFORMATION, sizeof(HID_INFORMATION));
    case ATTR_REPORT_MAP:
        return append_value(ctxt->om, HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    case ATTR_REPORT:
    case ATTR_BOOT_INPUT:
        return append_value(ctxt->om, keyboard_report, sizeof(keyboard_report));
    case ATTR_REPORT_REFERENCE:
        return append_value(ctxt->om, REPORT_REFERENCE, sizeof(REPORT_REFERENCE));
    case ATTR_PROTOCOL_MODE: {
        uint8_t mode = 1;
        portENTER_CRITICAL(&connections_lock);
        connection_state_t *connection = find_connection(connection_handle);
        if (connection != NULL) {
            mode = connection->protocol_mode;
        }
        portEXIT_CRITICAL(&connections_lock);
        return append_value(ctxt->om, &mode, sizeof(mode));
    }
    case ATTR_BOOT_OUTPUT:
        return append_value(ctxt->om, &boot_output, sizeof(boot_output));
    case ATTR_NUS_STATUS: {
        portENTER_CRITICAL(&connections_lock);
        uint8_t mode = usb_mode;
        portEXIT_CRITICAL(&connections_lock);
        return append_value(ctxt->om, &mode, sizeof(mode));
    }
    default:
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
}

static int copy_write(
    struct os_mbuf *input,
    void *destination,
    size_t minimum,
    size_t maximum,
    uint16_t *copied)
{
    uint16_t length = OS_MBUF_PKTLEN(input);
    if (length < minimum || length > maximum) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return ble_hs_mbuf_to_flat(input, destination, maximum, copied) == 0
        ? 0
        : BLE_ATT_ERR_UNLIKELY;
}

static int write_attribute(
    uint16_t connection_handle,
    struct ble_gatt_access_ctxt *ctxt,
    attribute_id_t attribute)
{
    uint16_t copied = 0;
    switch (attribute) {
    case ATTR_HID_CONTROL_POINT:
        return copy_write(
            ctxt->om,
            &hid_control_point,
            sizeof(hid_control_point),
            sizeof(hid_control_point),
            &copied);
    case ATTR_PROTOCOL_MODE: {
        uint8_t mode;
        int rc = copy_write(
            ctxt->om, &mode, sizeof(mode), sizeof(mode), &copied);
        if (rc != 0) {
            return rc;
        }
        if (mode > 1) {
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        portENTER_CRITICAL(&connections_lock);
        connection_state_t *connection = find_connection(connection_handle);
        if (connection != NULL) {
            connection->protocol_mode = mode;
        }
        portEXIT_CRITICAL(&connections_lock);
        return 0;
    }
    case ATTR_BOOT_OUTPUT:
        return copy_write(
            ctxt->om,
            &boot_output,
            sizeof(boot_output),
            sizeof(boot_output),
            &copied);
    case ATTR_NUS_RX: {
        portENTER_CRITICAL(&connections_lock);
        bool serial_connected = (usb_mode & ENIGMA_USB_MODE_SERIAL) != 0;
        portEXIT_CRITICAL(&connections_lock);
        if (!serial_connected) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        serial_packet_t packet;
        int rc = copy_write(
            ctxt->om, packet.data, 1, sizeof(packet.data), &copied);
        if (rc != 0) {
            return rc;
        }
        packet.length = copied;
        return xQueueSend(serial_queue, &packet, 0) == pdTRUE
            ? 0
            : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
}

static int attribute_access(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg)
{
    attribute_id_t attribute = (attribute_id_t)(uintptr_t)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_READ_DSC:
        return read_attribute(connection_handle, ctxt, attribute);
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        return write_attribute(connection_handle, ctxt, attribute);
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

#define HID_READ_FLAGS \
    (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
#define HID_WRITE_FLAGS \
    (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | \
     BLE_GATT_CHR_F_WRITE_ENC)
#define HID_NOTIFY_FLAGS \
    (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | \
     BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC)

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(HID_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(HID_INFORMATION_UUID),
                .access_cb = attribute_access,
                .arg = (void *)ATTR_HID_INFORMATION,
                .flags = HID_READ_FLAGS,
            },
            {
                .uuid = BLE_UUID16_DECLARE(REPORT_MAP_UUID),
                .access_cb = attribute_access,
                .arg = (void *)ATTR_REPORT_MAP,
                .flags = HID_READ_FLAGS,
            },
            {
                .uuid = BLE_UUID16_DECLARE(HID_CONTROL_POINT_UUID),
                .access_cb = attribute_access,
                .arg = (void *)ATTR_HID_CONTROL_POINT,
                .flags = HID_WRITE_FLAGS,
            },
            {
                .uuid = BLE_UUID16_DECLARE(REPORT_UUID),
                .access_cb = attribute_access,
                .arg = (void *)ATTR_REPORT,
                .flags = HID_NOTIFY_FLAGS,
                .val_handle = &report_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(REPORT_REFERENCE_UUID),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                        .access_cb = attribute_access,
                        .arg = (void *)ATTR_REPORT_REFERENCE,
                    },
                    {0},
                },
            },
            {
                .uuid = BLE_UUID16_DECLARE(PROTOCOL_MODE_UUID),
                .access_cb = attribute_access,
                .arg = (void *)ATTR_PROTOCOL_MODE,
                .flags = HID_READ_FLAGS | HID_WRITE_FLAGS,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BOOT_KEYBOARD_INPUT_UUID),
                .access_cb = attribute_access,
                .arg = (void *)ATTR_BOOT_INPUT,
                .flags = HID_NOTIFY_FLAGS,
                .val_handle = &boot_input_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BOOT_KEYBOARD_OUTPUT_UUID),
                .access_cb = attribute_access,
                .arg = (void *)ATTR_BOOT_OUTPUT,
                .flags = HID_READ_FLAGS | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &NUS_TX_UUID.u,
                .access_cb = attribute_access,
                .arg = (void *)ATTR_NUS_TX,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &nus_tx_handle,
            },
            {
                .uuid = &NUS_RX_UUID.u,
                .access_cb = attribute_access,
                .arg = (void *)ATTR_NUS_RX,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &NUS_STATUS_UUID.u,
                .access_cb = attribute_access,
                .arg = (void *)ATTR_NUS_STATUS,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &nus_status_handle,
            },
            {0},
        },
    },
    {0},
};

static void start_advertising(void)
{
    if (connection_count() >= MAX_CONNECTIONS || ble_gap_adv_active()) {
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.appearance = 0x03c1;
    fields.appearance_is_present = 1;
    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(HID_SERVICE_UUID),
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Unable to set advertising data: %d", rc);
        return;
    }

    struct ble_hs_adv_fields scan_response = {0};
    scan_response.uuids128 = (ble_uuid128_t *)&NUS_SERVICE_UUID;
    scan_response.num_uuids128 = 1;
    scan_response.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_response);
    if (rc != 0) {
        ESP_LOGE(TAG, "Unable to set scan response: %d", rc);
        return;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(
        own_address_type,
        NULL,
        BLE_HS_FOREVER,
        &parameters,
        gap_event,
        NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Unable to start advertising: %d", rc);
    }
}

static void add_connection(uint16_t handle)
{
    portENTER_CRITICAL(&connections_lock);
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].connection_handle == BLE_HS_CONN_HANDLE_NONE) {
            connections[i] = (connection_state_t) {
                .connection_handle = handle,
                .mtu = 23,
                .protocol_mode = 1,
            };
            break;
        }
    }
    portEXIT_CRITICAL(&connections_lock);
}

static void remove_connection(uint16_t handle)
{
    portENTER_CRITICAL(&connections_lock);
    connection_state_t *connection = find_connection(handle);
    if (connection != NULL) {
        memset(connection, 0, sizeof(*connection));
        connection->connection_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    portEXIT_CRITICAL(&connections_lock);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            add_connection(event->connect.conn_handle);
            ESP_LOGI(TAG, "BLE connected: %u", event->connect.conn_handle);
            start_advertising();
        } else {
            ESP_LOGW(TAG, "BLE connection failed: %d", event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(
            TAG,
            "BLE disconnected: %u, reason %d",
            event->disconnect.conn.conn_handle,
            event->disconnect.reason);
        remove_connection(event->disconnect.conn.conn_handle);
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc description;
        int rc = ble_gap_conn_find(
            event->enc_change.conn_handle, &description);
        bool secured = rc == 0 && event->enc_change.status == 0 &&
            description.sec_state.encrypted && description.sec_state.bonded;
        portENTER_CRITICAL(&connections_lock);
        connection_state_t *connection =
            find_connection(event->enc_change.conn_handle);
        if (connection != NULL) {
            connection->secured = secured;
        }
        portEXIT_CRITICAL(&connections_lock);
        if (rc == 0) {
            ESP_LOGI(
                TAG,
                "Security updated: status=%d encrypted=%d authenticated=%d bonded=%d",
                event->enc_change.status,
                description.sec_state.encrypted,
                description.sec_state.authenticated,
                description.sec_state.bonded);
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        portENTER_CRITICAL(&connections_lock);
        connection_state_t *connection =
            find_connection(event->subscribe.conn_handle);
        if (connection != NULL) {
            if (event->subscribe.attr_handle == report_handle) {
                connection->report_notify = event->subscribe.cur_notify;
            } else if (event->subscribe.attr_handle == boot_input_handle) {
                connection->boot_notify = event->subscribe.cur_notify;
            } else if (event->subscribe.attr_handle == nus_tx_handle) {
                connection->nus_notify = event->subscribe.cur_notify;
            } else if (event->subscribe.attr_handle == nus_status_handle) {
                connection->status_notify = event->subscribe.cur_notify;
            }
        }
        portEXIT_CRITICAL(&connections_lock);
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        portENTER_CRITICAL(&connections_lock);
        connection_state_t *connection =
            find_connection(event->mtu.conn_handle);
        if (connection != NULL) {
            connection->mtu = event->mtu.value;
        }
        portEXIT_CRITICAL(&connections_lock);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc description;
        if (ble_gap_conn_find(
                event->repeat_pairing.conn_handle, &description) == 0) {
            ble_store_util_delete_peer(&description.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(
            TAG,
            "Pairing action %u on connection %u",
            event->passkey.params.action,
            event->passkey.conn_handle);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &own_address_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Unable to configure BLE identity: %d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t enigma_ble_start(void)
{
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        connections[i].connection_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    serial_queue = xQueueCreate(SERIAL_QUEUE_LENGTH, sizeof(serial_packet_t));
    if (serial_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = nimble_port_init();
    if (error != ESP_OK) {
        return error;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc == 0) {
        rc = ble_svc_gap_device_appearance_set(0x03c1);
    }
    if (rc == 0) {
        rc = ble_gatts_count_cfg(services);
    }
    if (rc == 0) {
        rc = ble_gatts_add_svcs(services);
    }
    if (rc != 0) {
        nimble_port_deinit();
        return ESP_FAIL;
    }

    extern void ble_store_config_init(void);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool enigma_ble_connected(void)
{
    return connection_count() > 0;
}

static esp_err_t notify(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    const uint8_t *data,
    size_t length)
{
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(data, length);
    if (packet == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(
        connection_handle, attribute_handle, packet);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void enigma_ble_set_usb_mode(uint8_t mode)
{
    connection_state_t snapshot[MAX_CONNECTIONS];
    portENTER_CRITICAL(&connections_lock);
    if (usb_mode == mode) {
        portEXIT_CRITICAL(&connections_lock);
        return;
    }
    usb_mode = mode;
    memcpy(snapshot, connections, sizeof(snapshot));
    portEXIT_CRITICAL(&connections_lock);

    if ((mode & ENIGMA_USB_MODE_SERIAL) == 0) {
        xQueueReset(serial_queue);
    }
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (snapshot[i].connection_handle != BLE_HS_CONN_HANDLE_NONE &&
            snapshot[i].status_notify) {
            esp_err_t error = notify(
                snapshot[i].connection_handle, nus_status_handle, &mode, 1);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "USB status notification failed: %s",
                         esp_err_to_name(error));
            }
        }
    }
}

esp_err_t enigma_ble_send_keyboard_report(const uint8_t *data, size_t length)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 9) {
        ++data;
        --length;
    }
    if (length == 4) {
        memcpy(keyboard_report, data, length);
        memset(keyboard_report + length, 0, sizeof(keyboard_report) - length);
    } else if (length == sizeof(keyboard_report)) {
        memcpy(keyboard_report, data, length);
    } else {
        ESP_LOGW(TAG, "Ignoring %u-byte keyboard report", (unsigned)length);
        return ESP_ERR_INVALID_SIZE;
    }

    connection_state_t snapshot[MAX_CONNECTIONS];
    portENTER_CRITICAL(&connections_lock);
    memcpy(snapshot, connections, sizeof(snapshot));
    portEXIT_CRITICAL(&connections_lock);

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (snapshot[i].connection_handle == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }
        uint16_t handle = 0;
        if (snapshot[i].protocol_mode == 0 && snapshot[i].boot_notify) {
            handle = boot_input_handle;
        } else if (snapshot[i].protocol_mode != 0 &&
                   snapshot[i].report_notify) {
            handle = report_handle;
        }
        if (handle != 0) {
            esp_err_t error = notify(
                snapshot[i].connection_handle,
                handle,
                keyboard_report,
                sizeof(keyboard_report));
            if (error != ESP_OK) {
                result = error;
            } else if (result == ESP_ERR_NOT_FOUND) {
                result = ESP_OK;
            }
        }
    }
    return result;
}

esp_err_t enigma_ble_send_serial(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    connection_state_t snapshot[MAX_CONNECTIONS];
    portENTER_CRITICAL(&connections_lock);
    memcpy(snapshot, connections, sizeof(snapshot));
    portEXIT_CRITICAL(&connections_lock);

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (snapshot[i].connection_handle == BLE_HS_CONN_HANDLE_NONE ||
            !snapshot[i].nus_notify) {
            continue;
        }
        size_t chunk_size = snapshot[i].mtu > 3 ? snapshot[i].mtu - 3 : 20;
        for (size_t offset = 0; offset < length; offset += chunk_size) {
            size_t remaining = length - offset;
            size_t current = remaining < chunk_size ? remaining : chunk_size;
            esp_err_t error = notify(
                snapshot[i].connection_handle,
                nus_tx_handle,
                data + offset,
                current);
            if (error != ESP_OK) {
                result = error;
                break;
            }
            result = ESP_OK;
        }
    }
    return result;
}

esp_err_t enigma_ble_receive_serial(
    uint8_t *data,
    size_t capacity,
    size_t *length)
{
    if (data == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    serial_packet_t packet;
    if (xQueueReceive(serial_queue, &packet, 0) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }
    if (packet.length > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, packet.data, packet.length);
    *length = packet.length;
    return ESP_OK;
}
