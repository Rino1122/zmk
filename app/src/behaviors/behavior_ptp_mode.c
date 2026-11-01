/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT zmk_behavior_ptp_mode
#include <drivers/behavior.h>
#include <errno.h>
#include <zephyr/device.h>
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/pointing/ptp_compat.h>
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
/* Apply on release: a held mode key must not retrigger after the central reboots.
 * Capture the target on press even if the output changes before release. */
static struct held_mode {
    bool active;
    uint32_t position;
    struct zmk_endpoint_instance endpoint;
    unsigned operation;
} held[4];
static int pressed(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    if (binding->param1 > ZMK_PTP_COMPAT_TOGGLE)
        return -EINVAL;
    for (unsigned i = 0; i < ARRAY_SIZE(held); i++)
        if (held[i].active && held[i].position == event.position)
            return ZMK_BEHAVIOR_OPAQUE;
    for (unsigned i = 0; i < ARRAY_SIZE(held); i++)
        if (!held[i].active) {
            held[i] = (struct held_mode){.active = true,
                                         .position = event.position,
                                         .endpoint = zmk_endpoint_get_selected(),
                                         .operation = binding->param1};
            return ZMK_BEHAVIOR_OPAQUE;
        }
    return -EBUSY;
}
static int released(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    for (unsigned i = 0; i < ARRAY_SIZE(held); i++)
        if (held[i].active && held[i].position == event.position) {
            struct held_mode request = held[i];
            held[i].active = false;
            return zmk_ptp_compat_request(request.endpoint, request.operation);
        }
    return ZMK_BEHAVIOR_OPAQUE;
}
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata values[] = {
    {.value = 0,
     .display_name = "Automatic PTP / Mouse",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE},
    {.value = 1, .display_name = "Force Mouse", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE},
    {.value = 2,
     .display_name = "Toggle Auto / Mouse",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE}};
static const struct behavior_parameter_metadata_set set = {.param1_values = values,
                                                           .param1_values_len = ARRAY_SIZE(values)};
static const struct behavior_parameter_metadata metadata = {.sets_len = 1, .sets = &set};
#endif
static const struct behavior_driver_api api = {
    .binding_pressed = pressed,
    .binding_released = released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};
BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &api);
#endif
