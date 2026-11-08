/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
enum zmk_output_diag_stage {
 ZMK_OD_RX, ZMK_OD_LOCAL, ZMK_OD_LOCAL_AGE, ZMK_OD_LOCAL_CONSUME, ZMK_OD_RIGHT_QUEUE, ZMK_OD_ACK, ZMK_OD_ACK_CALL, ZMK_OD_ACK_RACE, ZMK_OD_WORK,
 ZMK_OD_BLE_ENQUEUE, ZMK_OD_BLE_QUEUE, ZMK_OD_BLE_NOTIFY, ZMK_OD_BLE_COMPLETE,
 ZMK_OD_BLE_DROP, ZMK_OD_USB_RETURN, ZMK_OD_USB_WAIT, ZMK_OD_USB_WRITE,
 ZMK_OD_USB_COMPLETE, ZMK_OD_INPUT, ZMK_OD_STAGES
};
#if defined(CONFIG_ZMK_OUTPUT_DIAGNOSTICS)
/* Optional sink supplied by the board. Packed session/time tokens are opaque. */
extern uint32_t zmk_output_diag_stamp(void) __attribute__((weak));
extern void zmk_output_diag_note(enum zmk_output_diag_stage stage,uint32_t token,int result) __attribute__((weak));
static inline uint32_t zmk_od_stamp(void){return zmk_output_diag_stamp?zmk_output_diag_stamp():0;}
static inline void zmk_od_note(enum zmk_output_diag_stage stage,uint32_t token,int result){if(token&&zmk_output_diag_note)zmk_output_diag_note(stage,token,result);}
#else
static inline uint32_t zmk_od_stamp(void){return 0;}
static inline void zmk_od_note(enum zmk_output_diag_stage stage,uint32_t token,int result){(void)stage;(void)token;(void)result;}
#endif
