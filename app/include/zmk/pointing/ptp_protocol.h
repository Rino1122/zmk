/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZMK_PTP_PAD_COUNT 2
#define ZMK_PTP_CONTACTS 4
#define ZMK_PTP_BODY_SIZE 27
#define ZMK_PTP_REPORT_ID(pad) (0x10 + (pad) * 0x10)
#define ZMK_PTP_CAPS_ID(pad) (ZMK_PTP_REPORT_ID(pad) + 1)
#define ZMK_PTP_CERT_ID(pad) (ZMK_PTP_REPORT_ID(pad) + 2)
#define ZMK_PTP_MODE_ID(pad) (ZMK_PTP_REPORT_ID(pad) + 3)
#define ZMK_PTP_SELECT_ID(pad) (ZMK_PTP_REPORT_ID(pad) + 4)
#define ZMK_PTP_LATENCY_ID(pad) (ZMK_PTP_REPORT_ID(pad) + 5)
#define ZMK_PTP_CONFIDENCE 1
#define ZMK_PTP_TIP 2
#define ZMK_PTP_COORD_MAX 32767

struct zmk_ptp_contact { uint8_t id; bool tip, confidence; uint16_t x, y; };
struct zmk_ptp_frame {
    /* Includes contacts with tip=false for their final UP report. */
    uint8_t count;
    uint16_t scan_time; /* 100 microsecond units, wraps at 65536. */
    struct zmk_ptp_contact contacts[ZMK_PTP_CONTACTS];
};
struct zmk_ptp_protocol { uint8_t mode, selective, latency; };
void zmk_ptp_protocol_init(struct zmk_ptp_protocol *state);
int zmk_ptp_feature_pad(uint8_t id);
/* Feature payloads exclude Report ID on both interfaces. Returns payload size. */
int zmk_ptp_feature_get(const struct zmk_ptp_protocol *state, uint8_t id,
                        uint8_t *data, size_t capacity);
int zmk_ptp_feature_set(struct zmk_ptp_protocol *state, uint8_t id,
                        const uint8_t *data, size_t length);
int zmk_ptp_encode(const struct zmk_ptp_frame *frame, uint8_t body[ZMK_PTP_BODY_SIZE]);
void zmk_ptp_encode_up(uint8_t body[ZMK_PTP_BODY_SIZE]);
