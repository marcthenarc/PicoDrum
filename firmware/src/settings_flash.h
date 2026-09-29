// The flash half of the settings store: the only file that knows about XIP,
// sector erases and the fact that the other core is playing audio out of the
// same flash it is about to erase.
//
// settings.c stays SDK-free and testable on the Mac; this is what it talks to
// on the target.
#ifndef SETTINGS_FLASH_H
#define SETTINGS_FLASH_H

#include "settings.h"

// The last two 4KB sectors of the 4MB flash. They sit above the sample
// library, which starts at 0x80000 and today ends around 0x3E1C00 — about
// 120KB of slack between the two.
#define SETTINGS_FLASH_SECTOR_SIZE 4096u
#define SETTINGS_FLASH_OFFSET \
    (PICO_FLASH_SIZE_BYTES - SETTINGS_SECTORS * SETTINGS_FLASH_SECTOR_SIZE)
#define SETTINGS_FLASH_XIP_ADDR (XIP_BASE + SETTINGS_FLASH_OFFSET)

// An erase stops the audio: while flash is being written, no core may fetch or
// read from XIP, and the sample data lives there. The guard hands core1 a RAM
// spin loop to sit in and takes the audio down to silence first — see
// core1_flash_guard_* in main.c.
//
// guard_enter returns false if it could not get core1 out of the way, in which
// case nothing is written: a refused save is recoverable, a sector erased under
// a running core is not. Both may be NULL on a build where core1 is idle.
void settings_flash_init(bool (*guard_enter)(void), void (*guard_leave)(void));

const SettingsBackend *settings_flash_backend(void);

// True if the sample library, as flashed, would run into the settings sectors.
// Cheap to check and the only way this silently corrupts either one.
bool settings_flash_region_clear_of(uint32_t lib_end_offset);

#endif // SETTINGS_FLASH_H
