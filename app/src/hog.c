/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/settings/settings.h>
#include <zephyr/init.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>

#include <zmk/ble.h>
#include <zmk/endpoints_types.h>
#include <zmk/hog.h>
#include <zmk/hid.h>
#if IS_ENABLED(CONFIG_ZMK_PTP)
#include <zmk/pointing/ptp.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

enum {
    HIDS_REMOTE_WAKE = BIT(0),
    HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

struct hids_info {
    uint16_t version; /* version number of base USB HID Specification */
    uint8_t code;     /* country HID Device hardware is localized for. */
    uint8_t flags;
} __packed;

struct hids_report {
    uint8_t id;   /* report id */
    uint8_t type; /* report type */
} __packed;

static struct hids_info info = {
    .version = 0x0000,
    .code = 0x00,
    .flags = HIDS_NORMALLY_CONNECTABLE | HIDS_REMOTE_WAKE,
};

enum {
    HIDS_INPUT = 0x01,
    HIDS_OUTPUT = 0x02,
    HIDS_FEATURE = 0x03,
};

static struct hids_report input = {
    .id = ZMK_HID_REPORT_ID_KEYBOARD,
    .type = HIDS_INPUT,
};

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static struct hids_report led_indicators = {
    .id = ZMK_HID_REPORT_ID_LEDS,
    .type = HIDS_OUTPUT,
};

#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static struct hids_report consumer_input = {
    .id = ZMK_HID_REPORT_ID_CONSUMER,
    .type = HIDS_INPUT,
};

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static struct hids_report mouse_input = {
    .id = ZMK_HID_REPORT_ID_MOUSE,
    .type = HIDS_INPUT,
};

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

static struct hids_report mouse_feature = {
    .id = ZMK_HID_REPORT_ID_MOUSE,
    .type = HIDS_FEATURE,
};

#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

static bool host_requests_notification = false;
static uint8_t ctrl_point;
// static uint8_t proto_mode;

static ssize_t read_hids_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_info));
}

static ssize_t read_hids_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_report));
}

static ssize_t read_hids_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, zmk_hid_report_desc,
                             sizeof(zmk_hid_report_desc));
}

static ssize_t read_hids_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset) {
    struct zmk_hid_keyboard_report_body *report_body = &zmk_hid_get_keyboard_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_keyboard_report_body));
}

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
static ssize_t write_hids_leds_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                      const void *buf, uint16_t len, uint16_t offset,
                                      uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(struct zmk_hid_led_report_body)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct zmk_hid_led_report_body *report = (struct zmk_hid_led_report_body *)buf;
    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_BLE,
                                             .ble = {
                                                 .profile_index = profile,
                                             }};
    zmk_hid_indicators_process_report(report, endpoint);

    return len;
}

#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static ssize_t read_hids_consumer_input_report(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr, void *buf,
                                               uint16_t len, uint16_t offset) {
    struct zmk_hid_consumer_report_body *report_body = &zmk_hid_get_consumer_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_consumer_report_body));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static ssize_t read_hids_mouse_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            void *buf, uint16_t len, uint16_t offset) {
    struct zmk_hid_mouse_report_body *report_body = &zmk_hid_get_mouse_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_mouse_report_body));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

static ssize_t read_hids_mouse_feature_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                              void *buf, uint16_t len, uint16_t offset) {

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        LOG_DBG("   BT_ATT_ERR_UNLIKELY");
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_endpoint_instance endpoint = {
        .transport = ZMK_TRANSPORT_BLE,
        .ble = {.profile_index = profile},
    };

    struct zmk_pointing_resolution_multipliers mult =
        zmk_pointing_resolution_multipliers_get_profile(endpoint);

    struct zmk_hid_mouse_resolution_feature_report_body report = {
        .wheel_res = mult.wheel,
        .hwheel_res = mult.hor_wheel,
    };

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &report,
                             sizeof(struct zmk_hid_mouse_resolution_feature_report_body));
}

static ssize_t write_hids_mouse_feature_report(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr, const void *buf,
                                               uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(struct zmk_hid_mouse_resolution_feature_report_body)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct zmk_hid_mouse_resolution_feature_report_body *report =
        (struct zmk_hid_mouse_resolution_feature_report_body *)buf;
    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_BLE,
                                             .ble = {
                                                 .profile_index = profile,
                                             }};
    zmk_pointing_resolution_multipliers_process_report(report, endpoint);

    return len;
}

#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

