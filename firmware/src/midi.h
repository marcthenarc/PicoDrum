// MIDI input parser, one byte at a time.
//
// No SDK dependency on purpose, same as encoder.c: the parser takes raw bytes
// and returns events, so it can be fed byte sequences from a test on the Mac —
// running status, interleaved clock, truncated messages — instead of being
// debugged against a live cable and a sequencer.
//
// It handles the four things that break naive parsers:
// running status, real-time bytes arriving in the middle of a message, Note On
// with velocity 0 as a Note Off, and channel filtering.
#ifndef MIDI_H
#define MIDI_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MIDI_NONE = 0,
    MIDI_NOTE_ON,   // velocity > 0
    MIDI_NOTE_OFF,  // a real Note Off, or a Note On with velocity 0
} MidiEventType;

typedef struct {
    MidiEventType type;
    uint8_t channel;   // 0-15, so channel 10 on the front panel is 9 in here
    uint8_t note;
    uint8_t velocity;
} MidiEvent;

// GM percussion lives on channel 10, which is index 9. Filtering is a runtime
// setting rather than a compile-time one because the config page planned for a
// future version has to be able to change it.
#define MIDI_CHANNEL_GM_DRUMS 9
#define MIDI_CHANNEL_ANY      0xFF

typedef struct {
    uint8_t filter;      // channel to accept, or MIDI_CHANNEL_ANY
    uint8_t status;      // running status, 0 = none held
    uint8_t data[2];
    uint8_t n_data;
    bool    in_sysex;

    // Counters, purely for the console: they turn "notes sometimes get lost"
    // into a number that says where they are lost.
    uint32_t bytes;
    uint32_t events;      // note on/off passing the channel filter
    uint32_t filtered;    // well-formed notes dropped by the channel filter
    uint32_t realtime;    // 0xF8-0xFF consumed without disturbing the parse
    uint32_t orphans;     // data bytes with no status to attach them to
} Midi;

// `filter` is a channel index 0-15, or MIDI_CHANNEL_ANY.
void midi_init(Midi *m, uint8_t filter);

void midi_set_filter(Midi *m, uint8_t filter);

// Drops the parse in progress without touching the filter or the counters.
// For use after a gap in which bytes were missed - a flash erase holds core0
// for ~147ms with its interrupts off - where whatever arrives next is a
// fragment and the running status held from before the gap no longer describes
// it. midi_init() would do this too, but it would also reset the counters,
// which are the only record of what the stream did.
void midi_resync(Midi *m);

// Feeds one received byte. Returns true and fills `out` when the byte
// completes a note event that passes the channel filter; returns false
// otherwise, which is the common case.
bool midi_feed(Midi *m, uint8_t byte, MidiEvent *out);

#endif // MIDI_H
