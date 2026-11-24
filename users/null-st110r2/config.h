// Copyright 2026 Yuki Rea <yuki@null-src.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define DYNAMIC_KEYMAP_LAYER_COUNT 8

// User EEPROM block size
// Stores protected macros, LED brightness settings, and PIN salt
#define EECONFIG_USER_DATA_SIZE 456

// Regular non-protected macros
#define DYNAMIC_KEYMAP_MACRO_COUNT 7

#define DEBOUNCE 5

// Magic keys
#define IS_COMMAND() (get_mods() == (MOD_BIT(KC_LSFT) | MOD_BIT(KC_RSFT)))

#define MOUSEKEY_INTERVAL 16
#define MOUSEKEY_DELAY 0
#define MOUSEKEY_TIME_TO_MAX 60
#define MOUSEKEY_MAX_SPEED 7
#define MOUSEKEY_WHEEL_DELAY 0

// Add to Magic Keys to jump to bootloader
#define MAGIC_KEY_BOOTLOADER PAUSE
