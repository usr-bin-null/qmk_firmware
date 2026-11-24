// Copyright 2026 Yuki Rea <yuki@null-src.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "protected_macros.h"
#include "via.h"
#include "raw_hid.h"
#include "dynamic_keymap.h"
#include "send_string.h"
#include "timer.h"
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(user_eeprom_t) <= EECONFIG_USER_DATA_SIZE, "user EEPROM block too small");

#define PM_MAGIC 0xC3
#define PM_CLEAR_TEXT "!clear"
// PBKDF2 iterations. Each costs two SHA-256 compressions.
// Keyboard will stall for ~1 second at unlock.
#ifndef PM_KDF_ITERATIONS
#    define PM_KDF_ITERATIONS 128
#endif
#ifndef PM_UNLOCK_TIMEOUT_MS
#    define PM_UNLOCK_TIMEOUT_MS (5UL * 60UL * 1000UL)
#endif

#define PM_OFF(field) offsetof(user_eeprom_t, field)
#define PM_SLOT_OFF(slot) (PM_OFF(pm_slot) + ((slot)-1) * sizeof(pm_slot_t)) // slot 1..PM_STORED

// SHA-256, HMAC-SHA256, PBKDF2, ChaCha20
static const uint32_t sha_k[64] PROGMEM = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

typedef struct {
    uint32_t h[8];
    uint8_t  buf[64];
    uint8_t  n;
    uint32_t len;
} sha_t;

