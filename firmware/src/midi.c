#include "midi.h"

#include <string.h>

#define IS_STATUS(b)   ((b) & 0x80u)
#define IS_REALTIME(b) ((b) >= 0xF8u)

#define STATUS_NOTE_OFF 0x80u
#define STATUS_NOTE_ON  0x90u
#define SYSEX_START     0xF0u
#define SYSEX_END       0xF7u

// Data bytes expected after a channel status byte. Program Change and Channel
// Pressure carry one, everything else carries two.
static uint8_t data_len(uint8_t status) {
    uint8_t kind = status & 0xF0u;
    return (kind == 0xC0u || kind == 0xD0u) ? 1u : 2u;
}

void midi_init(Midi *m, uint8_t filter) {
    memset(m, 0, sizeof(*m));
    m->filter = filter;
}

void midi_resync(Midi *m) {
    m->status = 0;
    m->n_data = 0;
    m->in_sysex = false;
}

void midi_set_filter(Midi *m, uint8_t filter) {
    m->filter = filter;
}

static bool accepted(const Midi *m, uint8_t channel) {
    return m->filter == MIDI_CHANNEL_ANY || m->filter == channel;
}

// Builds the event from a complete Note On/Off. Returns false when the channel
// filter rejects it, so the caller can count it.
static bool emit_note(Midi *m, MidiEvent *out) {
    uint8_t channel = m->status & 0x0Fu;
    uint8_t note = m->data[0] & 0x7Fu;
    uint8_t velocity = m->data[1] & 0x7Fu;

    if (!accepted(m, channel)) {
        m->filtered++;
        return false;
    }

    out->channel = channel;
    out->note = note;
    out->velocity = velocity;
    // A Note On at velocity 0 is a Note Off. For one-shots both are ignored
    // upstream, but it must not come out as a hit at zero velocity.
    out->type = (m->status & 0xF0u) == STATUS_NOTE_ON && velocity > 0
                    ? MIDI_NOTE_ON
                    : MIDI_NOTE_OFF;
    m->events++;
    return true;
}

bool midi_feed(Midi *m, uint8_t byte, MidiEvent *out) {
    m->bytes++;

    // Real-time messages are single bytes that may land anywhere, including
    // between the two data bytes of a note. Consuming them without touching
    // the parse in progress is the whole point: a parser that resets here
    // loses notes under a running clock, which is exactly when it is playing.
    if (IS_REALTIME(byte)) {
        m->realtime++;
        return false;
    }

    if (m->in_sysex) {
        // Inside a SysEx dump every byte is payload until the terminator.
        if (byte == SYSEX_END) {
            m->in_sysex = false;
        }
        return false;
    }

    if (IS_STATUS(byte)) {
        if (byte == SYSEX_START) {
            m->in_sysex = true;
            m->status = 0;  // System Common cancels running status
            m->n_data = 0;
            return false;
        }
        if (byte >= SYSEX_START) {
            m->status = 0;  // the other System Common messages cancel it too
            m->n_data = 0;
            return false;
        }
        m->status = byte;
        m->n_data = 0;
        return false;
    }

    // A data byte with no status to attach it to: the stream was joined
    // mid-message, or a status byte was lost.
    if (m->status == 0) {
        m->orphans++;
        return false;
    }

    m->data[m->n_data++] = byte;
    if (m->n_data < data_len(m->status)) {
        return false;
    }

    // Message complete. The status is deliberately kept: the next message may
    // arrive as data bytes alone, which is running status and is what nearly
    // every controller does.
    m->n_data = 0;

    uint8_t kind = m->status & 0xF0u;
    if (kind == STATUS_NOTE_ON || kind == STATUS_NOTE_OFF) {
        return emit_note(m, out);
    }
    return false;
}
