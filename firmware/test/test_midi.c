// Native test bench for the MIDI parser.
//
// Every case in here is a stream a real controller produces and a naive parser
// gets wrong: running status, a clock arriving between the two data bytes of a
// note, velocity 0, a message cut in half by a new status byte.
#include <stdio.h>
#include <string.h>

#include "midi.h"

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

#define DRUMS MIDI_CHANNEL_GM_DRUMS  // 9, i.e. channel 10 on a front panel

// Feeds a byte sequence and collects the events. Returns how many came out.
static int feed_all(Midi *m, const uint8_t *bytes, size_t n, MidiEvent *out,
                    int max) {
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        MidiEvent ev;
        if (midi_feed(m, bytes[i], &ev) && count < max) {
            out[count++] = ev;
        }
    }
    return count;
}

#define FEED(m, arr, out) feed_all((m), (arr), sizeof(arr), (out), 16)

static void test_basic_note(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // Note On 36 velocity 100 on channel 10, then its Note Off.
    const uint8_t s[] = {0x99, 36, 100, 0x89, 36, 64};
    int n = FEED(&m, s, ev);

    CHECK(n == 2, "expected 2 events, got %d", n);
    CHECK(n > 0 && ev[0].type == MIDI_NOTE_ON && ev[0].note == 36 &&
              ev[0].velocity == 100 && ev[0].channel == DRUMS,
          "the Note On did not come out intact");
    CHECK(n > 1 && ev[1].type == MIDI_NOTE_OFF, "the Note Off did not come out");
    printf("  note on/off               : 2 events, note 36 velocity 100\n");
}

static void test_running_status(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // One status byte, then four notes as bare data pairs. This is what nearly
    // every sequencer sends, and dropping it costs three notes out of four.
    const uint8_t s[] = {0x99, 36, 100, 38, 90, 42, 80, 46, 70};
    int n = FEED(&m, s, ev);

    CHECK(n == 4, "running status: expected 4 notes, got %d", n);
    const uint8_t want[4] = {36, 38, 42, 46};
    for (int i = 0; i < n && i < 4; i++) {
        CHECK(ev[i].note == want[i] && ev[i].type == MIDI_NOTE_ON,
              "running status: note %d is %u, expected %u", i, ev[i].note,
              want[i]);
    }
    printf("  running status            : 1 status byte, 4 notes out\n");
}

static void test_realtime_interleaved(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // 0xF8 clock lands between status and data, between the two data bytes,
    // and in the middle of running status. Under a running clock this is the
    // normal case, not the exception.
    const uint8_t s[] = {0x99, 0xF8, 36, 0xF8, 100, 0xF8, 38, 0xFE, 90};
    int n = FEED(&m, s, ev);

    CHECK(n == 2, "interleaved clock: expected 2 notes, got %d", n);
    CHECK(n > 0 && ev[0].note == 36 && ev[0].velocity == 100,
          "the clock corrupted the first note");
    CHECK(n > 1 && ev[1].note == 38 && ev[1].velocity == 90,
          "the clock broke running status");
    CHECK(m.realtime == 4, "expected 4 real-time bytes counted, got %u",
          m.realtime);
    printf("  clock between data bytes  : 4 real-time bytes, 2 notes intact\n");
}

static void test_velocity_zero(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    const uint8_t s[] = {0x99, 36, 0};
    int n = FEED(&m, s, ev);

    CHECK(n == 1, "velocity 0: expected 1 event, got %d", n);
    CHECK(n > 0 && ev[0].type == MIDI_NOTE_OFF,
          "Note On at velocity 0 must come out as a Note Off, not a silent hit");
    printf("  note on velocity 0        : reported as a Note Off\n");
}

static void test_channel_filter(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // Channel 1 (0x90) and channel 10 (0x99) in the same stream.
    const uint8_t s[] = {0x90, 60, 100, 0x99, 36, 100, 0x91, 62, 100};
    int n = FEED(&m, s, ev);

    CHECK(n == 1, "channel filter: expected 1 note, got %d", n);
    CHECK(n > 0 && ev[0].note == 36, "the wrong channel got through");
    CHECK(m.filtered == 2, "expected 2 notes filtered, got %u", m.filtered);

    // The same stream with the filter open must give all three.
    Midi any;
    midi_init(&any, MIDI_CHANNEL_ANY);
    n = FEED(&any, s, ev);
    CHECK(n == 3, "filter open: expected 3 notes, got %d", n);
    printf("  channel filter            : 1 of 3 on channel 10, 3 of 3 open\n");
}

