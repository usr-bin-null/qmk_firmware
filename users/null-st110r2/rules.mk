VIA_ENABLE = yes
COMMAND_ENABLE = yes

# Shared by the *_null keymaps (select with USER_NAME := null-st110r2 in the keymap's rules.mk)
# protected_macros.c: write only VIA macros on M0-M8 (M0 is used as PIN for Layer 7 where the read-only macros are activated).
SRC += null-st110r2.c led.c protected_macros.c
