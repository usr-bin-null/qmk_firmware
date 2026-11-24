// Copyright 2018 Yuki Rea <yuki@null-src.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "quantum.h"
#include "protected_macros.h"

#define _______ KC_TRNS               // Transparent key
#define xxxxxxx KC_NO                 // Disable key
#define SHFTHEX LT(L_HEX, KC_PENT)    // Tap = Enter | Hold = HEX keypad layer
#define FN1_APP LT(L_FN1, KC_APP)     // Tap = App Menu | Hold = Function layer 1
#define CAPS_FN LT(L_FN2, KC_CAPS)    // Tap = Caps Lock | Hold = Function layer 2
#define SAFECAP LSFT(KC_CAPS)         // Left Shift + Caps Lock to prevent IME switching

// Enumerate Layers
enum layers {
    L_BASE,
    L_HEX,
    L_GAME,
    L_MOUSE,
    L_FN1,
    L_FN2,
    L_FN3,
    L_PIN,
};

// CUSTOM KEYCODES
enum custom_keycodes {
    PIN_ENT = SAFE_RANGE, // Start PIN entry.
			  // Set this keycode on any key you wish to be used to trigger PIN entry for the protected macros function. 
};
