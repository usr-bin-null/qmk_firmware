// Copyright 2026 Yuki Rea <yuki@null-src.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "quantum.h"

/* Protected macros
 *
 * VIA macros M0-M8 are "write only": they can be typed into the VIA editor but VIA can never
 * read them back (it always sees them as empty). M9 and up are ordinary VIA macros.
 *
 * M0 is the PIN. It is NOT stored: only a random salt and a 16-byte verifier derived with
 * PBKDF2-HMAC-SHA256 are kept.
 * M1-M8 are encrypted (ChaCha20) and authenticated (HMAC-SHA256, 8 byte tag) with keys derived
 * from the PIN, so they cannot be decrypted without it, even from a dump of the EEPROM.
 *
 *  - The PIN is entered after the PIN entry key (PIN_ENT, FN + P): the keyboard swallows the keys
 *    typed while entering it, so nothing is sent to the computer. When all of its keys were
 *    entered it is checked; if right, layer 7 switches on (F1-F8 = M1-M8) and the keys stay in RAM
 *    for PM_UNLOCK_TIMEOUT_MS or until the keyboard is unplugged. M1-M8 only type while unlocked.
 *  - VIA can only change M1-M8 while the keyboard is unlocked, or in the same save that sets M0.
 *  - Setting or clearing M0 erases M1-M8 (they then type nothing).
 *  - Saving a protected macro empty in VIA keeps the stored one. Saving "!clear" in M0 removes
 *    the PIN (PIN lock off); saving "!clear" in M1-M8 clears the macro.
 *  - M0 itself cannot be typed or read back by anything.
 */
#define PM_SLOTS 9      // M0-M8 (virtual macros reserved at the start of VIA's macro list)
#define PM_STORED (PM_SLOTS - 1) // M1-M8 are stored (M0 only exists as salt/verifier)
#define PM_SLOT_LEN 40  // plaintext bytes per macro including the terminating NUL

typedef struct __attribute__((packed)) {
    uint8_t nonce[4];   // little endian write counter, 0 = erased slot
    uint8_t tag[8];     // HMAC-SHA256 (truncated) over slot, nonce and ciphertext
    uint8_t data[PM_SLOT_LEN];
} pm_slot_t;

// Everything kept in the user EEPROM datablock
typedef struct __attribute__((packed)) {
    uint8_t   led_magic;        // LED_EEPROM_MAGIC once led_level has been saved
    uint8_t   led_level;
    uint8_t   pm_magic;         // set once the protected macros are initialised
    uint8_t   pm_pin_len;       // number of keys in the PIN, 0 = no PIN
    uint8_t   pm_salt[16];
    uint8_t   pm_verifier[16];  // proves a typed PIN is right
    pm_slot_t pm_slot[PM_STORED];
} user_eeprom_t;

typedef enum { PM_IDLE, PM_UNLOCKED, PM_REJECTED } pm_result_t;

void    pm_init(void);
pm_result_t pm_task(void);                     // call from housekeeping: auto-lock, and verify an entered PIN
void    pm_send(uint8_t id);                   // type protected macro id (1..PM_STORED) if unlocked
void    pm_entry_begin(void);                  // start a PIN entry (forget keys entered so far)
bool    pm_entry_key(uint16_t keycode);        // enter a key; true once all keys are in (pm_task() then verifies)
uint8_t pm_pin_length(void);                   // number of keys in the PIN (0 = PIN lock disabled)
void    pm_busy(bool busy);                    // hook, called around the slow key derivation (weak default does nothing)
