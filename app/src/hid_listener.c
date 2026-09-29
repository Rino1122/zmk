/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/endpoints.h>

#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
#include <string.h>
#include <zephyr/kernel.h>
#include <zmk/harbour_hid_suppression.h>
#include <zmk/virtual_key_position.h>

#define HARBOUR_PHYSICAL_POSITIONS 50U
#define HARBOUR_HELD_KEYCODES 128U

struct harbour_held_keycode {
    struct zmk_keycode_state_changed event;
    bool used;
    bool output;
};

static struct harbour_held_keycode harbour_held[HARBOUR_HELD_KEYCODES];
static uint32_t harbour_position_epoch[HARBOUR_PHYSICAL_POSITIONS];
static bool harbour_position_held[HARBOUR_PHYSICAL_POSITIONS];
static bool harbour_active;
static bool harbour_rebuild_mode;
static uint32_t harbour_epoch;
static int64_t harbour_last_end_ms;
static K_MUTEX_DEFINE(harbour_hid_lock);

__attribute__((weak)) void zmk_harbour_hid_mark_pending_hold_taps(uint32_t epoch) {
    ARG_UNUSED(epoch);
}

__attribute__((weak)) void zmk_harbour_hid_mark_pending_tap_dances(uint32_t epoch) {
    ARG_UNUSED(epoch);
}

static bool harbour_is_key_origin(uint32_t position) {
    return position < HARBOUR_PHYSICAL_POSITIONS ||
           (position >= ZMK_VIRTUAL_KEY_POSITION_COMBO(0) &&
            position < ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN));
}

bool zmk_harbour_hid_key_origin(uint32_t position) {
    return harbour_is_key_origin(position);
}

static int harbour_rebuild_reports(void) {
    uint8_t implicit_modifiers = 0;
    zmk_hid_keyboard_reset_state();
    zmk_hid_consumer_clear();
    for (size_t i = 0; i < ARRAY_SIZE(harbour_held); i++) {
        const struct harbour_held_keycode *held = &harbour_held[i];
        if (!held->used || !held->output) {
            continue;
        }
        uint32_t usage = ZMK_HID_USAGE(held->event.usage_page, held->event.keycode);
        zmk_hid_press(usage);
        zmk_hid_register_mods(held->event.explicit_modifiers);
        implicit_modifiers |= held->event.implicit_modifiers;
    }
    zmk_hid_implicit_modifiers_press(implicit_modifiers);
    int keyboard_err = zmk_endpoint_send_report(HID_USAGE_KEY);
    int consumer_err = zmk_endpoint_send_report(HID_USAGE_CONSUMER);
    return keyboard_err < 0 ? keyboard_err : consumer_err;
}

bool zmk_harbour_hid_suppression_active(void) { return harbour_active; }

uint32_t zmk_harbour_hid_current_epoch(void) { return harbour_active ? harbour_epoch : 0; }

uint32_t zmk_harbour_hid_epoch_for_queued(uint32_t position, int64_t queued_at) {
    if (!harbour_is_key_origin(position)) {
        return 0;
    }
    if (harbour_active || (harbour_last_end_ms != 0 && queued_at <= harbour_last_end_ms)) {
        return harbour_epoch;
    }
    return 0;
}

uint32_t zmk_harbour_hid_epoch_for_position(uint32_t position, bool pressed) {
    if (position >= HARBOUR_PHYSICAL_POSITIONS) {
        return 0;
    }
    uint32_t epoch;
    k_mutex_lock(&harbour_hid_lock, K_FOREVER);
    if (pressed) {
        harbour_position_held[position] = true;
        harbour_position_epoch[position] = harbour_active ? harbour_epoch : 0;
    }
    epoch = harbour_position_epoch[position];
    if (!pressed) {
        harbour_position_held[position] = false;
        harbour_position_epoch[position] = 0;
    }
    k_mutex_unlock(&harbour_hid_lock);
    return epoch;
}

bool zmk_harbour_hid_suppress_binding(const struct zmk_behavior_binding_event *event) {
    return event->harbour_epoch != 0 ||
           (harbour_active && harbour_is_key_origin(event->position));
}

