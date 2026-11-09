/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/pointing/ptp.h>
#include <zmk/pointing/ptp_compat.h>
#define COMPAT_COUNT (1 + ZMK_BLE_PROFILE_COUNT)
BUILD_ASSERT(COMPAT_COUNT < 31);
static atomic_t saved_modes, applied_modes, pending_change, transition_active, storage_ready;
static atomic_t last_error[COMPAT_COUNT];
struct compat_job {
    struct zmk_endpoint_instance ep;
    unsigned operation;
};
K_MSGQ_DEFINE(compat_jobs, sizeof(struct compat_job), 1, 4);
static struct k_work_q compat_queue;
K_THREAD_STACK_DEFINE(compat_stack, 2048);
static struct k_work compat_work;
static int index_for(struct zmk_endpoint_instance ep) {
    if (ep.transport == ZMK_TRANSPORT_USB)
        return 0;
    if (ep.transport == ZMK_TRANSPORT_BLE && ep.ble.profile_index >= 0 &&
        ep.ble.profile_index < ZMK_BLE_PROFILE_COUNT)
        return ep.ble.profile_index + 1;
    return -EINVAL;
}
bool zmk_ptp_compat_forced(struct zmk_endpoint_instance ep) {
    int i = index_for(ep);
    return i < 0 || !atomic_get(&storage_ready) || !!(atomic_get(&applied_modes) & BIT(i));
}
bool zmk_ptp_compat_changing(struct zmk_endpoint_instance ep) {
    int i = index_for(ep);
    return i >= 0 && !!atomic_get(&transition_active);
}
int zmk_ptp_compat_get(struct zmk_endpoint_instance ep, struct zmk_ptp_compat_status *out) {
    int i = index_for(ep);
    if (i < 0 || !out)
        return -EINVAL;
    *out = (struct zmk_ptp_compat_status){.ready = atomic_get(&storage_ready),
                                          .saved_mouse = !!(atomic_get(&saved_modes) & BIT(i)),
                                          .applied_mouse = zmk_ptp_compat_forced(ep),
                                          .changing = zmk_ptp_compat_changing(ep),
                                          .error = atomic_get(&last_error[i])};
    return 0;
}
static int compat_load(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (!*name || *name < '0' || *name > '9')
        return -ENOENT;
    char *end;
    unsigned long i = strtoul(name, &end, 10);
    if (*end || i >= COMPAT_COUNT || len != 2)
        return -EINVAL;
    uint8_t value[2];
    ssize_t n = read_cb(arg, value, sizeof(value));
    if (n != sizeof(value) || value[0] != 1 || value[1] > 1)
        return -EINVAL;
    if (value[1])
        atomic_or(&saved_modes, BIT(i));
    else
        atomic_and(&saved_modes, ~BIT(i));
    /* h_set also runs during the later full settings load. Never change a live
     * descriptor then: early init is the only point that seeds applied modes. */
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(ptp_compat, "ptp/compat", NULL, compat_load, NULL, NULL);
static void apply_job(const struct compat_job *job) {
    int i = index_for(job->ep);
    bool desired = job->operation == ZMK_PTP_COMPAT_TOGGLE ? !(atomic_get(&saved_modes) & BIT(i))
                                                           : job->operation == ZMK_PTP_COMPAT_MOUSE;
    if (desired == !!(atomic_get(&applied_modes) & BIT(i)) &&
        desired == !!(atomic_get(&saved_modes) & BIT(i))) {
        atomic_clear(&last_error[i]);
        atomic_clear(&pending_change);
        return;
    }
    uint8_t value[] = {1, desired};
    char key[32];
    snprintk(key, sizeof(key), "ptp/compat/%d", i);
    int err = settings_save_one(key, value, sizeof(value));
    if (err) {
        atomic_set(&last_error[i], err);
        atomic_clear(&pending_change);
        return;
    }
    if (desired)
        atomic_or(&saved_modes, BIT(i));
    else
        atomic_and(&saved_modes, ~BIT(i));
    if (desired == !!(atomic_get(&applied_modes) & BIT(i))) {
        atomic_clear(&last_error[i]);
        atomic_clear(&pending_change);
        return;
    }
    atomic_clear(&last_error[i]);
    atomic_set(&transition_active, 1);
    zmk_ptp_notify_changed();
    /* All host links will disconnect on reboot. Release PTP and selected legacy
     * reports first; applied modes remain immutable for every live connection. */
    zmk_ptp_cancel((struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_USB}, 0);
    for (unsigned slot = 0; slot < ZMK_BLE_PROFILE_COUNT; slot++)
        zmk_ptp_cancel((struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_BLE,
                                                      .ble = {.profile_index = slot}},
                       0);
    zmk_endpoint_clear_reports();
    zmk_endpoint_send_mouse_report();
    /* This dedicated worker never occupies the keyboard or system work queue. */
    k_sleep(K_MSEC(150));
    /* BT slot/output preference normally use delayed saves; flush them so a
     * just-selected host is not lost during this intentional restart. */
    err = zmk_ble_flush_active_profile();
    if (!err)
        err = zmk_endpoint_flush_preference();
    if (err) {
        atomic_set(&last_error[i], err);
        atomic_clear(&transition_active);
    zmk_ptp_notify_changed();
        atomic_clear(&pending_change);
        return;
    }
    sys_reboot(SYS_REBOOT_COLD);
    /* A platform that returns from reboot has not applied the new descriptor. */
    atomic_set(&last_error[i], -EIO);
    atomic_clear(&transition_active);
    zmk_ptp_notify_changed();
    atomic_clear(&pending_change);
}
static void compat_worker(struct k_work *work) {
    struct compat_job job;
    while (!k_msgq_get(&compat_jobs, &job, K_NO_WAIT))
        apply_job(&job);
}
int zmk_ptp_compat_request(struct zmk_endpoint_instance ep, unsigned operation) {
    if (index_for(ep) < 0 || operation > ZMK_PTP_COMPAT_TOGGLE)
        return -EINVAL;
    if (!atomic_get(&storage_ready))
        return -EAGAIN;
    if (!atomic_cas(&pending_change, 0, 1))
        return -EBUSY;
    struct compat_job job = {.ep = ep, .operation = operation};
    if (k_msgq_put(&compat_jobs, &job, K_NO_WAIT)) {
        atomic_clear(&pending_change);
        return -EAGAIN;
    }
    k_work_submit_to_queue(&compat_queue, &compat_work);
    return 0;
}
static int compat_init(void) {
    k_work_init(&compat_work, compat_worker);
    k_work_queue_start(&compat_queue, compat_stack, K_THREAD_STACK_SIZEOF(compat_stack),
                       K_PRIO_PREEMPT(11), NULL);
    int err = settings_subsys_init();
    if (!err)
        err = settings_load_subtree("ptp/compat");
    if (err) {
        for (unsigned i = 0; i < COMPAT_COUNT; i++)
            atomic_set(&last_error[i], err);
        return err;
    }
    atomic_set(&applied_modes, atomic_get(&saved_modes));
    atomic_set(&storage_ready, 1);
    zmk_ptp_notify_changed();
    return 0;
}
/* USB HID registration is 95, USB enable is 96. BLE waits for the full settings
 * load in main before advertising, so this subtree is ready before any host. */
BUILD_ASSERT(CONFIG_ZMK_USB_HID_INIT_PRIORITY > 94);
SYS_INIT(compat_init, APPLICATION, 94);
