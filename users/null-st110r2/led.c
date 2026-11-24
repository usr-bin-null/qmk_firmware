// Copyright 2026 Yuki Rea <yuki@null-src.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "led.h"
#include "null-st110r2.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>

// User EEPROM layout: see user_eeprom_t in protected_macros.h (EECONFIG_USER_DATA_SIZE in config.h)
#define LED_EEPROM_MAGIC 0xA6

// LED ANIMATION
#define led_on_helper(port, lpin) DDR ## port |=  (1<<lpin); PORT ## port |=  (1<<lpin)
#define led_on(x) led_on_helper(x)
#define led_off_helper(port, lpin) DDR ## port &=  ~(1<<lpin); PORT ## port &=  ~(1<<lpin)
#define led_off(x) led_off_helper(x)

// LED PIN DEFINITIONS
#define fn_led     D,4
#define keypad_led B,4
#define num_led    B,5
#define caps_led   D,7
#define left_led   B,7
#define center_led D,0
#define right_led  D,1

// LED BRIGHTNESS STEPS
#define LED_DITHER_STEPS 64
#define LED_LEVELS 16
#define LED_DEFAULT_LEVEL 11
static const uint8_t led_duty_table[LED_LEVELS] PROGMEM = {1, 2, 3, 5, 7, 9, 12, 15, 19, 23, 27, 32, 38, 45, 54, 64};

static volatile uint8_t led_level = LED_DEFAULT_LEVEL;
static volatile uint8_t led_duty  = 32;

#define PIN_BUSY_STEP_TICKS 2048 // Time between LEDs when pin_busy_animation it true (~125ms at 16.4 Khz)

#define SLEEP_LED_TIMER_TOP F_CPU/(256*64)

static uint16_t load_animation_counter = 0;
static bool keypad_led_animation = false;
static bool meta_led_animation   = false;
static bool caps_led_animation   = false;
static bool fn_led_animation     = false;
static bool load_led_animation   = false;
static volatile bool pin_busy_animation = false;
static volatile bool pin_entry_led = false;
static bool left_led_static   = false;
static bool center_led_static = false;
static bool right_led_static  = false;
static bool num_led_static    = false;

// LED ANIMATION TABLES
static const uint8_t blinking_table[64] PROGMEM = {
    255, 255, 255, 255, 255, 255, 255, 200, 100, 75,  50,  25,  10,  5,   0,   0,
    255, 255, 255, 255, 255, 255, 255, 200, 100, 75,  50,  25,  10,  5,   0,   0,
    255, 255, 255, 255, 255, 255, 255, 200, 100, 75,  50,  25,  10,  5,   0,   0,
    255, 255, 255, 255, 255, 255, 255, 200, 100, 75,  50,  25,  10,  5,   0,   0
};
static const uint8_t load_left_table[64] PROGMEM = {
    0,   0,   0,   0,   0,   0,   5,   10,  25,  50,  75,  100, 150, 200, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 200, 100, 75,  50,  25,  10,  5,   0
};
static const uint8_t load_center_table[64] PROGMEM = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   5,   10,  25,  50,  75,  100, 150, 200, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 200, 100, 75,  50,  25,  10,  5,   0
};
static const uint8_t load_right_table[64] PROGMEM = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   5,   10,  25,  50,  75,  100, 150, 200, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 200, 100, 75,  50,  25,  10,  5,   0
};

static void pwm_timer_init(void) {
    TCCR1B |= _BV(WGM12);
    TCCR1B |= _BV(CS10);
    uint8_t sreg = SREG;
    cli();
    OCR1AH = (SLEEP_LED_TIMER_TOP >> 8) & 0xff;
    OCR1AL = SLEEP_LED_TIMER_TOP & 0xff;
    SREG   = sreg;
}

static void pwm_timer_enable(void) { TIMSK1 |= _BV(OCIE1A); }

#define LED_SET_H(port, lpin, on) do { if (on) { led_on_helper(port, lpin); } else { led_off_helper(port, lpin); } } while (0)
#define LED_SET(x, on) LED_SET_H(x, on)

