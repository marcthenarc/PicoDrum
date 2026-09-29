// Native test bench for the encoder decoder.
//
// Real rotation can only be tried with fingers, but the decoding cannot: here
// we feed it quadrature sequences built on paper, bounces included, and check
// that the detent count is the expected one.
#include <assert.h>
#include <stdio.h>

#include "encoder.h"

static int failures;

#define CHECK(cond, ...)         \
    do {                         \
        if (!(cond)) {           \
            printf("  FAIL: ");  \
            printf(__VA_ARGS__); \
            printf("\n");        \
            failures++;          \
        }                        \
    } while (0)

// Quadrature sequence of one full clockwise turn, starting from 00.
// State = (A << 1) | B.
static const uint8_t CW_SEQ[4] = {0b00, 0b01, 0b11, 0b10};

static uint32_t now;

static EncoderEvent feed(Encoder *e, uint8_t ab, bool sw) {
    return encoder_update(e, (ab >> 1) & 1, ab & 1, sw, now++);
}

// One detent = four transitions. Counts the events emitted.
static void turn(Encoder *e, int detents, bool cw, int *n_cw, int *n_ccw) {
    int steps = detents * 4;
    static int phase;
    for (int i = 0; i < steps; i++) {
        phase = cw ? (phase + 1) & 3 : (phase + 3) & 3;
        EncoderEvent ev = feed(e, CW_SEQ[phase], false);
        if (ev == ENC_CW) (*n_cw)++;
        if (ev == ENC_CCW) (*n_ccw)++;
    }
}

static void test_rotation(void) {
    Encoder e;
    encoder_init(&e);
    feed(&e, CW_SEQ[0], false);  // first call: state snapshot only

    int cw = 0, ccw = 0;
    turn(&e, 10, true, &cw, &ccw);
    CHECK(cw == 10 && ccw == 0, "10 cw detents -> cw=%d ccw=%d", cw, ccw);

    cw = ccw = 0;
    turn(&e, 7, false, &cw, &ccw);
    CHECK(ccw == 7 && cw == 0, "7 ccw detents -> cw=%d ccw=%d", cw, ccw);

    printf("  rotation                  : 10 cw, 7 ccw, none spurious\n");
}

// The case that breaks naive decoders: the contact bounces between two
// adjacent states without ever completing a detent. No step must come out.
static void test_bounce(void) {
    Encoder e;
    encoder_init(&e);
    feed(&e, 0b00, false);

    int spurious = 0;
    for (int i = 0; i < 50; i++) {
        EncoderEvent a = feed(&e, 0b01, false);
        EncoderEvent b = feed(&e, 0b00, false);
        if (a != ENC_NONE || b != ENC_NONE) {
            spurious++;
        }
    }
    CHECK(spurious == 0, "bounce between two states -> %d phantom detents", spurious);
    printf("  bounce over half a detent : 100 transitions, 0 phantom detents\n");
}

// Powering up with the encoder resting anywhere other than 00 must not produce
// a step: that is the bug that makes the slot jump at every power-up.
static void test_no_startup_step(void) {
    Encoder e;
    encoder_init(&e);
    EncoderEvent first = feed(&e, 0b11, false);
    EncoderEvent second = feed(&e, 0b11, false);
    CHECK(first == ENC_NONE && second == ENC_NONE,
          "start on state 11 -> events %d/%d instead of none", first, second);
    printf("  start on a non-zero state : no detent at power-up\n");
}

static void test_short_press(void) {
    Encoder e;
    encoder_init(&e);
    now = 1000;
    feed(&e, 0b00, false);

    // Pressed and held for less than the long threshold.
    int press = 0, longp = 0;
    for (uint32_t t = 0; t < 200; t++) {
        EncoderEvent ev = feed(&e, 0b00, true);
        if (ev == ENC_PRESS) press++;
        if (ev == ENC_LONG_PRESS) longp++;
    }
    CHECK(press == 0, "the short click must not fire while held down (%d)", press);
    CHECK(longp == 0, "long threshold reached too early (%d)", longp);

    for (uint32_t t = 0; t < 20; t++) {
        EncoderEvent ev = feed(&e, 0b00, false);
        if (ev == ENC_PRESS) press++;
        if (ev == ENC_LONG_PRESS) longp++;
    }
    CHECK(press == 1, "short click: expected 1 event on release, got %d", press);
    CHECK(longp == 0, "a short click must not produce a long press (%d)", longp);
    printf("  short click               : 1 event, emitted on release\n");
}

static void test_long_press(void) {
    Encoder e;
    encoder_init(&e);
    now = 5000;
    feed(&e, 0b00, false);

    int press = 0, longp = 0;
    for (uint32_t t = 0; t < ENC_LONG_PRESS_MS + 300; t++) {
        EncoderEvent ev = feed(&e, 0b00, true);
        if (ev == ENC_PRESS) press++;
        if (ev == ENC_LONG_PRESS) longp++;
    }
    CHECK(longp == 1, "long press: expected 1 event, got %d", longp);

    // Releasing after a long press must not also emit the short click,
    // otherwise leaving assign would confirm the assignment as well.
    for (uint32_t t = 0; t < 20; t++) {
        EncoderEvent ev = feed(&e, 0b00, false);
        if (ev == ENC_PRESS) press++;
    }
    CHECK(press == 0, "after the long press the release emitted %d clicks", press);
    printf("  long press                : 1 event at %ums, no click on release\n",
           ENC_LONG_PRESS_MS);
}

// A pulse shorter than the debounce window must not count.
static void test_debounce(void) {
    Encoder e;
    encoder_init(&e);
    now = 9000;
    feed(&e, 0b00, false);

    int press = 0;
    for (int burst = 0; burst < 30; burst++) {
        for (uint32_t t = 0; t < ENC_DEBOUNCE_MS - 1; t++) {
            if (feed(&e, 0b00, true) == ENC_PRESS) press++;
        }
        for (uint32_t t = 0; t < ENC_DEBOUNCE_MS - 1; t++) {
            if (feed(&e, 0b00, false) == ENC_PRESS) press++;
        }
    }
    CHECK(press == 0, "pulses below the debounce threshold produced %d clicks",
          press);
    printf("  debounce                  : 30 pulses of %dms, 0 clicks\n",
           ENC_DEBOUNCE_MS - 1);
}

int main(void) {
    test_rotation();
    test_bounce();
    test_no_startup_step();
    test_short_press();
    test_long_press();
    test_debounce();

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
