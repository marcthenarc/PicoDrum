// I2C transport stub for the native tests: counts bytes instead of sending
// them.
//
// It is what lets ssd1306.c run on the Mac. The framebuffer and the drawing
// are the same ones that end up on the target, so what the tests inspect is
// exactly what the display would show.
//
// It can also fail on demand. That is not a luxury: the too-short I2C timeout
// bug (5ms for an 11.6ms frame) went unnoticed precisely because the stub
// always said yes, and the error path — where the flush must not declare
// anything sent — was never exercised.
#include "oled_bus.h"

size_t stub_bytes_written;
int    stub_writes;
size_t stub_max_write;

// How many writes still succeed before failures start. Negative = never fail.
int stub_fail_after = -1;

void stub_reset(void) {
    stub_bytes_written = 0;
    stub_writes = 0;
    stub_max_write = 0;
    stub_fail_after = -1;
}

bool oled_bus_init(void) { return true; }

bool oled_bus_write(const uint8_t *buf, size_t len) {
    (void)buf;
    if (stub_fail_after >= 0) {
        if (stub_fail_after == 0) {
            return false;
        }
        stub_fail_after--;
    }
    stub_bytes_written += len;
    stub_writes++;
    if (len > stub_max_write) {
        stub_max_write = len;
    }
    return true;
}
