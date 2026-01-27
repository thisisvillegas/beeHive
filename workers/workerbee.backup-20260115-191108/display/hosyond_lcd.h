#ifndef HOSYOND_LCD_H
#define HOSYOND_LCD_H

#include <Arduino.h>
#include <stdint.h>

// Display dimensions (landscape)
#define LCD_WIDTH  320
#define LCD_HEIGHT 240

// Initialize the display hardware
void lcd_init(void);

// Set the drawing window
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

// Push a block of pixels (for LVGL flush)
void lcd_push_colors(uint16_t *data, uint32_t len);

// Fill a rectangle with a single color
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

// Set backlight level (0-255)
void lcd_set_backlight(uint8_t level);

#endif // HOSYOND_LCD_H
