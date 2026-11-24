// Copyright 2018 Yuki Rea <yuki@null-src.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "null-st110r2.h"
#include "led.h"
#include "dynamic_keymap.h"

_Static_assert(sizeof(user_eeprom_t) <= EECONFIG_USER_DATA_SIZE, "user EEPROM block too small");

// PIN LOCK FUNCTION
#define PIN_ENTRY_TIMEOUT_MS 10000 

static bool          pin_entry_active = false;
static bool          pin_entry_verifying = false;
static uint32_t      pin_entry_t;

static void pin_entry_end(void) {
    pin_entry_active    = false;
    null_led_pin_entry(false);
    pin_entry_verifying = false;
    pm_entry_begin();
}

static void pin_entry_start(void) {
    clear_keys();
    send_keyboard_report();
    pm_entry_begin();
    pin_entry_verifying = false;
    pin_entry_active    = true;
    null_led_pin_entry(true);
    pin_entry_t         = timer_read32();
}

bool led_update_user(led_t led_state) {
    null_led_update(led_state, layer_state);
    return false;
}

layer_state_t layer_state_set_user(layer_state_t state) {
    null_led_update(host_keyboard_led_state(), state);
    return state;
}

void keyboard_post_init_user(void) {
    null_led_init();
    pm_init();
}

void pm_busy(bool busy) {
    null_led_pin_busy(busy);
}

void housekeeping_task_user(void) {
    switch (pm_task()) {
        case PM_UNLOCKED:
            pin_entry_end();
            layer_on(L_PIN);
            break;
        case PM_REJECTED:
            pin_entry_end();
            break;
        default:
            break;
    }
    if (pin_entry_active && !pin_entry_verifying && timer_elapsed32(pin_entry_t) > PIN_ENTRY_TIMEOUT_MS) {
        pin_entry_end();
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (keycode == BL_UP || keycode == BL_DOWN) {
        if (record->event.pressed) null_led_brightness_step(keycode == BL_UP ? 1 : -1);
        return false;
    }

    // PIN ENTRY
    if (keycode == PIN_ENT) {
        if (record->event.pressed && pm_pin_length() > 0 && !pin_entry_active) {
            pin_entry_start();
        }
        return false;
    }
    if (pin_entry_active && keycode >= KC_A && keycode < KC_LCTL) { 
        if (record->event.pressed && !pin_entry_verifying) {
            pin_entry_t = timer_read32();
            if (keycode == KC_ESC) {
                pin_entry_end();
            } else if (pm_entry_key(keycode)) {
                pin_entry_verifying = true;
            }
        }
        return false;
    }

    // MACROS
    if (IS_MACRO_KEYCODE(keycode)) {
        if (record->event.pressed) {
            uint8_t id = keycode - QK_MACRO_0;
            if (IS_LAYER_ON(L_PIN)) layer_off(L_PIN);
            if (id < PM_SLOTS) {
                pm_send(id);
            } else if (id - PM_SLOTS < dynamic_keymap_macro_get_count()) {
                dynamic_keymap_macro_send(id - PM_SLOTS);
            }
        }
        return false;
    }
    return true;
}
