# users/null-st110r2

Shared code for the `*_null` keymaps of the [null] ST110r2.x keyboard (select with `USER_NAME := null-st110r2`).

* `null-st110r2.c` / `null-st110r2.h`: layer enum, PIN entry mode, key handling, QMK hooks.
* `led.c` / `led.h`: indicator LED animations and brightness control (AT90USB1286 timer 1 PWM, ST110r2 pins).
* `protected_macros.c` / `.h`: PIN lock and write-only encrypted VIA macros.
* `config.h`, `rules.mk`: shared settings (8 layers, VIA, user EEPROM block).

The LED code is specific to the ST110r2 hardware, so this userspace is only for that keyboard.

## PIN lock and protected macros

VIA macros M0-M8 are write-only. These macros can be set in VIA's Macros tab.
Once set, VIA will not be able to read back the encrypted macros.
M9-M15 are ordinary macros.

* **M0 is the PIN.** It is not stored, only a random salt and a verifier from PBKDF2-HMAC-SHA256
  are kept. When M0 is empty, the PIN lock feature is disabled.
* **M1-M8 are encrypted** (ChaCha20 + HMAC-SHA256 tag) with keys derived from the PIN, so a dump of
  the EEPROM, or firmware flashed through the bootloader, cannot read them without the PIN.
* **PIN entry**: Press the **PIN Entry key (FN + P)**, then type the PIN.
  While entering the PIN, the keyboard prevents keys from being sent to he host.
  When PIN entry mode is activated, all LEDs stay lit to indicate the mode is active.
  Esc cancels, and 10 seconds without a key cancels. Type the correct PIN to switch to later 7
  and decrypt protected macros. While decrypting, a loading animation plays on the LEDs.
  Once decrypted, protected macros can be played by pressing F1-F8.
* **VIA can change M1-M8 only while unlocked**, or in the same save that sets M0.
* **Setting or clearing M0 erases M1-M8**
* Saving a protected macro empty keeps the stored one.
  Saving `!clear` in M0 removes the PIN and disabled the PIN functionality.
  saving `!clear` in M1-M8 clears the macro.
* PIN: letters, digits and space. Each macro holds up to 39 bytes.

**Security notes:**
  Someone with physical access could dump the EEPROM and try to brute force PINs.
  Use a passphrase of 10+ characters rather than a short number.
  Text that a macro types is of course visible to the computer it is typed into.
