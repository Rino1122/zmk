/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zmk/pointing/ptp_compat.h>
/* Same report IDs/byte lengths as PTP, entirely vendor-defined usages. */
int zmk_ptp_compat_vendor_map(uint8_t *out, size_t length) {
    static const uint8_t map[] = {0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x26, 0xff,
                                  0x00, 0x75, 0x08, 0x85, 0x10, 0x09, 0x10, 0x95, 28,   0x81, 0x02,
                                  0x85, 0x11, 0x09, 0x11, 0x95, 2,    0xb1, 0x02, 0x85, 0x12, 0x09,
                                  0x12, 0x96, 0x00, 0x01, 0xb1, 0x02, 0x85, 0x13, 0x09, 0x13, 0x95,
                                  1,    0xb1, 0x02, 0x85, 0x14, 0x09, 0x14, 0x95, 1,    0xb1, 0x02,
                                  0x85, 0x15, 0x09, 0x15, 0x95, 1,    0xb1, 0x02, 0xc0};
    if (!out || length < sizeof(map) || ((length - sizeof(map)) & 1))
        return -EINVAL;
    memcpy(out, map, sizeof(map));
    /* Balanced global PUSH/POP padding keeps the USB descriptor length stable.
     * No additional collections or reports are introduced. */
    for (size_t i = sizeof(map); i < length; i += 2) {
        out[i] = 0xa4;
        out[i + 1] = 0xb4;
    }
    return length;
}
