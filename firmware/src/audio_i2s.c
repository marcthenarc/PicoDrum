#include "audio_i2s.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "audio_i2s.pio.h"
#include "pins.h"

#define I2S_PIO pio0

// Two buffers taking turns: while the DMA plays one, the ISR regenerates the
// other. The buffers live in RAM, not in flash: the DMA must never contend for
// XIP with the sample reads.
static int32_t buf[2][AUDIO_BUF_FRAMES];

static uint sm;
static uint dma_chan[2];
static uint dma_chan_silence;
static audio_render_fn render_cb;

// The word the keep-alive channel streams while the ISR is out of service.
// Deliberately not `const`: a const would land in .rodata, which is flash, and
// the whole point of this channel is to keep feeding the PIO while a sector is
// being erased and XIP reads nothing. It lives in RAM, like the buffers.
static int32_t silence_word;

static volatile uint32_t stat_buffers;
static volatile uint32_t stat_last_us;
static volatile uint32_t stat_max_us;
static volatile uint32_t stat_late;

// Set while flash is being written. See audio_i2s_set_mute().
static volatile bool muted;

// Time available to regenerate a buffer before the DMA catches up with it.
#define RENDER_BUDGET_US ((AUDIO_BUF_FRAMES * 1000000u) / AUDIO_I2S_RATE)

// Resident in RAM, and deliberately so: when `muted` is set this handler runs
// while a flash sector is being erased, and anything fetched from XIP then
// comes back as garbage. The muted path calls nothing — not even memset, which
// lives in flash — and reads nothing outside this file's RAM.
static void __isr __not_in_flash_func(dma_irq_handler)(void) {
    for (int i = 0; i < 2; i++) {
        if (!dma_channel_get_irq0_status(dma_chan[i])) {
            continue;
        }
        dma_channel_acknowledge_irq0(dma_chan[i]);

        if (muted) {
            for (size_t f = 0; f < AUDIO_BUF_FRAMES; f++) {
                buf[i][f] = 0;
            }
            dma_channel_set_read_addr(dma_chan[i], buf[i], false);
            dma_channel_set_trans_count(dma_chan[i], AUDIO_BUF_FRAMES, false);
            continue;
        }

        // Channel i is done and has already chained to the other one, so
        // buf[i] is free: we regenerate it and rearm the channel for the next
        // round.
        uint32_t t0 = time_us_32();
        render_cb(buf[i], AUDIO_BUF_FRAMES);
        uint32_t dt = time_us_32() - t0;

        stat_last_us = dt;
        if (dt > stat_max_us) {
            stat_max_us = dt;
        }
        if (dt > RENDER_BUDGET_US) {
            stat_late++;
        }
        stat_buffers++;

        // Both the address and the count must be restored: after a complete
        // transfer the transfer_count is left at zero, and a channel rearmed
        // without a count would complete immediately, sending the chain
        // spinning on empty.
        dma_channel_set_read_addr(dma_chan[i], buf[i], false);
        dma_channel_set_trans_count(dma_chan[i], AUDIO_BUF_FRAMES, false);
    }
}

void audio_i2s_init(audio_render_fn render) {
    render_cb = render;
    memset(buf, 0, sizeof(buf));

    uint offset = pio_add_program(I2S_PIO, &audio_i2s_program);
    sm = (uint)pio_claim_unused_sm(I2S_PIO, true);
    audio_i2s_program_init(I2S_PIO, sm, offset, PIN_I2S_DATA, PIN_I2S_CLOCK_BASE);

    // 2 PIO cycles per bit, 32 bits per frame -> 64 cycles per frame.
    float div = (float)clock_get_hz(clk_sys) / (float)(AUDIO_I2S_RATE * 64);
    pio_sm_set_clkdiv(I2S_PIO, sm, div);

    dma_chan[0] = (uint)dma_claim_unused_channel(true);
    dma_chan[1] = (uint)dma_claim_unused_channel(true);
    // Never armed in normal play: see audio_i2s_hold_silence().
    dma_chan_silence = (uint)dma_claim_unused_channel(true);

    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(dma_chan[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(I2S_PIO, sm, true));
        // Ping-pong: each channel, once done with its buffer, starts the other.
        channel_config_set_chain_to(&c, dma_chan[i ^ 1]);

        dma_channel_configure(dma_chan[i], &c, &I2S_PIO->txf[sm], buf[i],
                              AUDIO_BUF_FRAMES, false);
        dma_channel_set_irq0_enabled(dma_chan[i], true);
    }

    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // First buffer filled before starting, so the initial silence is not heard
    // as a click.
    render_cb(buf[0], AUDIO_BUF_FRAMES);
    render_cb(buf[1], AUDIO_BUF_FRAMES);

    dma_channel_start(dma_chan[0]);
    pio_sm_set_enabled(I2S_PIO, sm, true);
}

void audio_i2s_set_mute(bool mute) { muted = mute; }

bool audio_i2s_muted(void) { return muted; }

