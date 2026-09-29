// I2S driver: PIO + double-buffered DMA into the PCM5102A.
//
// Rendering happens inside the DMA ISR, on whichever core called
// audio_i2s_init(). It must therefore be initialised from core1, so that core0
// stays free for MIDI, encoder and OLED without ever holding up the audio.
#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// I2S frame rate. Samples are stored at 22050Hz and duplicated 2x by the
// mixer: no interpolation, consistent with the "no pitch shifting" non-goal.
#define AUDIO_I2S_RATE 44100

// Frames per buffer. At 44.1kHz that is 256/44100 = 5.8ms of headroom for the
// render. Larger = more headroom but more trigger latency.
#define AUDIO_BUF_FRAMES 256

// Fills `frames` with `n` stereo frames, formatted as (L << 16) | (R & 0xFFFF).
typedef void (*audio_render_fn)(int32_t *frames, size_t n);

typedef struct {
    uint32_t buffers_rendered;
    uint32_t render_us_last;
    uint32_t render_us_max;   // the peak is what matters: if it goes past the
    uint32_t render_us_budget; // buffer period, the audio breaks up
    uint32_t late_renders;    // renders that overran the budget
} audio_i2s_stats_t;

// Starts PIO, DMA and IRQ. Call once, from core1.
void audio_i2s_init(audio_render_fn render);

// Renders digital silence instead of calling the render callback, and does it
// from RAM. That is what makes a flash write survivable: while a sector is
// being erased no core may fetch or read from XIP, and both the render
// callback and the sample data it reads live there. Muted, the ISR touches
// nothing outside RAM and registers, so the DMA chain keeps running and the
// output is silence rather than 5.8ms of sound looping.
//
// Safe to call from either core: it is one volatile flag.
void audio_i2s_set_mute(bool mute);
bool audio_i2s_muted(void);

// Why the audio stopped, when nothing else says. `buffers_rendered` only counts
// in the ISR's unmuted path, so a frozen count cannot tell "still muted" from
// "the DMA chain died": the first keeps the chain running on zeros, the second
// stops it dead. This reports both halves - the mute flag, and whether each
// ping-pong channel is still busy and what transfer count it holds.
void audio_i2s_debug(bool *muted_out, bool busy[2], uint32_t count[2]);

// Hands the PIO over to a CPU-free source of digital silence, for a window in
// which the DMA ISR will not run - which is what a flash write is, since the
// other core is locked out of the bus for the whole erase.
//
// Muting alone is not enough, and this is what the save's burst of noise turned
// out to be. The ping-pong needs the ISR to re-arm each channel (a finished one
// is left with a transfer count of zero), so without it the pair spins on empty
// in microseconds, the PIO's 8-word FIFO drains, and the state machine stalls
// on its autopull with BCK, LRCK and DIN frozen. The PCM5102A is wired with
// SCK to ground, so it runs off its own PLL locked to BCK: take BCK away for
// ~50ms and the PLL unlocks, and what is heard on the way back is it
// re-locking. XSMT is tied high on the board, so there is no soft-mute to hide
// behind either - the only answer is to never stop the clocks.
//
// A single channel reading one RAM word with no increment does that: no
// buffer, no interrupt, nothing on the flash bus. audio_i2s_restart_chain()
// takes it back down.
void audio_i2s_hold_silence(void);

// Rebuilds the ping-pong from a stop. The two channels chain into each other,
// so once the pair goes idle - which is what a long flash erase does to it,
// because the ISR that re-arms them cannot run while the other core is locked
// out - nothing restarts it: each would need the other to fire first. Call it
// after any window in which the ISR was not serviced.
void audio_i2s_restart_chain(void);

void audio_i2s_get_stats(audio_i2s_stats_t *out);
void audio_i2s_reset_stats(void);

// --- pin diagnostics --------------------------------------------------------
// The RP2350 can read back the level of its own GPIOs even while the PIO is
// driving them: enough to establish, with no instruments, whether the pins are
// really moving.

#define AUDIO_I2S_NUM_PINS 3

typedef struct {
    uint8_t  pin;
    uint8_t  func;           // current GPIO function
    uint8_t  func_expected;  // the one needed for the PIO to drive the pin
    bool     level;          // level at the last sampling
    uint32_t edges;          // transitions seen in the window
    const char *name;
} audio_i2s_pin_probe_t;

// Samples the three I2S pins for `ms` milliseconds and counts the transitions.
void audio_i2s_probe_pins(audio_i2s_pin_probe_t out[AUDIO_I2S_NUM_PINS], uint32_t ms);

#endif // AUDIO_I2S_H
