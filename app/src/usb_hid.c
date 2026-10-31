/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/usb.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_PTP)
#include <zmk/pointing/ptp.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct device *hid_dev;

static K_SEM_DEFINE(hid_sem, 1, 1);

static void in_ready_cb(const struct device *dev) { k_sem_give(&hid_sem); }

#define HID_GET_REPORT_TYPE_MASK 0xff00
#define HID_GET_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_INPUT 0x100
#define HID_REPORT_TYPE_OUTPUT 0x200
#define HID_REPORT_TYPE_FEATURE 0x300

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
static uint8_t hid_protocol = HID_PROTOCOL_REPORT;

static void set_proto_cb(const struct device *dev, uint8_t protocol) { hid_protocol = protocol; }

void zmk_usb_hid_set_protocol(uint8_t protocol) { hid_protocol = protocol; }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

static uint8_t *get_keyboard_report(size_t *len) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol != HID_PROTOCOL_REPORT) {
        zmk_hid_boot_report_t *boot_report = zmk_hid_get_boot_report();
        *len = sizeof(*boot_report);
        return (uint8_t *)boot_report;
    }
#endif
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    *len = sizeof(*report);
    return (uint8_t *)report;
}

#if IS_ENABLED(CONFIG_ZMK_PTP)
/* Independent interrupt endpoint and backpressure: PTP cannot consume the
 * keyboard/mouse semaphore, and neither callback can address the other map. */
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT >= 2, "PTP requires HID_1");
static const struct device *ptp_hid_dev;
static K_SEM_DEFINE(ptp_hid_sem, 1, 1);
static void ptp_in_ready_cb(const struct device *dev) { k_sem_give(&ptp_hid_sem); }
static int ptp_get_report_cb(const struct device *dev, struct usb_setup_packet *setup,
                             int32_t *len, uint8_t **data) {
    if (dev != ptp_hid_dev) return -ENOTSUP;
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_FEATURE:
        if (zmk_ptp_feature_pad(setup->wValue & HID_GET_REPORT_ID_MASK) >= 0) {
            static uint8_t feature[257];
            feature[0] = setup->wValue & HID_GET_REPORT_ID_MASK;
            int size = zmk_ptp_get_feature((struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_USB},
                                           feature[0], feature + 1, sizeof(feature) - 1);
            zmk_ptp_note_usb_feature(feature[0], false, setup->wLength,
                                      size < 0 ? 0 : size + 1, feature, size < 0 ? size : 0);
            if (size < 0) return size;
            *data = feature;
            *len = size + 1;
            return 0;
        }
        break;
    case HID_REPORT_TYPE_INPUT:
        if ((setup->wValue & 0xff) == ZMK_PTP_REPORT_ID(0)) {
            static uint8_t idle[1 + ZMK_PTP_BODY_SIZE];
            idle[0] = setup->wValue & 0xff;
            zmk_ptp_get_input((struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_USB},
                              0, idle + 1);
            *data = idle; *len = sizeof(idle);
            return 0;
        }
        break;
    }
    return -ENOTSUP;
}
static int ptp_set_report_cb(const struct device *dev, struct usb_setup_packet *setup,
                             int32_t *len, uint8_t **data) {
    if (dev != ptp_hid_dev) return -ENOTSUP;
    if ((setup->wValue & HID_GET_REPORT_TYPE_MASK) == HID_REPORT_TYPE_FEATURE) {
        if (zmk_ptp_feature_pad(setup->wValue & HID_GET_REPORT_ID_MASK) >= 0) {
            uint8_t id = setup->wValue & HID_GET_REPORT_ID_MASK;
            int result = -EINVAL;
            if (*len >= 1 && *data && (*data)[0] == id)
                result = zmk_ptp_set_feature((struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_USB},
                                              id, *data + 1, *len - 1);
            zmk_ptp_note_usb_feature(id, true, setup->wLength, *len < 0 ? 0 : *len, *data, result);
            return result;
        }
    }
    return -ENOTSUP;
}
static const struct hid_ops ptp_ops = {
    .int_in_ready = ptp_in_ready_cb,
    .get_report = ptp_get_report_cb,
    .set_report = ptp_set_report_cb,
};
#endif

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if (dev != hid_dev) return -ENOTSUP;
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_FEATURE:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE:
            static struct zmk_hid_mouse_resolution_feature_report res_feature_report;

            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            *len = sizeof(struct zmk_hid_mouse_resolution_feature_report);
            struct zmk_pointing_resolution_multipliers mult =
                zmk_pointing_resolution_multipliers_get_profile(endpoint);

            res_feature_report.body.wheel_res = mult.wheel;
            res_feature_report.body.hwheel_res = mult.hor_wheel;
            *data = (uint8_t *)&res_feature_report;
            break;
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        default:
            return -ENOTSUP;
        }
        break;
    case HID_REPORT_TYPE_INPUT:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
        case ZMK_HID_REPORT_ID_KEYBOARD: {
            size_t size;
            *data = get_keyboard_report(&size);
            *len = (int32_t)size;
            break;
        }
        case ZMK_HID_REPORT_ID_CONSUMER: {
            struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
            *data = (uint8_t *)report;
            *len = sizeof(*report);
            break;
        }
        default:
            LOG_ERR("Invalid report ID %d requested", setup->wValue & HID_GET_REPORT_ID_MASK);
            return -EINVAL;
        }
        break;
    default:
        /*
         * 7.2.1 of the HID v1.11 spec is unclear about handling requests for reports that do not
         * exist For requested reports that aren't input reports, return -ENOTSUP like the Zephyr
         * subsys does
         */
        LOG_ERR("Unsupported report type %d requested", (setup->wValue & HID_GET_REPORT_TYPE_MASK)
                                                            << 8);
        return -ENOTSUP;
    }

    return 0;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if (dev != hid_dev) return -ENOTSUP;
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_FEATURE:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE:
            if (*len != sizeof(struct zmk_hid_mouse_resolution_feature_report)) {
                return -EINVAL;
            }

            struct zmk_hid_mouse_resolution_feature_report *report =
                (struct zmk_hid_mouse_resolution_feature_report *)*data;
            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            zmk_pointing_resolution_multipliers_process_report(&report->body, endpoint);

            break;
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        default:
            return -ENOTSUP;
        }
        break;

    case HID_REPORT_TYPE_OUTPUT:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        case ZMK_HID_REPORT_ID_LEDS:
            if (*len != sizeof(struct zmk_hid_led_report)) {
                LOG_ERR("LED set report is malformed: length=%d", *len);
                return -EINVAL;
            } else {
                struct zmk_hid_led_report *report = (struct zmk_hid_led_report *)*data;
                struct zmk_endpoint_instance endpoint = {
                    .transport = ZMK_TRANSPORT_USB,
                };
                zmk_hid_indicators_process_report(&report->body, endpoint);
            }
            break;
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        default:
            LOG_ERR("Invalid report ID %d requested", setup->wValue & HID_GET_REPORT_ID_MASK);
            return -EINVAL;
        }
        break;
    default:
        LOG_ERR("Unsupported report type %d requested",
                (setup->wValue & HID_GET_REPORT_TYPE_MASK) >> 8);
        return -ENOTSUP;
    }

    return 0;
}

