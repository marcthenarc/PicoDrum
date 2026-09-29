// User interface: strip of the 8 slots, sample name, library, kit and preset
// browsers, and the settings page.
//
//   select : rotation walks the slots, press enters assign,
//            long press opens the menu
//   assign : rotation walks the library, press confirms,
//            long press leaves without assigning
//   menu   : KIT / LOAD PRESET / SAVE PRESET / CONFIG / ABOUT.
//            One encoder has three gestures and select had already spent all
//            three, so everything added since lives behind this one list
//   kit    : rotation walks the kits loading all eight slots as it goes,
//            press confirms, long press puts back what was there before
//   preset : the same, over the eight user presets
//   save   : rotation picks the preset to overwrite, press writes it to flash
//   config : rotation walks the settings, press edits one, long press leaves
//            and saves if anything changed. NOTES > and PAN > are groups: a
//            press opens the eight per-slot entries, a long press comes back
//   about  : firmware version, build and copyright. Nothing to edit: either
//            gesture just leaves
//
// Kit mode only exists on a kit-organised library (blob version 2). On a flat
// one mixer_kit_count() is 0 and the menu entry does nothing, exactly as the
// long press did before kit mode existed.
//
// No SDK dependency: it draws through ssd1306.h and reads state from mixer.h,
// both pure headers. That way the state machine can be exercised on the Mac
// against a fake framebuffer, with no display and no encoder.
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

#include "encoder.h"

typedef enum {
    UI_SELECT = 0,
    UI_ASSIGN,
    UI_KIT,
    UI_MENU,
    UI_PRESET,
    UI_SAVE,
    UI_CONFIG,
    UI_CONFIG_EDIT,
    UI_ABOUT,
} UiMode;

// Menu entries, in the order they are walked.
typedef enum {
    UI_MENU_KIT = 0,
    UI_MENU_LOAD,
    UI_MENU_SAVE,
    UI_MENU_CONFIG,
    UI_MENU_ABOUT,
    UI_MENU_COUNT,
} UiMenuEntry;

// The two things the UI cannot do on its own. `apply` pushes the settings
// record onto the mixer, the MIDI parser and the display; `save` writes it to
// flash, which stops the audio for the length of an erase. Both live in
// main.c: keeping them out of here is what lets ui.c be exercised on the Mac.
typedef struct {
    void (*apply)(void);
    bool (*save)(void);
} UiHooks;

// May be called with NULL, and then confirming a save simply does nothing —
// which is the right behaviour on a build with no flash behind it.
void ui_set_hooks(const UiHooks *hooks);

void ui_init(void);

// Shows the boot splash (logo + product name) until SPLASH_MS elapses or
// the first encoder event, whichever comes first - a turn or a press skips
// it rather than fighting it. `now_ms` is the splash's own start time, the
// same clock ui_tick()/ui_render() already take, so this stays as testable
// as everything else here. Never called by ui_init() itself: tests that
// never call this see the ordinary select screen from the first frame, with
// nothing to skip past.
void ui_show_splash(uint32_t now_ms);

// Applies an encoder event. Does not draw: redrawing happens in ui_tick.
void ui_event(EncoderEvent ev);

// Rebuilds the framebuffer from the current state. Kept separate from ui_tick
// because the tests call it to inspect what was drawn.
void ui_render(uint32_t now_ms);

// Call from the loop: redraws at a fixed rate and only sends to the display
// when the content changed.
void ui_tick(uint32_t now_ms);

UiMode  ui_mode(void);
uint8_t ui_slot(void);
uint32_t ui_browse_index(void);

// Kit being auditioned in kit mode. Meaningless in the other modes.
uint32_t ui_kit_index(void);

// Cursor of each of the added modes, for the tests and for nothing else.
uint32_t ui_menu_index(void);
uint32_t ui_preset_index(void);
// Position in the settings list the cursor is in, and which level that list
// is: 0 the top one, 1 inside NOTES or PAN.
uint32_t ui_config_index(void);
uint32_t ui_config_depth(void);

// MIDI note name in the anglosaxon notation with the octave, e.g. "C#1".
// `out` must be at least 5 bytes long.
void ui_note_name(uint8_t note, char *out);

#endif // UI_H
