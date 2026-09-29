/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/keys.h>
#include <zmk/behavior.h>
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
#include <zmk/harbour_hid_suppression.h>
#endif

struct zmk_keycode_state_changed {
    uint16_t usage_page;
    uint32_t keycode;
    uint8_t implicit_modifiers;
    uint8_t explicit_modifiers;
    bool state;
    int64_t timestamp;
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
    uint32_t position;
    uint32_t harbour_epoch;
#endif
};

ZMK_EVENT_DECLARE(zmk_keycode_state_changed);

static inline struct zmk_keycode_state_changed
zmk_keycode_state_changed_from_encoded(uint32_t encoded, bool pressed, int64_t timestamp) {
    uint16_t page = ZMK_HID_USAGE_PAGE(encoded);
    uint16_t id = ZMK_HID_USAGE_ID(encoded);
    uint8_t implicit_modifiers = 0x00;
    uint8_t explicit_modifiers = 0x00;

    if (!page) {
        page = HID_USAGE_KEY;
    }

    if (is_mod(page, id)) {
        explicit_modifiers = SELECT_MODS(encoded);
    } else {
        implicit_modifiers = SELECT_MODS(encoded);
    }

    return (struct zmk_keycode_state_changed){.usage_page = page,
                                              .keycode = id,
                                              .implicit_modifiers = implicit_modifiers,
                                              .explicit_modifiers = explicit_modifiers,
                                              .state = pressed,
                                              .timestamp = timestamp};
}

static inline int raise_zmk_keycode_state_changed_from_encoded(uint32_t encoded, bool pressed,
                                                               int64_t timestamp) {
    return raise_zmk_keycode_state_changed(
        zmk_keycode_state_changed_from_encoded(encoded, pressed, timestamp));
}

#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
static inline int raise_zmk_keycode_state_changed_from_binding(
    uint32_t encoded, bool pressed, const struct zmk_behavior_binding_event *binding_event) {
    struct zmk_keycode_state_changed event =
        zmk_keycode_state_changed_from_encoded(encoded, pressed, binding_event->timestamp);
    event.position = binding_event->position;
    event.harbour_epoch = binding_event->harbour_epoch;
    if (event.harbour_epoch == 0 && binding_event->position < 50 &&
        zmk_harbour_hid_suppression_active()) {
        event.harbour_epoch = zmk_harbour_hid_current_epoch();
    }
    return raise_zmk_keycode_state_changed(event);
}
#endif
