// I2C transport for the display, the only file of the OLED driver that depends
// on the SDK.
#include "oled_bus.h"

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "pins.h"

#define OLED_I2C i2c0

// Every write has a timeout. Without one, an unplugged display or a bus held
// low by a fault would block core0 forever, and with it the UI and the
// triggers: the module would stop playing because of an accessory.
//
// The timeout MUST be proportional to the length. At 400kHz each byte costs 9
// bits (8 plus the ACK), i.e. 22.5us: a whole frame is 513 bytes and wants
// 11.6ms. With a fixed 5ms timeout the write was truncated halfway, the
// display showed partial frames and the flush, always failing, retransmitted
// on every tick instead of staying quiet. The margin here is about 30%.
#define OLED_BYTE_US   30
#define OLED_FIXED_US  2000

static uint32_t timeout_for(size_t len) {
    return OLED_FIXED_US + (uint32_t)(len + 1) * OLED_BYTE_US;
}

bool oled_bus_init(void) {
    i2c_init(OLED_I2C, OLED_I2C_BAUD);
    gpio_set_function(PIN_OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_OLED_SDA);
    gpio_pull_up(PIN_OLED_SCL);

    // Probe: a "commands follow" control byte with no commands behind it
    // changes nothing on the display, but an ACK says someone answers at the
    // address.
    const uint8_t probe = 0x00;
    int r = i2c_write_timeout_us(OLED_I2C, OLED_I2C_ADDR, &probe, 1, false,
                                 timeout_for(1));
    return r == 1;
}

bool oled_bus_write(const uint8_t *buf, size_t len) {
    int r = i2c_write_timeout_us(OLED_I2C, OLED_I2C_ADDR, buf, len, false,
                                 timeout_for(len));
    return r == (int)len;
}

int oled_bus_scan(uint8_t *found, int max) {
    int n = 0;
    for (uint8_t addr = 0x08; addr < 0x78 && n < max; addr++) {
        uint8_t dummy;
        // A one-byte read is the least invasive way to probe: it writes
        // nothing into any register of any device.
        if (i2c_read_timeout_us(OLED_I2C, addr, &dummy, 1, false,
                                timeout_for(1)) >= 0) {
            found[n++] = addr;
        }
    }
    return n;
}
