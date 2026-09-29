#include "ssd1306.h"

#include <string.h>

#include "font5x7.h"
#include "oled_bus.h"

_Static_assert(FONT_W == OLED_CHAR_W, "font metrics and ssd1306.h disagree");
_Static_assert(FONT_ADVANCE == OLED_CHAR_ADV, "font metrics and ssd1306.h disagree");

// fb[page * OLED_W + x], bit (y & 7): bit 0 of each byte is the top row of the
// page. That is the layout the chip expects in horizontal addressing mode, so
// the flush is a single linear write with no reshuffling.
static uint8_t fb[OLED_FB_LEN];
static uint8_t fb_sent[OLED_FB_LEN];
static bool    fb_sent_valid;
static bool    present;

static bool cmd(uint8_t c) {
    const uint8_t buf[2] = {0x00, c};
    return oled_bus_write(buf, sizeof buf);
}

bool ssd1306_init(void) {
    present = false;
    fb_sent_valid = false;
    memset(fb, 0, sizeof fb);

    if (!oled_bus_init()) {
        return false;
    }

    // The two values that differ between the 128x32 and 128x64 variants are
    // the multiplex ratio and the COM pin config: get either wrong and the
    // image comes out doubled or on alternate rows, the classic mistake on
    // this panel. Everything else in the sequence is identical, confirmed
    // against the 0.96" 128x64 module's own datasheet (same SSD1306
    // controller family, fitted to the 3U panel because the 128x32 one would
    // read sideways on it).
    static const uint8_t seq[] = {
        0xAE,              // display off
        0xD5, 0x80,        // clock divide / oscillator
#if OLED_H == 64
        0xA8, 0x3F,        // multiplex ratio = 64-1
#else
        0xA8, 0x1F,        // multiplex ratio = 32-1
#endif
        0xD3, 0x00,        // display offset
        0x40,              // start line = 0
        0x8D, 0x14,        // charge pump on (internal supply)
        0x20, 0x00,        // horizontal addressing
        0xA1,              // segment remap: column 127 -> SEG0
        0xC8,              // COM scan descending
#if OLED_H == 64
        0xDA, 0x12,        // COM pins: alternative, no remap
#else
        0xDA, 0x02,        // COM pins: sequential, no remap
#endif
        0x81, 0x8F,        // contrast
        0xD9, 0xF1,        // precharge
        0xDB, 0x40,        // VCOMH deselect
        0xA4,              // resume from RAM content
        0xA6,              // normal display, not inverted
        0x2E,              // scrolling off
        0xAF,              // display on
    };
    for (size_t i = 0; i < sizeof seq; i++) {
        if (!cmd(seq[i])) {
            return false;
        }
    }

    present = true;
    ssd1306_flush();
    return true;
}

void ssd1306_set_contrast(uint8_t value) {
    if (!present) {
        return;
    }
    cmd(0x81);
    cmd(value);
}

bool ssd1306_present(void) { return present; }

const uint8_t *ssd1306_framebuffer(void) { return fb; }

void ssd1306_clear(void) { memset(fb, 0, sizeof fb); }

void ssd1306_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) {
        return;
    }
    uint8_t *p = &fb[(y / 8) * OLED_W + x];
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) {
        *p |= mask;
    } else {
        *p = (uint8_t)(*p & ~mask);
    }
}

void ssd1306_fill(int x, int y, int w, int h, bool on) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            ssd1306_pixel(xx, yy, on);
        }
    }
}

void ssd1306_bitmap(int x, int y, int w, int h, const uint8_t *bits) {
    int stride = (w + 7) / 8;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            uint8_t byte = bits[yy * stride + xx / 8];
            bool on = (byte >> (7 - (xx & 7))) & 1u;
            ssd1306_pixel(x + xx, y + yy, on);
        }
    }
}

// Draws a glyph. Outside the font range it falls back to '?' instead of
// skipping the character: a string that silently gets shorter throws the
// alignment out of register and gives no clue about what happened.
static void glyph(int x, int y, char c, bool on) {
    unsigned idx = (unsigned char)c;
    if (idx < FONT_FIRST || idx > FONT_LAST) {
        idx = '?';
    }
    const uint8_t *col = font5x7[idx - FONT_FIRST];
    for (int i = 0; i < FONT_W; i++) {
        for (int row = 0; row < FONT_H; row++) {
            if (col[i] & (1u << row)) {
                ssd1306_pixel(x + i, y + row, on);
            }
        }
    }
}