// Enough frames to outlast any flash operation by orders of magnitude: at
// 44.1kHz this is about 1.7 hours, against the ~50ms of a sector erase. The
// count is 28 bits on the RP2350, so this is also the largest value that fits
// without touching the mode field above it.
#define SILENCE_FRAMES 0x0FFFFFFFu

void audio_i2s_hold_silence(void) {
    // Same order as restart_chain(): break the chain before aborting, or each
    // abort just restarts the pair through its partner.
    for (int i = 0; i < 2; i++) {
        hw_clear_bits(&dma_channel_hw_addr(dma_chan[i])->al1_ctrl,
                      DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    dma_channel_abort(dma_chan[0]);
    dma_channel_abort(dma_chan[1]);

    silence_word = 0;
    dma_channel_config c = dma_channel_get_default_config(dma_chan_silence);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    // One word, read over and over: no buffer to refill, hence no ISR, hence
    // nothing for the lockout to starve.
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(I2S_PIO, sm, true));
    // The default config chains a channel to itself, which means no chain.
    dma_channel_configure(dma_chan_silence, &c, &I2S_PIO->txf[sm],
                          &silence_word, SILENCE_FRAMES, true);
}

void audio_i2s_restart_chain(void) {
    // Whatever is feeding the PIO now, it is not the ping-pong.
    dma_channel_abort(dma_chan_silence);

    // Break the chain before aborting: aborting one channel while its partner
    // still chains back into it just starts the pair up again, and the SDK
    // warns that aborting a chained channel is not safe on its own.
    for (int i = 0; i < 2; i++) {
        hw_clear_bits(&dma_channel_hw_addr(dma_chan[i])->al1_ctrl,
                      DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    dma_channel_abort(dma_chan[0]);
    dma_channel_abort(dma_chan[1]);

    for (int i = 0; i < 2; i++) {
        for (size_t f = 0; f < AUDIO_BUF_FRAMES; f++) {
            buf[i][f] = 0;
        }
        dma_channel_acknowledge_irq0(dma_chan[i]);
        dma_channel_set_read_addr(dma_chan[i], buf[i], false);
        dma_channel_set_trans_count(dma_chan[i], AUDIO_BUF_FRAMES, false);
        hw_set_bits(&dma_channel_hw_addr(dma_chan[i])->al1_ctrl,
                    DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    dma_channel_start(dma_chan[0]);
}

void audio_i2s_debug(bool *muted_out, bool busy[2], uint32_t count[2]) {
    if (muted_out != NULL) {
        *muted_out = muted;
    }
    for (int i = 0; i < 2; i++) {
        if (busy != NULL) {
            busy[i] = dma_channel_is_busy(dma_chan[i]);
        }
        if (count != NULL) {
            count[i] = dma_channel_hw_addr(dma_chan[i])->transfer_count;
        }
    }
}

void audio_i2s_get_stats(audio_i2s_stats_t *out) {
    out->buffers_rendered = stat_buffers;
    out->render_us_last = stat_last_us;
    out->render_us_max = stat_max_us;
    out->render_us_budget = RENDER_BUDGET_US;
    out->late_renders = stat_late;
}

void audio_i2s_probe_pins(audio_i2s_pin_probe_t out[AUDIO_I2S_NUM_PINS], uint32_t ms) {
    static const uint8_t pins[AUDIO_I2S_NUM_PINS] = {
        PIN_I2S_DATA, PIN_I2S_CLOCK_BASE, PIN_I2S_CLOCK_BASE + 1};
    static const char *const names[AUDIO_I2S_NUM_PINS] = {"DIN", "BCK", "LRCK"};

    bool prev[AUDIO_I2S_NUM_PINS];
    for (int i = 0; i < AUDIO_I2S_NUM_PINS; i++) {
        out[i].pin = pins[i];
        out[i].name = names[i];
        out[i].func = (uint8_t)gpio_get_function(pins[i]);
        out[i].func_expected =
            (uint8_t)(I2S_PIO == pio0 ? GPIO_FUNC_PIO0 : GPIO_FUNC_PIO1);
        out[i].edges = 0;
        prev[i] = gpio_get(pins[i]);
    }

    // Brute force sampling: BCK runs at 1.4MHz and the loop cannot keep up,
    // but there is no need to reconstruct the waveform. All it takes is
    // telling "toggling" from "stuck", and counting the changes seen is enough
    // for that.
    absolute_time_t end = make_timeout_time_ms(ms);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        for (int i = 0; i < AUDIO_I2S_NUM_PINS; i++) {
            bool now = gpio_get(pins[i]);
            if (now != prev[i]) {
                out[i].edges++;
                prev[i] = now;
            }
        }
    }

    for (int i = 0; i < AUDIO_I2S_NUM_PINS; i++) {
        out[i].level = prev[i];
    }
}

void audio_i2s_reset_stats(void) {
    stat_max_us = 0;
    stat_late = 0;
    stat_buffers = 0;
}
