#include "encoder.h"

// Quadrature transition table, indexed by (previous << 2) | current, where a
// state is (A << 1) | B.
//
// The positive direction is the sequence 00 -> 01 -> 11 -> 10 -> 00. Which way
// that is physically depends on how A and B are wired: if the knob turns the
// wrong way on the hardware, swap PIN_ENC_QUAD_A and PIN_ENC_QUAD_B in pins.h
// and nothing else — no need to touch this table (and that is exactly what was
// done for the encoder on this build).
//
// The impossible combinations, i.e. two-bit jumps caused by a missed sampling,
// are zero: better to lose a quarter than to guess a direction wrong and have
// the cursor jump the other way.
static const int8_t QUAD_TABLE[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0,
};

void encoder_init(Encoder *e) {
    e->ab_prev = 0;
    e->quarters = 0;
    e->sw_stable = false;
    e->sw_raw = false;
    e->sw_raw_ms = 0;
    e->sw_down_ms = 0;
    e->long_sent = false;
    e->started = false;
}

EncoderEvent encoder_update(Encoder *e, bool a, bool b, bool sw_pressed,
                            uint32_t now_ms) {
    uint8_t ab = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));

    // The first call only takes a snapshot of the current state. Without it,
    // starting with the encoder resting anywhere other than 00 would read as a
    // transition and produce a step at boot.
    if (!e->started) {
        e->started = true;
        e->ab_prev = ab;
        e->sw_raw = sw_pressed;
        e->sw_stable = sw_pressed;
        e->sw_raw_ms = now_ms;
        return ENC_NONE;
    }

    if (ab != e->ab_prev) {
        e->quarters = (int8_t)(e->quarters +
                               QUAD_TABLE[(e->ab_prev << 2) | ab]);
        e->ab_prev = ab;

        if (e->quarters >= ENC_QUARTERS_PER_DETENT) {
            e->quarters = 0;
            return ENC_CW;
        }
        if (e->quarters <= -ENC_QUARTERS_PER_DETENT) {
            e->quarters = 0;
            return ENC_CCW;
        }
    }

    // Debounce: a level only counts once it has held for ENC_DEBOUNCE_MS.
    if (sw_pressed != e->sw_raw) {
        e->sw_raw = sw_pressed;
        e->sw_raw_ms = now_ms;
    }

    if (e->sw_raw != e->sw_stable &&
        (uint32_t)(now_ms - e->sw_raw_ms) >= ENC_DEBOUNCE_MS) {
        e->sw_stable = e->sw_raw;
        if (e->sw_stable) {
            e->sw_down_ms = now_ms;
            e->long_sent = false;
        } else if (!e->long_sent) {
            // The short click is emitted on release: before that there is no
            // way to know whether it will turn into a long press.
            return ENC_PRESS;
        }
    }

    if (e->sw_stable && !e->long_sent &&
        (uint32_t)(now_ms - e->sw_down_ms) >= ENC_LONG_PRESS_MS) {
        e->long_sent = true;
        // Emitted while the button is still down: leaving "assign" must happen
        // when the threshold trips, not when the user lets go. That way the
        // feedback on the display arrives while the finger is still pressing.
        return ENC_LONG_PRESS;
    }

    return ENC_NONE;
}
