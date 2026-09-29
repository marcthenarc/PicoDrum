// Display transport, kept apart from the rest of the driver.
//
// Its job is to keep ssd1306.c free of the SDK: the framebuffer and the drawing
// are pure logic and compile natively on the Mac, while the real I2C lives in
// oled_i2c.c. In the tests the same symbols are implemented by a stub that
// records the bytes instead of sending them.
#ifndef OLED_BUS_H
#define OLED_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Brings up the bus. false if the display does not answer at the expected
// address.
bool oled_bus_init(void);

// Writes an already framed block: buf[0] is the SSD1306 control byte (0x00 for
// commands, 0x40 for data). false on timeout or NACK.
bool oled_bus_write(const uint8_t *buf, size_t len);

// Target only: lists the addresses that answer on the bus. Useful when the
// display stays dark and it is not clear whether the wiring or the address is
// at fault (some modules sit at 0x3D instead of 0x3C). Returns how many were
// found.
int oled_bus_scan(uint8_t *found, int max);

#endif // OLED_BUS_H