// static ssize_t write_proto_mode(struct bt_conn *conn,
//                                 const struct bt_gatt_attr *attr,
//                                 const void *buf, uint16_t len, uint16_t offset,
//                                 uint8_t flags)
// {
//     printk("PROTO CHANGED\n");
//     return 0;
// }

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    host_requests_notification = (value == BT_GATT_CCC_NOTIFY) ? 1 : 0;
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    uint8_t *value = attr->user_data;

    if (offset + len > sizeof(ctrl_point)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(value + offset, buf, len);

    return len;
}

#if IS_ENABLED(CONFIG_ZMK_PTP)
static struct hids_report ptp_input[] = {
    {.id = ZMK_PTP_REPORT_ID(0), .type = HIDS_INPUT},
    {.id = ZMK_PTP_REPORT_ID(1), .type = HIDS_INPUT},
};
#define PTP_FEATURE_REFS(p) \
    {.id=ZMK_PTP_CAPS_ID(p),.type=HIDS_FEATURE}, \
    {.id=ZMK_PTP_CERT_ID(p),.type=HIDS_FEATURE}, \
    {.id=ZMK_PTP_MODE_ID(p),.type=HIDS_FEATURE}, \
    {.id=ZMK_PTP_SELECT_ID(p),.type=HIDS_FEATURE}, \
    {.id=ZMK_PTP_LATENCY_ID(p),.type=HIDS_FEATURE}
static struct hids_report ptp_features[] = {PTP_FEATURE_REFS(0), PTP_FEATURE_REFS(1)};
static int ptp_endpoint(struct bt_conn *conn, struct zmk_endpoint_instance *ep) {
    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0 || bt_conn_get_security(conn) < BT_SECURITY_L2) return -EACCES;
    *ep = (struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_BLE,
                                       .ble = {.profile_index = profile}};
    return 0;
}
static ssize_t read_ptp_input(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset) {
    const uint8_t idle[ZMK_PTP_BODY_SIZE] = {0};
    return bt_gatt_attr_read(conn, attr, buf, len, offset, idle, sizeof(idle));
}
static ssize_t read_ptp_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset) {
    struct zmk_endpoint_instance ep;
    if (ptp_endpoint(conn, &ep)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    const struct hids_report *ref = attr->user_data;
    uint8_t data[256];
    int size = zmk_ptp_get_feature(ep, ref->id, data, sizeof(data));
    if (size < 0) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    /* bt_gatt_attr_read implements Read Blob offsets and short final reads. */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, data, size);
}
static ssize_t write_ptp_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    if (len != 1) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    struct zmk_endpoint_instance ep;
    if (ptp_endpoint(conn, &ep)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    const struct hids_report *ref = attr->user_data;
    int err = zmk_ptp_set_feature(ep, ref->id, buf, len);
    return err ? BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED) : len;
}
static void ptp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_INF("PTP CCC handle=%u value=%u", attr->handle, value);
}
#define PTP_INPUT_ATTRS(p) \
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, \
        BT_GATT_PERM_READ_ENCRYPT, read_ptp_input, NULL, &ptp_input[p]), \
    BT_GATT_CCC(ptp_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT), \
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, \
        read_hids_report_ref, NULL, &ptp_input[p])
#define PTP_FEATURE_ATTR(i, writable) \
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, \
        BT_GATT_CHRC_READ | ((writable) ? BT_GATT_CHRC_WRITE : 0), \
        BT_GATT_PERM_READ_ENCRYPT | ((writable) ? BT_GATT_PERM_WRITE_ENCRYPT : 0), \
        read_ptp_feature, (writable) ? write_ptp_feature : NULL, &ptp_features[i]), \
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, \
        read_hids_report_ref, NULL, &ptp_features[i])
#define PTP_PAD_ATTRS(p) \
    PTP_INPUT_ATTRS(p), \
    PTP_FEATURE_ATTR((p)*5, false), PTP_FEATURE_ATTR((p)*5+1, false), \
    PTP_FEATURE_ATTR((p)*5+2, true), PTP_FEATURE_ATTR((p)*5+3, true), \
    PTP_FEATURE_ATTR((p)*5+4, true)
#endif

/* HID Service Declaration */
BT_GATT_SERVICE_DEFINE(
    hog_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    //    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
    //                           BT_GATT_PERM_WRITE, NULL, write_proto_mode, &proto_mode),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_hids_info,
                           NULL, &info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
                           read_hids_report_map, NULL, NULL),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &input),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_consumer_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &consumer_input),