static const struct hid_ops ops = {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    .protocol_change = set_proto_cb,
#endif
    .int_in_ready = in_ready_cb,
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

static int zmk_usb_hid_send_report(const uint8_t *report, size_t len) {
    switch (zmk_usb_get_status()) {
    case USB_DC_SUSPEND:
        return usb_wakeup_request();
    case USB_DC_ERROR:
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_UNKNOWN:
        return -ENODEV;
    default: {
        int err = k_sem_take(&hid_sem, K_MSEC(30));
        if (err) return err;
        err = hid_int_ep_write(hid_dev, report, len, NULL);

        if (err) {
            k_sem_give(&hid_sem);
        }

        return err;
    }
    }
}

int zmk_usb_hid_send_keyboard_report(void) {
    size_t len;
    uint8_t *report = get_keyboard_report(&len);
    return zmk_usb_hid_send_report(report, len);
}

int zmk_usb_hid_send_consumer_report(void) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    return zmk_usb_hid_send_report((uint8_t *)report, sizeof(*report));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_usb_hid_send_mouse_report() {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    return zmk_usb_hid_send_report((uint8_t *)report, sizeof(*report));
}
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

#if IS_ENABLED(CONFIG_ZMK_PTP)
int zmk_usb_hid_send_ptp_report(unsigned pad, const uint8_t body[ZMK_PTP_BODY_SIZE]) {
    if (pad >= ZMK_PTP_PAD_COUNT || !body) return -EINVAL;
    if (!ptp_hid_dev) return -ENODEV;
    if (!zmk_usb_is_hid_ready() || zmk_usb_get_status() == USB_DC_SUSPEND) return -ENOTCONN;
    /* nrfx transfers asynchronously from this address without copying it.
     * Hold storage until int_in_ready releases the semaphore. Acquire before
     * touching the buffer, including on failed/busy sends. */
    static uint8_t report[1 + ZMK_PTP_BODY_SIZE];
    if (k_sem_take(&ptp_hid_sem, K_NO_WAIT)) return -EAGAIN;
    report[0] = ZMK_PTP_REPORT_ID(pad);
    memcpy(report + 1, body, ZMK_PTP_BODY_SIZE);
    int err = hid_int_ep_write(ptp_hid_dev, report, sizeof(report), NULL);
    if (err) k_sem_give(&ptp_hid_sem);
    return err;
}
#endif

static int zmk_usb_hid_init(void) {
    hid_dev = device_get_binding("HID_0");
    if (hid_dev == NULL) {
        LOG_ERR("Unable to locate HID device");
        return -EINVAL;
    }

    usb_hid_register_device(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    usb_hid_set_proto_code(hid_dev, HID_BOOT_IFACE_CODE_KEYBOARD);
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    int err = usb_hid_init(hid_dev);
    if (err) return err;
#if IS_ENABLED(CONFIG_ZMK_PTP)
    ptp_hid_dev = device_get_binding("HID_1");
    if (!ptp_hid_dev) return -ENODEV;
    usb_hid_register_device(ptp_hid_dev, zmk_ptp_usb_report_desc,
                            sizeof(zmk_ptp_usb_report_desc), &ptp_ops);
    err = usb_hid_init(ptp_hid_dev);
    if (err) return err;
#endif
    return 0;
}

SYS_INIT(zmk_usb_hid_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