void ssd1306_text(int x, int y, const char *s, bool on) {
    ssd1306_text_trunc(x, y, s, -1, on);
}

void ssd1306_text_trunc(int x, int y, const char *s, int max_chars, bool on) {
    if (s == NULL) {
        return;
    }
    for (int i = 0; s[i] != '\0'; i++) {
        if (max_chars >= 0 && i >= max_chars) {
            break;
        }
        glyph(x + i * FONT_ADVANCE, y, s[i], on);
    }
}

int ssd1306_text_w(const char *s) {
    if (s == NULL || *s == '\0') {
        return 0;
    }
    int n = (int)strlen(s);
    return n * FONT_ADVANCE - (FONT_ADVANCE - FONT_W);
}

// Same shape as glyph(), each source pixel drawn as a 2x2 block instead of
// one, through ssd1306_fill() rather than ssd1306_pixel() to say so.
static void glyph_2x(int x, int y, char c, bool on) {
    unsigned idx = (unsigned char)c;
    if (idx < FONT_FIRST || idx > FONT_LAST) {
        idx = '?';
    }
    const uint8_t *col = font5x7[idx - FONT_FIRST];
    for (int i = 0; i < FONT_W; i++) {
        for (int row = 0; row < FONT_H; row++) {
            if (col[i] & (1u << row)) {
                ssd1306_fill(x + i * 2, y + row * 2, 2, 2, on);
            }
        }
    }
}

void ssd1306_text_2x(int x, int y, const char *s, bool on) {
    if (s == NULL) {
        return;
    }
    for (int i = 0; s[i] != '\0'; i++) {
        glyph_2x(x + i * FONT_ADVANCE * 2, y, s[i], on);
    }
}

int ssd1306_text_2x_w(const char *s) {
    if (s == NULL || *s == '\0') {
        return 0;
    }
    int n = (int)strlen(s);
    return 2 * (n * FONT_ADVANCE - (FONT_ADVANCE - FONT_W));
}

static bool page_dirty(int page) {
    if (!fb_sent_valid) {
        return true;
    }
    return memcmp(&fb[page * OLED_W], &fb_sent[page * OLED_W], OLED_W) != 0;
}

// Sends only the pages that changed, merging contiguous ones into a single
// write.
//
// This is not only about bandwidth: the I2C write blocks core0, and with it
// the encoder sampling. A whole frame is 11.6ms of blindness, which is enough
// to lose a few quadrature transitions when the knob is spun fast. Changing a
// slot touches two or three rows out of four, so the window shrinks to a good
// third of that.
bool ssd1306_flush(void) {
    if (!present) {
        return false;
    }

    bool sent_any = false;
    int page = 0;

    while (page < OLED_PAGES) {
        if (!page_dirty(page)) {
            page++;
            continue;
        }

        int end = page;
        while (end < OLED_PAGES && page_dirty(end)) {
            end++;
        }
        size_t bytes = (size_t)(end - page) * OLED_W;

        const uint8_t window[] = {0x00,  0x21, 0, OLED_W - 1,
                                  0x22,  (uint8_t)page, (uint8_t)(end - 1)};
        if (!oled_bus_write(window, sizeof window)) {
            return sent_any;
        }

        // Static, not on the stack: on the 128x64 build this is 1025 bytes
        // against a 2048-byte core0 stack (PICO_STACK_SIZE), and the frame
        // measured 1088 bytes with the call chain reaching ~70% of the stack.
        // Only core0 ever flushes - the main loop and ssd1306_init() - so
        // there is no reentrancy to protect against.
        static uint8_t packet[1 + OLED_FB_LEN];
        packet[0] = 0x40;  // control byte: data follows
        memcpy(&packet[1], &fb[page * OLED_W], bytes);
        if (!oled_bus_write(packet, 1 + bytes)) {
            return sent_any;
        }

        memcpy(&fb_sent[page * OLED_W], &fb[page * OLED_W], bytes);
        sent_any = true;
        page = end;
    }

    // It only declares itself in sync after a full pass with no errors: if a
    // write fails halfway, the next frame starts over instead of believing it
    // sent something it did not.
    if (sent_any) {
        fb_sent_valid = true;
    }
    return sent_any;
}