static inline uint32_t ror32(uint32_t x, uint8_t n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(uint32_t *st, const uint8_t *p) {
    uint32_t w[16];
    for (uint8_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (uint8_t i = 0; i < 64; i++) {
        uint32_t wi;
        if (i < 16) {
            wi = w[i];
        } else {
            uint32_t x  = w[(i + 1) & 15];
            uint32_t y  = w[(i + 14) & 15];
            uint32_t s0 = ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3);
            uint32_t s1 = ror32(y, 17) ^ ror32(y, 19) ^ (y >> 10);
            wi          = w[i & 15] + s0 + w[(i + 9) & 15] + s1;
            w[i & 15]   = wi;
        }
        uint32_t t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + pgm_read_dword(&sha_k[i]) + wi;
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sha_init(sha_t *s) {
    static const uint32_t iv[8] PROGMEM = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (uint8_t i = 0; i < 8; i++) s->h[i] = pgm_read_dword(&iv[i]);
    s->n   = 0;
    s->len = 0;
}

static void sha_update(sha_t *s, const uint8_t *d, uint8_t len) {
    s->len += len;
    while (len--) {
        s->buf[s->n++] = *d++;
        if (s->n == 64) {
            sha_block(s->h, s->buf);
            s->n = 0;
        }
    }
}

static void sha_final(sha_t *s, uint8_t out[32]) {
    uint32_t bits = s->len << 3; 
    s->buf[s->n++] = 0x80;
    if (s->n > 56) {
        while (s->n < 64) s->buf[s->n++] = 0;
        sha_block(s->h, s->buf);
        s->n = 0;
    }
    while (s->n < 56) s->buf[s->n++] = 0;
    for (uint8_t i = 0; i < 4; i++) s->buf[56 + i] = 0;
    s->buf[60] = bits >> 24;
    s->buf[61] = bits >> 16;
    s->buf[62] = bits >> 8;
    s->buf[63] = bits;
    sha_block(s->h, s->buf);
    for (uint8_t i = 0; i < 8; i++) {
        out[4 * i]     = s->h[i] >> 24;
        out[4 * i + 1] = s->h[i] >> 16;
        out[4 * i + 2] = s->h[i] >> 8;
        out[4 * i + 3] = s->h[i];
    }
}

typedef struct {
    sha_t in, out;
} hmac_t;

static void hmac_init(hmac_t *h, const uint8_t *key, uint8_t keylen) {
    uint8_t k[64] = {0};
    memcpy(k, key, keylen);
    for (uint8_t i = 0; i < 64; i++) k[i] ^= 0x36;
    sha_init(&h->in);
    sha_update(&h->in, k, 64);
    for (uint8_t i = 0; i < 64; i++) k[i] ^= 0x36 ^ 0x5c;
    sha_init(&h->out);
    sha_update(&h->out, k, 64);
    memset(k, 0, sizeof(k));
}

static void hmac_run(const hmac_t *h, const uint8_t *msg, uint8_t len, uint8_t out[32]) {
    sha_t   c = h->in;
    uint8_t inner[32];
    sha_update(&c, msg, len);
    sha_final(&c, inner);
    c = h->out;
    sha_update(&c, inner, 32);
    sha_final(&c, out);
}

// PBKDF2-HMAC-SHA256
static void pbkdf2(const uint8_t *pw, uint8_t pwlen, const uint8_t *salt, uint8_t saltlen, uint16_t iterations, uint8_t out[32]) {
    hmac_t  h;
    uint8_t u[32], t[32], msg[20];
    hmac_init(&h, pw, pwlen);
    memcpy(msg, salt, saltlen);
    msg[saltlen] = msg[saltlen + 1] = msg[saltlen + 2] = 0;
    msg[saltlen + 3] = 1;
    hmac_run(&h, msg, saltlen + 4, u);
    memcpy(t, u, 32);
    for (uint16_t i = 1; i < iterations; i++) {
        hmac_run(&h, u, 32, u);
        for (uint8_t j = 0; j < 32; j++) t[j] ^= u[j];
    }
    memcpy(out, t, 32);
    memset(&h, 0, sizeof(h));
    memset(u, 0, sizeof(u));
    memset(t, 0, sizeof(t));
}

static void sha_labelled(const char *label, const uint8_t k[32], uint8_t out[32]) {
    sha_t s;
    sha_init(&s);
    sha_update(&s, (const uint8_t *)label, 3);
    sha_update(&s, k, 32);
    sha_final(&s, out);
    memset(&s, 0, sizeof(s));
}

#define QR(a, b, c, d)                                                    \
    a += b; d ^= a; d = (d << 16) | (d >> 16);                            \
    c += d; b ^= c; b = (b << 12) | (b >> 20);                            \
    a += b; d ^= a; d = (d << 8) | (d >> 24);                             \
    c += d; b ^= c; b = (b << 7) | (b >> 25);

static void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t x[16], s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    memcpy(&s[4], key, 32);
    s[12] = counter;
    memcpy(&s[13], nonce, 12);
    memcpy(x, s, sizeof(s));
    for (uint8_t i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    for (uint8_t i = 0; i < 16; i++) x[i] += s[i];
    memcpy(out, x, 64); // little endian
    memset(x, 0, sizeof(x));
    memset(s, 0, sizeof(s));
}

// UNLOCKED STATE - Keys are stored in RAM when unlocked
static uint8_t  pin_len = 0;       // cached copies of what is stored in EEPROM
static uint8_t  pin_salt[16];
static uint8_t  typed[PM_SLOT_LEN]; // last typed keys, newest last
static uint8_t  typed_n = 0;

static bool     unlocked = false;
static uint32_t unlock_t;
static uint8_t  enc_key[32];
static uint8_t  mac_key[32];

static void pm_lock(void) {
    unlocked = false;
    memset(enc_key, 0, sizeof(enc_key));
    memset(mac_key, 0, sizeof(mac_key));
}

static void pm_touch(void) { unlock_t = timer_read32(); }

static bool    verify_pending = false; // entered keys are verified in pm_task()

static void derive(const uint8_t *codes, uint8_t n, const uint8_t *salt, uint8_t *enc, uint8_t *mac, uint8_t *ver16);

pm_result_t pm_task(void) {
    if (unlocked && timer_elapsed32(unlock_t) > PM_UNLOCK_TIMEOUT_MS) pm_lock();
    if (!verify_pending) return PM_IDLE;
    verify_pending = false;
    uint8_t enc[32], mac[32], ver[16], stored[16];
    derive(typed, pin_len, pin_salt, enc, mac, ver);
    eeconfig_read_user_datablock(stored, PM_OFF(pm_verifier), 16);
    bool ok = memcmp(ver, stored, 16) == 0;
    if (ok) {
        memcpy(enc_key, enc, 32);
        memcpy(mac_key, mac, 32);
        unlocked = true;
        pm_touch();
    }
    memset(enc, 0, sizeof(enc));
    memset(mac, 0, sizeof(mac));
    pm_entry_begin(); // clear the entered keys
    return ok ? PM_UNLOCKED : PM_REJECTED;
}

// CHAR_TO_KEYCODE - Converts PIN to Keycodes
static uint8_t char_to_keycode(uint8_t c) {
    if (c >= 'a' && c <= 'z') return KC_A + (c - 'a');
    if (c >= 'A' && c <= 'Z') return KC_A + (c - 'A');
    if (c >= '1' && c <= '9') return KC_1 + (c - '1');
    if (c == '0') return KC_0;
    if (c == ' ') return KC_SPC;
    return 0;
}

// TEXT_TO_KEYS
static uint8_t text_to_keys(const uint8_t *buf, uint8_t *codes) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < PM_SLOT_LEN && buf[i]; i++) {
        uint8_t kc = 0;
        if (buf[i] == SS_QMK_PREFIX) { // VIA special key sequences
            uint8_t op = buf[++i];
            if (op == SS_TAP_CODE) {
                kc = buf[++i];
            } else if (op == SS_DOWN_CODE || op == SS_UP_CODE) {
                i++;
            } else if (op == SS_DELAY_CODE) {
                while (i < PM_SLOT_LEN && buf[i] && buf[i] != '|') i++;
            }
        } else {
            kc = char_to_keycode(buf[i]);
        }
        if (kc && n < PM_SLOT_LEN - 1) codes[n++] = kc;
    }
    return n;
}

__attribute__((weak)) void pm_busy(bool busy) { (void)busy; }

static void derive(const uint8_t *codes, uint8_t n, const uint8_t *salt, uint8_t *enc, uint8_t *mac, uint8_t *ver16) {
    uint8_t k[32], v[32];
    pm_busy(true);
    pbkdf2(codes, n, salt, 16, PM_KDF_ITERATIONS, k);
    sha_labelled("enc", k, enc);
    sha_labelled("mac", k, mac);
    sha_labelled("ver", k, v);
    memcpy(ver16, v, 16);
    memset(k, 0, sizeof(k));
    memset(v, 0, sizeof(v));
    pm_busy(false);
}

// SLOT_TAG - Macro slots M1-M8
static void slot_tag(uint8_t slot, const pm_slot_t *ps, uint8_t tag8[8]) {
    hmac_t  h;
    uint8_t msg[1 + 4 + PM_SLOT_LEN], full[32];
    msg[0] = slot;
    memcpy(&msg[1], ps->nonce, 4);
    memcpy(&msg[5], ps->data, PM_SLOT_LEN);
    hmac_init(&h, mac_key, 32);
    hmac_run(&h, msg, sizeof(msg), full);
    memcpy(tag8, full, 8);
    memset(&h, 0, sizeof(h));
}

static void slot_crypt(uint8_t slot, const uint8_t nonce4[4], uint8_t *data) {
    uint8_t n12[12] = {0}, ks[64];
    n12[0] = slot;
    memcpy(&n12[4], nonce4, 4);
    chacha20_block(enc_key, 1, n12, ks);
    for (uint8_t i = 0; i < PM_SLOT_LEN; i++) data[i] ^= ks[i];
    memset(ks, 0, sizeof(ks));
}

// PM_WIPE
static void pm_wipe(uint8_t slot) {
    pm_slot_t ps;
    memset(&ps, 0, sizeof(ps));
    eeconfig_update_user_datablock(&ps, PM_SLOT_OFF(slot), sizeof(ps)); // only write bytes that have changed
}

// PM_STORE
static void pm_store(uint8_t slot, const uint8_t *text, uint8_t len) {
    pm_slot_t ps;
    uint32_t  ctr;
    eeconfig_read_user_datablock(&ctr, PM_SLOT_OFF(slot) + offsetof(pm_slot_t, nonce), 4);
    ctr++;
    if (ctr == 0) ctr = 1;
    memcpy(ps.nonce, &ctr, 4);
    memset(ps.data, 0, PM_SLOT_LEN);
    if (len > PM_SLOT_LEN - 1) len = PM_SLOT_LEN - 1;
    memcpy(ps.data, text, len);
    slot_crypt(slot, ps.nonce, ps.data);
    slot_tag(slot, &ps, ps.tag);
    eeconfig_update_user_datablock(&ps, PM_SLOT_OFF(slot), sizeof(ps));
    memset(&ps, 0, sizeof(ps));
}

// PM_LOAD
static void pm_load(uint8_t slot, uint8_t *out) {
    pm_slot_t ps;
    memset(out, 0, PM_SLOT_LEN);
    eeconfig_read_user_datablock(&ps, PM_SLOT_OFF(slot), sizeof(ps));
    uint32_t ctr;
    memcpy(&ctr, ps.nonce, 4);
    if (ctr == 0) return; // erase
    uint8_t tag[8];
    slot_tag(slot, &ps, tag);
    uint8_t diff = 0;
    for (uint8_t i = 0; i < 8; i++) diff |= tag[i] ^ ps.tag[i];
    if (diff) return; // on wrong PIN do nothing
    slot_crypt(slot, ps.nonce, ps.data);
    memcpy(out, ps.data, PM_SLOT_LEN);
    out[PM_SLOT_LEN - 1] = 0;
    memset(&ps, 0, sizeof(ps));
}

// PM_SEND
void pm_send(uint8_t id) {
    if (id < 1 || id > PM_STORED || !unlocked) return;
    uint8_t buf[PM_SLOT_LEN];
    pm_load(id, buf);
    pm_touch();
    send_string((const char *)buf);
    memset(buf, 0, sizeof(buf));
}

// PM_SAVE_PIN - MACRO M0
static void pm_save_pin(uint8_t n, const uint8_t *ver) { // stores pin_salt, the verifier and the length
    pin_len = n;
    eeconfig_update_user_datablock(pin_salt, PM_OFF(pm_salt), 16);
    eeconfig_update_user_datablock(ver, PM_OFF(pm_verifier), 16);
    eeconfig_update_user_datablock(&pin_len, PM_OFF(pm_pin_len), 1);
    for (uint8_t i = 1; i <= PM_STORED; i++) pm_wipe(i); // nothing stored under the old PIN survives
}

static void pm_set_pin(const uint8_t *text) {
    uint8_t codes[PM_SLOT_LEN];
    uint8_t n = text_to_keys(text, codes);
    if (n == 0) return; // nothing typeable: ignore
    uint32_t s = timer_read32() * 2654435761u;
    s ^= ((uint32_t)TCNT0 << 24) ^ ((uint32_t)TCNT1 << 8) ^ ((uint32_t)TCNT3 << 16);
    for (uint8_t i = 0; i < 16; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        pin_salt[i] = pin_salt[i] ^ (uint8_t)(s >> 8) ^ (uint8_t)i;
    }
    uint8_t ver[16];
    pm_lock();
    derive(codes, n, pin_salt, enc_key, mac_key, ver);
    unlocked  = true;
    pm_touch();
    pm_save_pin(n, ver);
    memset(codes, 0, sizeof(codes));
}

static void pm_clear_pin(void) {
    uint8_t zero[16] = {0};
    pm_lock();
    memset(pin_salt, 0, sizeof(pin_salt));
    pm_save_pin(0, zero);
}

void pm_init(void) {
    uint8_t magic;
    eeconfig_read_user_datablock(&magic, PM_OFF(pm_magic), 1);
    if (magic != PM_MAGIC) {
        pm_clear_pin(); // Clear all macro slots to protect values when the PIN is set. Prevents someone from setting a new PIN to gain access to protected macros. 
        magic = PM_MAGIC;
        eeconfig_update_user_datablock(&magic, PM_OFF(pm_magic), 1);
    }
    eeconfig_read_user_datablock(&pin_len, PM_OFF(pm_pin_len), 1);
    eeconfig_read_user_datablock(pin_salt, PM_OFF(pm_salt), 16);
    if (pin_len > PM_SLOT_LEN - 1) pin_len = 0;
}

uint8_t pm_pin_length(void) { return pin_len; }

// MC_ENTRY_BEGIN - PIN entry, does not send keycodes to host
void pm_entry_begin(void) {
    typed_n = 0;
    memset(typed, 0, sizeof(typed));
}

// PM_ENTRY_KEY - Return true when pin_len keys were entered
bool pm_entry_key(uint16_t keycode) {
    if (pin_len == 0 || verify_pending) return false;
    if (keycode >= KC_P1 && keycode <= KC_P9) keycode = KC_1 + (keycode - KC_P1);
    if (keycode == KC_P0) keycode = KC_0;
    if (keycode < KC_A || keycode > KC_SPC || keycode == KC_BSPC) return false; // only keys that can be part of a PIN count
    typed[typed_n++] = keycode;
    if (typed_n < pin_len) return false;
    verify_pending = true;
    return true;
}

static uint8_t  wr_nuls;   // protected macros
static uint16_t wr_prefix; // bytes of the protected prefix
static uint16_t wr_next;   // next offset
static uint8_t  wr_buf[PM_SLOT_LEN];
static uint8_t  wr_len;

// WR_FINISH_ENTRY - clear or write M1-M8
static void wr_finish_entry(void) {
    if (wr_len > 0) {
        bool clr = wr_len == sizeof(PM_CLEAR_TEXT) - 1 && memcmp(wr_buf, PM_CLEAR_TEXT, wr_len) == 0;
        if (wr_nuls == 0) {
            if (clr) {
                pm_clear_pin();
            } else {
                pm_set_pin(wr_buf);
            }
        } else if (unlocked) { // M1-M8 can only be written while unlocked
            pm_store(wr_nuls, wr_buf, clr ? 0 : wr_len);
            pm_touch();
        }
    }
    wr_nuls++;
    wr_len = 0;
    memset(wr_buf, 0, sizeof(wr_buf));
}

// REAL_ACCESS - VIA macro buffer (M9+)
static void real_access(bool write, uint16_t real_off, uint8_t n, uint8_t *data) {
    uint16_t real_size = dynamic_keymap_macro_get_buffer_size();
    if (real_off >= real_size) return;
    if (real_off + n > real_size) n = real_size - real_off;
    if (write) {
        dynamic_keymap_macro_set_buffer(real_off, n, data);
    } else {
        dynamic_keymap_macro_get_buffer(real_off, n, data);
    }
}

// PM_WRITE
static void pm_write(uint16_t offset, uint8_t size, uint8_t *data) {
    if (offset == 0) { // Start of new save
        wr_nuls   = 0;
        wr_prefix = 0;
        wr_len    = 0;
        memset(wr_buf, 0, sizeof(wr_buf));
    } else if (offset != wr_next && wr_nuls < PM_SLOTS) {
        wr_next = offset + size;
        return;
    }
    wr_next = offset + size;

    uint8_t i = 0;
    for (; i < size && wr_nuls < PM_SLOTS; i++) {
        wr_prefix++;
        if (data[i] == 0) {
            wr_finish_entry();
        } else if (wr_len < PM_SLOT_LEN - 1) {
            wr_buf[wr_len++] = data[i];
        }
    }
    if (i < size && offset + i >= wr_prefix) real_access(true, offset + i - wr_prefix, size - i, &data[i]);
}

bool via_command_kb(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case id_dynamic_keymap_macro_get_count:
            data[1] = dynamic_keymap_macro_get_count() + PM_SLOTS;
            raw_hid_send(data, length);
            return true;
        case id_dynamic_keymap_macro_get_buffer_size: {
            uint16_t size = dynamic_keymap_macro_get_buffer_size() + PM_SLOTS;
            data[1]       = size >> 8;
            data[2]       = size & 0xFF;
            raw_hid_send(data, length);
            return true;
        }
        case id_dynamic_keymap_macro_get_buffer: {
            uint16_t offset = (data[1] << 8) | data[2];
            uint8_t  size   = data[3]; // <= 28
            uint8_t *out    = &data[4];
            memset(out, 0, size);
            uint8_t skip = offset < PM_SLOTS ? PM_SLOTS - offset : 0; // Read back protected entries as empty to VIA
            if (skip < size) real_access(false, offset + skip - PM_SLOTS, size - skip, out + skip);
            raw_hid_send(data, length);
            return true;
        }
        case id_dynamic_keymap_macro_set_buffer: {
            uint16_t offset = (data[1] << 8) | data[2];
            uint8_t  size   = data[3]; // <= 28
            pm_write(offset, size, &data[4]);
            raw_hid_send(data, length); // VIA expects the request echoed back
            return true;
        }
    }
    return false;
}
