/* SPDX-License-Identifier: MIT */
#pragma once
#include <zmk/endpoints_types.h>
#include <zmk/pointing/ptp_protocol.h>

/* USB callback evidence retained since MCU boot, independent of log visibility.
 * requested/length are setup wLength and callback-provided/received byte length. */
struct zmk_ptp_usb_diagnostics {
    uint32_t gets, sets, mode_sets, resets;
    uint8_t get_mask, set_mask, last_id, data0, data1, mode_value;
    uint16_t requested, length;
    int result, mode_result;
};
struct zmk_ptp_status {
    struct zmk_ptp_protocol protocol;
    uint32_t generation, sent, dropped;
    int last_error;
    struct zmk_ptp_usb_diagnostics usb;
};
int zmk_ptp_get_status(struct zmk_endpoint_instance endpoint, unsigned pad,
                       struct zmk_ptp_status *status);
int zmk_ptp_get_input(struct zmk_endpoint_instance endpoint, unsigned pad,
                      uint8_t body[ZMK_PTP_BODY_SIZE]);
int zmk_ptp_get_feature(struct zmk_endpoint_instance endpoint, uint8_t id, uint8_t *data,
                        size_t capacity);
int zmk_ptp_set_feature(struct zmk_endpoint_instance endpoint, uint8_t id, const uint8_t *data,
                        size_t length);
/* Nonblocking, bounded queue. generation is from get_status at gesture start.
 * A stale generation, mode change, or overflow cancels the gesture. */
int zmk_ptp_submit(struct zmk_endpoint_instance endpoint, unsigned pad, uint32_t generation,
                   const struct zmk_ptp_frame *frame);
void zmk_ptp_reset(struct zmk_endpoint_instance endpoint);
void zmk_ptp_cancel(struct zmk_endpoint_instance endpoint, unsigned pad);
/* Transport adapters: exactly one complete body, no BLE Report ID prefix. */
int zmk_usb_hid_send_ptp_report(unsigned pad, const uint8_t body[ZMK_PTP_BODY_SIZE]);
int zmk_hog_send_ptp_report(struct zmk_endpoint_instance endpoint, unsigned pad,
                            const uint8_t body[ZMK_PTP_BODY_SIZE]);

void zmk_ptp_note_usb_feature(uint8_t id, bool write, uint16_t requested, size_t length,
                              const uint8_t *data, int result);
