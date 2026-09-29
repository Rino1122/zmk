/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mouse_key_press

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
#include <zmk/harbour_hid_suppression.h>
#endif
#include <zmk/hid.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {.display_name = "MB1", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB1},
    {.display_name = "MB2", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB2},
    {.display_name = "MB3", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB3},
    {.display_name = "MB4", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB4},
    {.display_name = "MB5", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB5}};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif

static void process_key_state(const struct device *dev, int32_t val, bool pressed,
                              uint32_t position) {
    for (int i = 0; i < ZMK_HID_MOUSE_NUM_BUTTONS; i++) {
        if (val & BIT(i)) {
            WRITE_BIT(val, i, 0);
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
            zmk_harbour_hid_mouse_key_button(zmk_harbour_hid_key_origin(position), i, pressed);
#else
            input_report_key(dev, INPUT_BTN_0 + i, pressed ? 1 : 0, val == 0, K_FOREVER);
#endif
        }
    }
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
    if (zmk_harbour_hid_suppress_binding(&event)) {
        return 0;
    }
#endif
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    process_key_state(zmk_behavior_get_binding(binding->behavior_dev), binding->param1, true,
                      event.position);

    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_ZMK_HARBOUR_KEY_TRIAL)
    if (zmk_harbour_hid_suppress_binding(&event)) {
        return 0;
    }
#endif
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    process_key_state(zmk_behavior_get_binding(binding->behavior_dev), binding->param1, false,
                      event.position);

    return 0;
}

static const struct behavior_driver_api behavior_mouse_key_press_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define MKP_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_mouse_key_press_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MKP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
