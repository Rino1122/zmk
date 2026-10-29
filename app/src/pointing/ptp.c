/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/pointing/ptp.h>
LOG_MODULE_REGISTER(zmk_ptp, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BLE)
#define ENDPOINT_COUNT (1 + ZMK_BLE_PROFILE_COUNT)
#else
#define ENDPOINT_COUNT 1
#endif
struct pad_state {
    bool initialized, release_pending;
    struct zmk_ptp_status status;
    uint8_t last[ZMK_PTP_BODY_SIZE];
};
static struct pad_state states[ENDPOINT_COUNT][ZMK_PTP_PAD_COUNT];
static struct k_spinlock state_lock;
struct queued_frame {
    struct zmk_endpoint_instance endpoint;
    uint32_t generation;
    uint8_t pad, body[ZMK_PTP_BODY_SIZE];
};
K_MSGQ_DEFINE(ptp_frames, sizeof(struct queued_frame), 8, 4);
static struct k_work_q ptp_queue;
K_THREAD_STACK_DEFINE(ptp_stack, 2048);
static struct k_work_delayable send_work;

static int endpoint_index(struct zmk_endpoint_instance ep) {
    if (ep.transport == ZMK_TRANSPORT_USB)
        return 0;
#if IS_ENABLED(CONFIG_ZMK_BLE)
    if (ep.transport == ZMK_TRANSPORT_BLE && ep.ble.profile_index >= 0 &&
        ep.ble.profile_index < ZMK_BLE_PROFILE_COUNT)
        return 1 + ep.ble.profile_index;
#endif
    return -1;
}
static struct zmk_endpoint_instance endpoint_for(unsigned index) {
    return index ? (struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_BLE,
                                                  .ble = {.profile_index = index - 1}}
                 : (struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_USB};
}
/* Called with state_lock. */
static struct pad_state *state_for(int index, unsigned pad) {
    struct pad_state *s = &states[index][pad];
    if (!s->initialized) {
        zmk_ptp_protocol_init(&s->status.protocol);
        s->status.generation = 1;
        s->initialized = true;
    }
    return s;
}
static void invalidate(struct pad_state *s) {
    if (!++s->status.generation)
        s->status.generation = 1;
    s->release_pending = true;
}
static int transport_send(struct zmk_endpoint_instance ep, unsigned pad, const uint8_t *body) {
    switch (ep.transport) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    case ZMK_TRANSPORT_USB:
        return zmk_usb_hid_send_ptp_report(pad, body);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
    case ZMK_TRANSPORT_BLE:
        return zmk_hog_send_ptp_report(ep, pad, body);
#endif
    default:
        return -ENOTCONN;
    }
}
static void schedule(void) { k_work_reschedule_for_queue(&ptp_queue, &send_work, K_NO_WAIT); }

