// EC11 rotary encoder with pushbutton (KY-040 breakout).
//
// No SDK dependency on purpose: the function takes already sampled pin levels
// and a millisecond timestamp, so the decoding can be tested on the Mac by
// feeding it state sequences, with no need to turn a real knob by hand to try
// out a threshold change.
#ifndef ENCODER_H
#define ENCODER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ENC_NONE = 0,
    ENC_CW,          // one detent clockwise
    ENC_CCW,         // one detent counter-clockwise
    ENC_PRESS,       // short press, emitted on release
    ENC_LONG_PRESS,  // threshold reached, emitted once while still held down
} EncoderEvent;

// One EC11 detent is four quadrature transitions. We accumulate quarters and
// emit a step every four: that way contact bounce, which swings back and
// forth, sums to zero instead of producing phantom steps.
#define ENC_QUARTERS_PER_DETENT 4

#define ENC_DEBOUNCE_MS   5
#define ENC_LONG_PRESS_MS 600

typedef struct {
    uint8_t  ab_prev;
    int8_t   quarters;
    bool     sw_stable;    // level accepted after debouncing
    bool     sw_raw;       // last raw level seen
    uint32_t sw_raw_ms;    // when the raw level changed
    uint32_t sw_down_ms;   // when the press was accepted
    bool     long_sent;
    bool     started;
} Encoder;

void encoder_init(Encoder *e);

// Call once per loop iteration. `a` and `b` are the levels of the two
// quadrature pins, `sw_pressed` is the button already normalised to positive
// logic (the physical pin is active low: the caller does the normalising, so
// there is no hidden electrical convention in here).
//
// Returns at most one event per call. If rotation and button change at the
// same instant, rotation wins and the button is evaluated on the next
// iteration: at a 1ms period the delay is invisible and no event is lost.
EncoderEvent encoder_update(Encoder *e, bool a, bool b, bool sw_pressed,
                            uint32_t now_ms);

#endif // ENCODER_H