// PWM timer
ISR(TIMER1_COMPA_vect) {
    static union {
        uint16_t row;
        struct {
            uint8_t count : 8;
            uint8_t duration : 1;
            uint8_t index : 6;
        } pwm;
    } timer = {.row = 0};
    timer.row++;

    // LED BRIGHTNESS CONTROL
    const uint8_t c   = timer.pwm.count;
    const uint8_t rev = ((c & 1) << 5) | ((c & 2) << 3) | ((c & 4) << 1) | ((c & 8) >> 1) | ((c & 16) >> 3) | ((c & 32) >> 5);
    const bool gate   = rev < led_duty;
    bool left, center, right, num = false;

    if (pin_busy_animation) {
        static uint16_t busy_ticks = 0;
        if (++busy_ticks >= 3 * PIN_BUSY_STEP_TICKS) busy_ticks = 0;
        const uint8_t step = busy_ticks / PIN_BUSY_STEP_TICKS;
        left   = step == 0;
        center = step == 1;
        right  = step == 2;
    } else if (load_led_animation) {
        keypad_led_animation = false;
        caps_led_animation   = false;
        meta_led_animation   = false;
        fn_led_animation     = false;
        if (load_animation_counter == 0) { timer.row = 0; }
        left   = timer.pwm.count < pgm_read_byte(&load_left_table[timer.pwm.index]);
        center = timer.pwm.count < pgm_read_byte(&load_center_table[timer.pwm.index]);
        right  = timer.pwm.count < pgm_read_byte(&load_right_table[timer.pwm.index]);
        load_animation_counter++;
        if (load_animation_counter >= 32500) {
            load_animation_counter = 0;
            load_led_animation     = false;
        }
    } else {
        const bool blink = timer.pwm.count < pgm_read_byte(&blinking_table[timer.pwm.index]);
        left   = left_led_static   || (keypad_led_animation && blink);
        center = center_led_static || ((caps_led_animation || fn_led_animation) && blink);
        right  = right_led_static  || (meta_led_animation && blink);
        num    = num_led_static;
        if (pin_entry_led) { left = center = right = true; }
    }
    LED_SET(left_led, left && gate);
    LED_SET(center_led, center && gate);
    LED_SET(right_led, right && gate);
    LED_SET(num_led, num && gate);
}

void null_led_update(led_t host, layer_state_t state) {
    // RIGHT LED
    if (host.scroll_lock && !layer_state_cmp(state, L_GAME)) {
        meta_led_animation = false;
        right_led_static   = true;
    } else if (layer_state_cmp(state, L_GAME)) {
        meta_led_animation = true;
        right_led_static   = false;
    } else {
        meta_led_animation = false;
        right_led_static   = false;
    }
    // CENTER LED
    if (layer_state_cmp(state, L_FN1)) {
        fn_led_animation = true;
    } else {
        fn_led_animation = false;
        led_off(fn_led);
    }
    if (layer_state_cmp(state, L_FN2) || layer_state_cmp(state, L_FN3) || host.caps_lock) {
        if (layer_state_cmp(state, L_FN2) || layer_state_cmp(state, L_FN3)) {
            caps_led_animation = true;
            center_led_static  = false;
        } else {
            caps_led_animation = false;
            center_led_static  = true;
        }
    } else {
        caps_led_animation = false;
        center_led_static  = false;
        led_off(caps_led);
    }
    // LEFT LED
    left_led_static = num_led_static = host.num_lock && !layer_state_cmp(state, L_MOUSE) && !layer_state_cmp(state, L_HEX);
    keypad_led_animation = layer_state_cmp(state, L_HEX) || layer_state_cmp(state, L_MOUSE);
}

static void load_led_level(void) {
    user_eeprom_t ee;
    eeconfig_read_user_datablock(&ee, 0, sizeof(ee));
    if (ee.led_magic == LED_EEPROM_MAGIC && ee.led_level < LED_LEVELS) {
        led_level = ee.led_level;
    }
    led_duty = pgm_read_byte(&led_duty_table[led_level]);
}

static void save_led_level(void) {
    const uint8_t magic = LED_EEPROM_MAGIC;
    const uint8_t level = led_level;
    eeconfig_update_user_datablock(&magic, offsetof(user_eeprom_t, led_magic), sizeof(magic));
    eeconfig_update_user_datablock(&level, offsetof(user_eeprom_t, led_level), sizeof(level));
}

void null_led_init(void) {
    load_led_level();
    pwm_timer_init();
    pwm_timer_enable();
    load_led_animation = true;
}

void null_led_brightness_step(int8_t step) {
    int8_t level = (int8_t)led_level + step;
    if (level > LED_LEVELS - 1) level = LED_LEVELS - 1;
    if (level < 0) level = 0;
    if (level != led_level) {
        led_level = level;
        led_duty  = pgm_read_byte(&led_duty_table[level]);
        save_led_level();
    }
}

void null_led_pin_entry(bool active) { pin_entry_led = active; }
void null_led_pin_busy(bool busy) { pin_busy_animation = busy; }