static void test_truncated_message(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // A note cut off after its first data byte, then a new status byte. The
    // half-message must be dropped, and the one that follows must survive.
    const uint8_t s[] = {0x99, 36, 0x99, 38, 90};
    int n = FEED(&m, s, ev);

    CHECK(n == 1, "truncated: expected 1 note, got %d", n);
    CHECK(n > 0 && ev[0].note == 38 && ev[0].velocity == 90,
          "the note after the truncated one did not survive");
    printf("  truncated message         : half-message dropped, next intact\n");
}

static void test_sysex_and_common(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // A SysEx dump whose payload contains 36 and 100: read as notes it would
    // fire the kick. Then a Song Position, which cancels running status.
    const uint8_t s[] = {0x99, 36, 100, 0xF0, 0x7E, 36, 100, 0xF7,
                         38, 90,                       // orphans, status gone
                         0x99, 42, 80};
    int n = FEED(&m, s, ev);

    CHECK(n == 2, "sysex: expected 2 notes, got %d", n);
    CHECK(n > 1 && ev[1].note == 42, "the note after the dump was lost");
    CHECK(m.orphans == 2, "expected 2 orphan data bytes, got %u", m.orphans);
    printf("  sysex dump                : payload not played, 2 orphans\n");
}

static void test_stream_joined_midway(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // Powering the module up with the sequencer already running: the first
    // bytes are data with no status. They must be dropped, quietly.
    const uint8_t s[] = {100, 38, 90, 0x99, 36, 100};
    int n = FEED(&m, s, ev);

    CHECK(n == 1, "joined midway: expected 1 note, got %d", n);
    CHECK(m.orphans == 3, "expected 3 orphans, got %u", m.orphans);
    printf("  stream joined midway      : 3 orphans, first real note plays\n");
}

static void test_other_channel_messages(void) {
    Midi m;
    midi_init(&m, DRUMS);
    MidiEvent ev[16];

    // Control Change (2 data bytes) and Program Change (1). Getting the
    // lengths wrong here shifts every following byte by one and turns the
    // stream into noise.
    const uint8_t s[] = {0xB9, 7, 127, 0xC9, 5, 0x99, 36, 100};
    int n = FEED(&m, s, ev);

    CHECK(n == 1, "CC/PC: expected 1 note, got %d", n);
    CHECK(n > 0 && ev[0].note == 36 && ev[0].velocity == 100,
          "the note after CC/PC came out shifted");
    printf("  control/program change    : lengths right, note follows clean\n");
}

int main(void) {
    test_basic_note();
    test_running_status();
    test_realtime_interleaved();
    test_velocity_zero();
    test_channel_filter();
    test_truncated_message();
    test_sysex_and_common();
    test_stream_joined_midway();
    test_other_channel_messages();

        // A gap in the stream: bytes were missed while flash was erasing, and what
    // comes back is a fragment. Without a resync the parser reads it against
    // the running status it still holds from before the gap and plays notes
    // nobody sent; with one it waits for a fresh status byte and counts the
    // fragment as orphans, exactly as it does for a stream joined at power-on.
    {
        Midi m;
        MidiEvent ev;
        midi_init(&m, MIDI_CHANNEL_GM_DRUMS);
        midi_feed(&m, 0x99, &ev);            // note on, channel 10
        midi_feed(&m, 36, &ev);
        CHECK(midi_feed(&m, 100, &ev), "the first note should have come out");
        uint32_t events_before = m.events;

        // The stale half of a message left in the FIFO across the gap.
        midi_resync(&m);
        CHECK(m.status == 0, "resync must drop the running status");
        CHECK(m.n_data == 0, "resync must drop the half-parsed data");
        CHECK(m.filter == MIDI_CHANNEL_GM_DRUMS, "resync must keep the filter");
        CHECK(m.events == events_before, "resync must not touch the counters");

        // Those two bytes would have been a note under the old running status.
        uint32_t orphans_before = m.orphans;
        midi_feed(&m, 38, &ev);
        midi_feed(&m, 120, &ev);
        CHECK(m.events == events_before,
              "a fragment after a resync played %u notes, expected none",
              m.events - events_before);
        CHECK(m.orphans == orphans_before + 2,
              "the fragment should have counted 2 orphans, counted %u",
              m.orphans - orphans_before);
        printf("  resync after a flash gap  : status dropped, fragment orphaned\n");
    }

printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