#if IS_ENABLED(CONFIG_ZMK_POINTING)
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_mouse_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &mouse_input),

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_hids_mouse_feature_report, write_hids_mouse_feature_report, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &mouse_feature),
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, NULL,
                           write_hids_leds_report, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &led_indicators),
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

#if IS_ENABLED(CONFIG_ZMK_PTP)
    PTP_PAD_ATTRS(0),
    PTP_PAD_ATTRS(1),
#endif

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl_point, &ctrl_point));

/* Match a characteristic's own report-reference object, never a numeric index. */
static const struct bt_gatt_attr *input_attribute(const struct hids_report *ref) {
    for (size_t i = 0; i < hog_svc.attr_count; i++) {
        const struct bt_gatt_attr *attr = &hog_svc.attrs[i];
        if (!bt_uuid_cmp(attr->uuid, BT_UUID_HIDS_REPORT_REF) && attr->user_data == ref && i >= 2)
            return &hog_svc.attrs[i - 2]; /* value, CCC, report reference */
    }
    return NULL;
}
#if IS_ENABLED(CONFIG_ZMK_PTP)
int zmk_hog_send_ptp_report(struct zmk_endpoint_instance ep, unsigned pad,
                           const uint8_t body[ZMK_PTP_BODY_SIZE]) {
    if (ep.transport != ZMK_TRANSPORT_BLE || pad >= 2 || !body ||
        ep.ble.profile_index < 0 || ep.ble.profile_index >= ZMK_BLE_PROFILE_COUNT) return -EINVAL;
    bt_addr_le_t *addr = zmk_ble_profile_address(ep.ble.profile_index);
    struct bt_conn *conn = addr ? bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr) : NULL;
    if (!conn) return -ENOTCONN;
    const struct bt_gatt_attr *attr = input_attribute(&ptp_input[pad]);
    int err;
    if (bt_conn_get_security(conn) < BT_SECURITY_L2) err = -EACCES;
    else if (!attr) err = -ENOENT;
    else if (!bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY)) err = -EACCES;
    else if (bt_gatt_get_mtu(conn) < ZMK_PTP_BODY_SIZE + 3) err = -EMSGSIZE;
    else {
        struct bt_gatt_notify_params params = {.attr = attr, .data = body, .len = ZMK_PTP_BODY_SIZE};
        err = bt_gatt_notify_cb(conn, &params);
    }
    if (err) LOG_WRN("PTP BLE pad=%u mtu=%u err=%d", pad, bt_gatt_get_mtu(conn), err);
    bt_conn_unref(conn);
    return err;
}
static void ptp_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;
    if (err || bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL) return;
    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile >= 0) zmk_ptp_reset((struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_BLE,
                                                                .ble = {.profile_index = profile}});
}
static void ptp_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL) return;
    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile >= 0) zmk_ptp_reset((struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_BLE,
                                                                .ble = {.profile_index = profile}});
}
BT_CONN_CB_DEFINE(ptp_conn_callbacks) = {.connected = ptp_connected, .disconnected = ptp_disconnected};
#endif

K_THREAD_STACK_DEFINE(hog_q_stack, CONFIG_ZMK_BLE_THREAD_STACK_SIZE);

struct k_work_q hog_work_q;

K_MSGQ_DEFINE(zmk_hog_keyboard_msgq, sizeof(struct zmk_hid_keyboard_report_body),
              CONFIG_ZMK_BLE_KEYBOARD_REPORT_QUEUE_SIZE, 4);

void send_keyboard_report_callback(struct k_work *work) {
    struct zmk_hid_keyboard_report_body report;

    while (k_msgq_get(&zmk_hog_keyboard_msgq, &report, K_NO_WAIT) == 0) {
        struct bt_conn *conn = zmk_ble_active_profile_conn();
        if (conn == NULL) {
            return;
        }

        struct bt_gatt_notify_params notify_params = {
            .attr = input_attribute(&input),
            .data = &report,
            .len = sizeof(report),
        };

        int err = bt_gatt_notify_cb(conn, &notify_params);
        if (err == -EPERM) {
            bt_conn_set_security(conn, BT_SECURITY_L2);
        } else if (err) {
            LOG_DBG("Error notifying %d", err);
        }

        bt_conn_unref(conn);
    }
}

K_WORK_DEFINE(hog_keyboard_work, send_keyboard_report_callback);

