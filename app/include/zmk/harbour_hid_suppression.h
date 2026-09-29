/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct zmk_behavior_binding_event;

/* Central-only hooks used by the Bretagne Harbour Studio subsystem. */
int zmk_harbour_hid_suppression_start(void);
void zmk_harbour_hid_suppression_stop(void);
bool zmk_harbour_hid_suppression_active(void);
uint32_t zmk_harbour_hid_current_epoch(void);
uint32_t zmk_harbour_hid_epoch_for_queued(uint32_t position, int64_t queued_at);
uint32_t zmk_harbour_hid_epoch_for_position(uint32_t position, bool pressed);
bool zmk_harbour_hid_suppress_binding(const struct zmk_behavior_binding_event *event);
bool zmk_harbour_hid_key_origin(uint32_t position);
void zmk_harbour_hid_mouse_key_button(bool key_origin, uint8_t button, bool pressed);
void zmk_harbour_hid_release_key_mouse_buttons(void);
void zmk_harbour_hid_stop_key_mouse_motion(void);
void zmk_harbour_hid_mark_pending_hold_taps(uint32_t epoch);
void zmk_harbour_hid_mark_pending_tap_dances(uint32_t epoch);
