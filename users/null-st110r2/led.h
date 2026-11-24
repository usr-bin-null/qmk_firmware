// Copyright 2026 Yuki Rea <yuki@null-src.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// This file has the same name as QMK's core quantum/led.h (led_t) and is found first in the include path, so include the core one explicitly.
#include "quantum/led.h"
#include "quantum.h"

// LED status animation and brightness control (ST110r2 LEDs on the AT90USB1286 timer 1 PWM)
void null_led_init(void);                                   // load brightness, start the PWM timer, play the startup animation
void null_led_update(led_t host, layer_state_t state);     // refresh indicator LEDs from host LED state and active layers
void null_led_brightness_step(int8_t step);                 // +1 / -1, clamped and saved to EEPROM
void null_led_pin_entry(bool active);                       // PIN entry mode: all indicator LEDs lit
void null_led_pin_busy(bool busy);                          // key derivation running: sweeping animation