static void send_pending(struct k_work *work) {
    ARG_UNUSED(work);
    bool retry = false;
    /* Releases bypass the finite movement queue and retain the last coordinates. */
    for (unsigned i = 0; i < ENDPOINT_COUNT; i++)
        for (unsigned pad = 0; pad < 2; pad++) {
            uint8_t up[ZMK_PTP_BODY_SIZE];
            uint32_t generation;
            k_spinlock_key_t key = k_spin_lock(&state_lock);
            struct pad_state *s = state_for(i, pad);
            if (!s->release_pending) {
                k_spin_unlock(&state_lock, key);
                continue;
            }
            memcpy(up, s->last, sizeof(up));
            generation = s->status.generation;
            k_spin_unlock(&state_lock, key);
            zmk_ptp_encode_up(up);
            int err = up[26] ? transport_send(endpoint_for(i), pad, up) : 0;
            key = k_spin_lock(&state_lock);
            s->status.last_error = err;
            if (!err || err == -ENOTCONN || err == -ENODEV || err == -EACCES) {
                memset(s->last, 0, sizeof(s->last));
                if (s->status.generation == generation)
                    s->release_pending = false;
            } else {
                retry = true;
            }
            k_spin_unlock(&state_lock, key);
        }
    struct queued_frame frame;
    while (k_msgq_peek(&ptp_frames, &frame) == 0) {
        int index = endpoint_index(frame.endpoint);
        k_spinlock_key_t key = k_spin_lock(&state_lock);
        struct pad_state *s = state_for(index, frame.pad);
        bool valid = s->status.generation == frame.generation && !s->release_pending &&
                     s->status.protocol.mode == 3 && (s->status.protocol.selective & 1);
        if (!valid)
            s->status.dropped++;
        k_spin_unlock(&state_lock, key);
        if (!valid) {
            k_msgq_get(&ptp_frames, &frame, K_NO_WAIT);
            continue;
        }
        int err = transport_send(frame.endpoint, frame.pad, frame.body);
        if (err == -EAGAIN || err == -EBUSY || err == -ENOMEM) {
            retry = true;
            break;
        }
        k_msgq_get(&ptp_frames, &frame, K_NO_WAIT);
        key = k_spin_lock(&state_lock);
        s->status.last_error = err;
        if (!err) {
            memcpy(s->last, frame.body, sizeof(s->last));
            s->status.sent++;
            /* If the host changed mode during transport_send, clear this report next. */
            if (s->status.generation != frame.generation) {
                s->release_pending = true;
                retry = true;
            }
        } else {
            s->status.dropped++;
            invalidate(s);
            retry = true;
        }
        k_spin_unlock(&state_lock, key);
        if (err)
            LOG_WRN("PTP ep=%d pad=%u send=%d; gesture cancelled", index, frame.pad, err);
    }
    if (retry)
        k_work_reschedule_for_queue(&ptp_queue, &send_work, K_MSEC(8));
}
int zmk_ptp_get_status(struct zmk_endpoint_instance ep, unsigned pad, struct zmk_ptp_status *out) {
    int index = endpoint_index(ep);
    if (index < 0 || pad >= 2 || !out)
        return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    *out = state_for(index, pad)->status;
    k_spin_unlock(&state_lock, key);
    return 0;
}
int zmk_ptp_get_input(struct zmk_endpoint_instance ep, unsigned pad,
                      uint8_t body[ZMK_PTP_BODY_SIZE]) {
    int index = endpoint_index(ep);
    if (index < 0 || pad >= 2 || !body)
        return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    memcpy(body, state_for(index, pad)->last, ZMK_PTP_BODY_SIZE);
    k_spin_unlock(&state_lock, key);
    return 0;
}
int zmk_ptp_get_feature(struct zmk_endpoint_instance ep, uint8_t id, uint8_t *data, size_t cap) {
    int index = endpoint_index(ep), pad = zmk_ptp_feature_pad(id);
    if (index < 0 || pad < 0)
        return -ENOTSUP;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    int err = zmk_ptp_feature_get(&state_for(index, pad)->status.protocol, id, data, cap);
    k_spin_unlock(&state_lock, key);
    LOG_INF("PTP GET ep=%d id=0x%02x len=%d", index, id, err);
    return err;
}
int zmk_ptp_set_feature(struct zmk_endpoint_instance ep, uint8_t id, const uint8_t *data,
                        size_t len) {
    int index = endpoint_index(ep), pad = zmk_ptp_feature_pad(id);
    if (index < 0 || pad < 0)
        return -ENOTSUP;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    struct pad_state *s = state_for(index, pad);
    struct zmk_ptp_protocol before = s->status.protocol;
    int err = zmk_ptp_feature_set(&s->status.protocol, id, data, len);
    if (!err && (before.mode != s->status.protocol.mode ||
                 before.selective != s->status.protocol.selective))
        invalidate(s);
    k_spin_unlock(&state_lock, key);
    if (!err)
        schedule();
    LOG_INF("PTP SET ep=%d id=0x%02x len=%u value=%u err=%d", index, id, (unsigned)len,
            len && data ? data[0] : 0, err);
    return err;
}
int zmk_ptp_submit(struct zmk_endpoint_instance ep, unsigned pad, uint32_t generation,
                   const struct zmk_ptp_frame *frame) {
    int index = endpoint_index(ep);
    if (index < 0 || pad >= 2)
        return -EINVAL;
    struct queued_frame queued = {.endpoint = ep, .pad = pad, .generation = generation};
    int err = zmk_ptp_encode(frame, queued.body);
    if (err)
        return err;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    struct pad_state *s = state_for(index, pad);
    if (s->status.generation != generation)
        err = -ESTALE;
    else if (s->status.protocol.mode != 3 || !(s->status.protocol.selective & 1))
        err = -EACCES;
    else if (s->release_pending)
        err = -EAGAIN;
    else {
        err = k_msgq_put(&ptp_frames, &queued, K_NO_WAIT);
        if (err) {
            s->status.dropped++;
            invalidate(s);
        }
    }
    k_spin_unlock(&state_lock, key);
    schedule();
    return err;
}
void zmk_ptp_cancel(struct zmk_endpoint_instance ep, unsigned pad) {
    int index = endpoint_index(ep);
    if (index < 0 || pad >= 2)
        return;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    invalidate(state_for(index, pad));
    k_spin_unlock(&state_lock, key);
    schedule();
}
void zmk_ptp_reset(struct zmk_endpoint_instance ep) {
    int index = endpoint_index(ep);
    if (index < 0)
        return;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    for (unsigned pad = 0; pad < 2; pad++) {
        struct pad_state *s = state_for(index, pad);
        invalidate(s);
        zmk_ptp_protocol_init(&s->status.protocol);
    }
    k_spin_unlock(&state_lock, key);
    schedule();
}
static struct zmk_endpoint_instance previous;
static int endpoint_listener(const zmk_event_t *event) {
    const struct zmk_endpoint_changed *changed = as_zmk_endpoint_changed(event);
    if (changed) {
        /* Keep the live connection's negotiation when merely selecting it again. */
        for (unsigned pad = 0; pad < 2; pad++)
            zmk_ptp_cancel(previous, pad);
        previous = changed->endpoint;
        for (unsigned pad = 0; pad < 2; pad++)
            zmk_ptp_cancel(previous, pad);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ptp_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(ptp_endpoint, zmk_endpoint_changed);
static int init(void) {
    k_work_init_delayable(&send_work, send_pending);
    k_work_queue_start(&ptp_queue, ptp_stack, K_THREAD_STACK_SIZEOF(ptp_stack), K_PRIO_PREEMPT(10),
                       NULL);
    return 0;
}
SYS_INIT(init, APPLICATION, 50);
