/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zmk/endpoints_types.h>
#define ZMK_PTP_COMPAT_AUTO 0
#define ZMK_PTP_COMPAT_MOUSE 1
#define ZMK_PTP_COMPAT_TOGGLE 2
struct zmk_ptp_compat_status {
    bool ready, saved_mouse, applied_mouse, changing;
    int error;
};
/* Equal-length vendor-only fallback. Legacy keyboard/mouse HID is separate. */
int zmk_ptp_compat_vendor_map(uint8_t *out, size_t length);
#ifdef CONFIG_ZMK_PTP_COMPAT
bool zmk_ptp_compat_forced(struct zmk_endpoint_instance ep);
bool zmk_ptp_compat_changing(struct zmk_endpoint_instance ep);
int zmk_ptp_compat_get(struct zmk_endpoint_instance ep, struct zmk_ptp_compat_status *out);
/* Nonblocking admission. Persisted change is applied by restarting the left MCU. */
int zmk_ptp_compat_request(struct zmk_endpoint_instance ep, unsigned operation);
#else
#include <errno.h>
static inline bool zmk_ptp_compat_forced(struct zmk_endpoint_instance ep) {
    (void)ep;
    return false;
}
static inline bool zmk_ptp_compat_changing(struct zmk_endpoint_instance ep) {
    (void)ep;
    return false;
}
static inline int zmk_ptp_compat_get(struct zmk_endpoint_instance ep,
                                     struct zmk_ptp_compat_status *out) {
    (void)ep;
    if (!out)
        return -EINVAL;
    *out = (struct zmk_ptp_compat_status){.ready = true};
    return 0;
}
static inline int zmk_ptp_compat_request(struct zmk_endpoint_instance ep, unsigned operation) {
    (void)ep;
    (void)operation;
    return -ENOTSUP;
}
#endif
