/**
 * Private configuration file for the SSD1306 library.
 * Configured for this project: STM32F446 (STM32F4), I2C1, addr 0x3C, 128x64.
 */

#ifndef __SSD1306_CONF_H__
#define __SSD1306_CONF_H__

// Microcontroller family
#define STM32F4

// Bus
#define SSD1306_USE_I2C

// I2C Configuration
#define SSD1306_I2C_PORT        hi2c1
#define SSD1306_I2C_ADDR        (0x3C << 1)

// Fonts: only the two we use, to save flash.
#define SSD1306_INCLUDE_FONT_6x8
#define SSD1306_INCLUDE_FONT_7x10

// Display geometry (defaults, listed for clarity)
#define SSD1306_WIDTH           128
#define SSD1306_HEIGHT          64

#endif /* __SSD1306_CONF_H__ */
