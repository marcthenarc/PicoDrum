#include "settings_flash.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

// flash_safe_execute() hands the callback a void*; this is what it carries.
typedef struct {
    uint32_t       offset;
    const uint8_t *page;
} EraseProgram;

// How long flash_safe_execute() may wait for the other core to reach the
// lockout, and to be let go again. Generous: refusing the save is recoverable,
// and the erase itself is the slow part anyway.
#define FLASH_SAFE_TIMEOUT_MS 1000

static bool (*guard_enter)(void);
static void (*guard_leave)(void);

void settings_flash_init(bool (*enter)(void), void (*leave)(void)) {
    guard_enter = enter;
    guard_leave = leave;
}

bool settings_flash_region_clear_of(uint32_t lib_end_offset) {
    return lib_end_offset <= SETTINGS_FLASH_OFFSET;
}

static uint32_t sector_offset(uint32_t sector) {
    return SETTINGS_FLASH_OFFSET + sector * SETTINGS_FLASH_SECTOR_SIZE;
}

static const void *flash_read(uint32_t sector) {
    if (sector >= SETTINGS_SECTORS) {
        return NULL;
    }
    return (const void *)(XIP_BASE + sector_offset(sector));
}

// Programming happens a page at a time: the record is padded up to a whole
// number of pages rather than writing a partial one, whose leftover bytes would
// be undefined. 320 bytes of record become two 256-byte pages. At file scope
// because the flash_safe_execute() callback needs it as well as flash_write().
#define SETTINGS_PROG_LEN                                        \
    (((sizeof(SettingsRecord) + FLASH_PAGE_SIZE - 1u) /          \
      FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)

static void erase_program(void *param) {
    const EraseProgram *ep = (const EraseProgram *)param;
    flash_range_erase(ep->offset, SETTINGS_FLASH_SECTOR_SIZE);
    flash_range_program(ep->offset, ep->page, SETTINGS_PROG_LEN);
}

static bool flash_write(uint32_t sector, const void *buf, uint32_t len) {
    if (sector >= SETTINGS_SECTORS || len > SETTINGS_FLASH_SECTOR_SIZE) {
        return false;
    }

    _Static_assert(SETTINGS_PROG_LEN <= SETTINGS_FLASH_SECTOR_SIZE,
                   "the record no longer fits one flash sector");
    static uint8_t page[SETTINGS_PROG_LEN];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, buf, len);

    if (guard_enter != NULL && !guard_enter()) {
        return false;
    }
    // Through flash_safe_execute(), not raw: it is what arranges the multicore
    // lockout the RP2350 bootrom's flash routines need. Calling flash_range_*
    // directly with the other core merely spinning in RAM wrote the record and
    // then hung core0 inside the erase - reliably, on the second (full-cost)
    // save of a session. See the flash guard in main.c.
    EraseProgram ep = {.offset = sector_offset(sector), .page = page};
    int rc = flash_safe_execute(erase_program, &ep, FLASH_SAFE_TIMEOUT_MS);
    if (guard_leave != NULL) {
        guard_leave();
    }
    if (rc != PICO_OK) {
        return false;
    }

    // Read back through XIP: the point is not to trust the write, it is to
    // catch a sector that has stopped taking programs.
    return memcmp(flash_read(sector), buf, len) == 0;
}

static const SettingsBackend BACKEND = {flash_read, flash_write};

const SettingsBackend *settings_flash_backend(void) { return &BACKEND; }