int zmk_harbour_hid_suppression_start(void) {
    k_mutex_lock(&harbour_hid_lock, K_FOREVER);
    if (harbour_active) {
        k_mutex_unlock(&harbour_hid_lock);
        return 0;
    }
    harbour_epoch++;
    if (harbour_epoch == 0) {
        harbour_epoch++;
    }
    harbour_active = true;
    harbour_rebuild_mode = true;
    for (size_t i = 0; i < HARBOUR_PHYSICAL_POSITIONS; i++) {
        if (harbour_position_held[i]) {
            harbour_position_epoch[i] = harbour_epoch;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(harbour_held); i++) {
        if (harbour_held[i].used &&
            harbour_is_key_origin(harbour_held[i].event.position)) {
            harbour_held[i].output = false;
        }
    }
    zmk_harbour_hid_mark_pending_hold_taps(harbour_epoch);
    zmk_harbour_hid_mark_pending_tap_dances(harbour_epoch);
    int ret = harbour_rebuild_reports();
    zmk_harbour_hid_release_key_mouse_buttons();
    zmk_harbour_hid_stop_key_mouse_motion();
    k_mutex_unlock(&harbour_hid_lock);
    return ret;
}

void zmk_harbour_hid_suppression_stop(void) {
    k_mutex_lock(&harbour_hid_lock, K_FOREVER);
    if (harbour_active) {
        harbour_last_end_ms = k_uptime_get();
    }
    harbour_active = false;
    k_mutex_unlock(&harbour_hid_lock);
}

static void harbour_record_keycode(const struct zmk_keycode_state_changed *event) {
    int match = -1;
    int free_slot = -1;
    for (size_t i = 0; i < ARRAY_SIZE(harbour_held); i++) {
        if (!harbour_held[i].used) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        const struct zmk_keycode_state_changed *held = &harbour_held[i].event;
        if (held->position == event->position && held->usage_page == event->usage_page &&
            held->keycode == event->keycode) {
            match = i;
            break;
        }
    }
    if (!event->state) {
        if (match >= 0) {
            harbour_held[match].used = false;
        }
        return;
    }
    int slot = match >= 0 ? match : free_slot;
    if (slot < 0) {
        LOG_ERR("Harbour HID ledger full; releasing all keyboard reports");
        memset(harbour_held, 0, sizeof(harbour_held));
        harbour_rebuild_mode = true;
        harbour_rebuild_reports();
        return;
    }
    harbour_held[slot].used = true;
    harbour_held[slot].event = *event;
    harbour_held[slot].output =
        event->harbour_epoch == 0 && !(harbour_active && harbour_is_key_origin(event->position));
}
#endif

static int hid_listener_keycode_pressed(const struct zmk_keycode_state_changed *ev) {
    int err, explicit_mods_changed, implicit_mods_changed;

    if (!is_mod(ev->usage_page, ev->keycode) &&
        zmk_hid_is_pressed(ZMK_HID_USAGE(ev->usage_page, ev->keycode))) {
        LOG_DBG("unregistering usage_page 0x%02X keycode 0x%02X since it was already pressed",
                ev->usage_page, ev->keycode);
        err = zmk_hid_release(ZMK_HID_USAGE(ev->usage_page, ev->keycode));
        if (err < 0) {
            LOG_DBG("Unable to pre-release keycode (%d)", err);
            return err;
        }
        err = zmk_endpoint_send_report(ev->usage_page);
        if (err < 0) {
            LOG_ERR("Failed to send key report for pre-releasing keycode (%d)", err);
        }
    }

    LOG_DBG("usage_page 0x%02X keycode 0x%02X implicit_mods 0x%02X explicit_mods 0x%02X",
            ev->usage_page, ev->keycode, ev->implicit_modifiers, ev->explicit_modifiers);
    err = zmk_hid_press(ZMK_HID_USAGE(ev->usage_page, ev->keycode));
    if (err < 0) {
        LOG_DBG("Unable to press keycode");
        return err;
    }
    explicit_mods_changed = zmk_hid_register_mods(ev->explicit_modifiers);
    implicit_mods_changed = zmk_hid_implicit_modifiers_press(ev->implicit_modifiers);
    if (ev->usage_page != HID_USAGE_KEY &&
        (explicit_mods_changed > 0 || implicit_mods_changed > 0)) {
        err = zmk_endpoint_send_report(HID_USAGE_KEY);
        if (err < 0) {
            LOG_ERR("Failed to send key report for changed mofifiers for consumer page event (%d)",
                    err);
        }
    }

    return zmk_endpoint_send_report(ev->usage_page);
}

static int hid_listener_keycode_released(const struct zmk_keycode_state_changed *ev) {
    int err, explicit_mods_changed, implicit_mods_changed;

    LOG_DBG("usage_page 0x%02X keycode 0x%02X implicit_mods 0x%02X explicit_mods 0x%02X",
            ev->usage_page, ev->keycode, ev->implicit_modifiers, ev->explicit_modifiers);
    err = zmk_hid_release(ZMK_HID_USAGE(ev->usage_page, ev->keycode));
    if (err < 0) {
        LOG_DBG("Unable to release keycode");
        return err;
    }

#if IS_ENABLED(CONFIG_ZMK_HID_SEPARATE_MOD_RELEASE_REPORT)

    // send report of normal key release early to fix the issue
    // of some programs recognizing the implicit_mod release before the actual key release
    err = zmk_endpoint_send_report(ev->usage_page);
    if (err < 0) {
        LOG_ERR("Failed to send key report for the released keycode (%d)", err);
    }

#endif // IS_ENABLED(CONFIG_ZMK_HID_SEPARATE_MOD_RELEASE_REPORT)

    explicit_mods_changed = zmk_hid_unregister_mods(ev->explicit_modifiers);
    // There is a minor issue with this code.
    // If LC(A) is pressed, then LS(B), then LC(A) is released, the shift for B will be released
    // prematurely. This causes if LS(B) to repeat like Bbbbbbbb when pressed for a long time.
    // Solving this would require keeping track of which key's implicit modifiers are currently
    // active and only releasing modifiers at that time.
    implicit_mods_changed = zmk_hid_implicit_modifiers_release();

    if (ev->usage_page != HID_USAGE_KEY &&
        (explicit_mods_changed > 0 || implicit_mods_changed > 0)) {
        err = zmk_endpoint_send_report(HID_USAGE_KEY);
        if (err < 0) {
            LOG_ERR("Failed to send key report for changed mofifiers for consumer page event (%d)",
                    err);
        }
    }
    return zmk_endpoint_send_report(ev->usage_page);
}

int hid_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev) {
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
        k_mutex_lock(&harbour_hid_lock, K_FOREVER);
        harbour_record_keycode(ev);
        if (harbour_rebuild_mode) {
            harbour_rebuild_reports();
            k_mutex_unlock(&harbour_hid_lock);
            return 0;
        }
#endif
        if (ev->state) {
            hid_listener_keycode_pressed(ev);
        } else {
            hid_listener_keycode_released(ev);
        }
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
        k_mutex_unlock(&harbour_hid_lock);
#endif
    }
    return 0;
}

ZMK_LISTENER(hid_listener, hid_listener);
ZMK_SUBSCRIPTION(hid_listener, zmk_keycode_state_changed);