int zmk_hog_send_keyboard_report(struct zmk_hid_keyboard_report_body *report) {
    int err = k_msgq_put(&zmk_hog_keyboard_msgq, report, K_MSEC(100));
    if (err) {
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Keyboard message queue full, popping first message and queueing again");
            struct zmk_hid_keyboard_report_body discarded_report;
            k_msgq_get(&zmk_hog_keyboard_msgq, &discarded_report, K_NO_WAIT);
            return zmk_hog_send_keyboard_report(report);
        }
        default:
            LOG_WRN("Failed to queue keyboard report to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&hog_work_q, &hog_keyboard_work);

    return 0;
};

K_MSGQ_DEFINE(zmk_hog_consumer_msgq, sizeof(struct zmk_hid_consumer_report_body),
              CONFIG_ZMK_BLE_CONSUMER_REPORT_QUEUE_SIZE, 4);

void send_consumer_report_callback(struct k_work *work) {
    struct zmk_hid_consumer_report_body report;

    while (k_msgq_get(&zmk_hog_consumer_msgq, &report, K_NO_WAIT) == 0) {
        struct bt_conn *conn = zmk_ble_active_profile_conn();
        if (conn == NULL) {
            return;
        }

        struct bt_gatt_notify_params notify_params = {
            .attr = input_attribute(&consumer_input),
            .data = &report,
            .len = sizeof(report),
        };

        int err = bt_gatt_notify_cb(conn, &notify_params);
        if (err == -EPERM) {
            bt_conn_set_security(conn, BT_SECURITY_L2);
        } else if (err) {
            LOG_DBG("Error notifying %d", err);
        }

        bt_conn_unref(conn);
    }
};

K_WORK_DEFINE(hog_consumer_work, send_consumer_report_callback);

int zmk_hog_send_consumer_report(struct zmk_hid_consumer_report_body *report) {
    int err = k_msgq_put(&zmk_hog_consumer_msgq, report, K_MSEC(100));
    if (err) {
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Consumer message queue full, popping first message and queueing again");
            struct zmk_hid_consumer_report_body discarded_report;
            k_msgq_get(&zmk_hog_consumer_msgq, &discarded_report, K_NO_WAIT);
            return zmk_hog_send_consumer_report(report);
        }
        default:
            LOG_WRN("Failed to queue consumer report to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&hog_work_q, &hog_consumer_work);

    return 0;
};

#if IS_ENABLED(CONFIG_ZMK_POINTING)

K_MSGQ_DEFINE(zmk_hog_mouse_msgq, sizeof(struct zmk_hid_mouse_report_body),
              CONFIG_ZMK_BLE_MOUSE_REPORT_QUEUE_SIZE, 4);

void send_mouse_report_callback(struct k_work *work) {
    struct zmk_hid_mouse_report_body report;
    while (k_msgq_get(&zmk_hog_mouse_msgq, &report, K_NO_WAIT) == 0) {
        struct bt_conn *conn = zmk_ble_active_profile_conn();
        if (conn == NULL) {
            return;
        }

        struct bt_gatt_notify_params notify_params = {
            .attr = input_attribute(&mouse_input),
            .data = &report,
            .len = sizeof(report),
        };

        int err = bt_gatt_notify_cb(conn, &notify_params);
        if (err == -EPERM) {
            bt_conn_set_security(conn, BT_SECURITY_L2);
        } else if (err) {
            LOG_DBG("Error notifying %d", err);
        }

        bt_conn_unref(conn);
    }
};

K_WORK_DEFINE(hog_mouse_work, send_mouse_report_callback);

int zmk_hog_send_mouse_report(struct zmk_hid_mouse_report_body *report) {
    int err = k_msgq_put(&zmk_hog_mouse_msgq, report, K_MSEC(100));
    if (err) {
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Consumer message queue full, popping first message and queueing again");
            struct zmk_hid_mouse_report_body discarded_report;
            k_msgq_get(&zmk_hog_mouse_msgq, &discarded_report, K_NO_WAIT);
            return zmk_hog_send_mouse_report(report);
        }
        default:
            LOG_WRN("Failed to queue mouse report to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&hog_work_q, &hog_mouse_work);

    return 0;
};
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

static int zmk_hog_init(void) {
    static const struct k_work_queue_config queue_config = {.name = "HID Over GATT Send Work"};
    k_work_queue_start(&hog_work_q, hog_q_stack, K_THREAD_STACK_SIZEOF(hog_q_stack),
                       CONFIG_ZMK_BLE_THREAD_PRIORITY, &queue_config);

    return 0;
}

SYS_INIT(zmk_hog_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
