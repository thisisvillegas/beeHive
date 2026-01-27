/**
 * Hosyond ESP32-S3 2.8" ILI9341V Display Driver
 * Based on official Hosyond example code
 */

#include "hosyond_lcd.h"
#include <SPI.h>
#include "soc/spi_reg.h"
#include "driver/spi_master.h"
#include "hal/gpio_ll.h"

// Pin definitions - Hosyond ESP32-S3 2.8"
#define LCD_CS   10
#define LCD_DC   46
#define LCD_RST  -1
#define LCD_BL   45
#define SPI_MOSI 11
#define SPI_MISO 13
#define SPI_SCLK 12

// SPI configuration
#define SPI_PORT FSPI
#define LCD_SPI_FREQUENCY 27000000

// SPI register pointers
#define REG_SPI_BASE(i) (((i)>1) ? (DR_REG_SPI3_BASE) : (DR_REG_SPI2_BASE))
static volatile uint32_t* spi_cmd_reg = (volatile uint32_t*)(SPI_CMD_REG(SPI_PORT));
static volatile uint32_t* spi_usr_reg = (volatile uint32_t*)(SPI_USER_REG(SPI_PORT));
static volatile uint32_t* spi_len_reg = (volatile uint32_t*)(SPI_MS_DLEN_REG(SPI_PORT));
static volatile uint32_t* spi_buf_reg = (volatile uint32_t*)(SPI_W0_REG(SPI_PORT));

// Pin macros for GPIO >= 32
#define LCD_CS_LOW   GPIO.out_w1tc = (1 << LCD_CS)
#define LCD_CS_HIGH  GPIO.out_w1ts = (1 << LCD_CS)
#define LCD_DC_LOW   GPIO.out1_w1tc.val = (1 << (LCD_DC - 32))
#define LCD_DC_HIGH  GPIO.out1_w1ts.val = (1 << (LCD_DC - 32))
#define LCD_BL_LOW   GPIO.out1_w1tc.val = (1 << (LCD_BL - 32))
#define LCD_BL_HIGH  GPIO.out1_w1ts.val = (1 << (LCD_BL - 32))

#define SET_SPI_WRITE_MODE *spi_usr_reg = SPI_USR_MOSI
#define SET_SPI_READ_MODE  *spi_usr_reg = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN

// SPI instance
static SPIClass lcd_spi = SPIClass(SPI_PORT);
static bool spi_locked = false;

// Inline SPI write macros
#define SPI_WRITE_BITS(D, L) \
    *spi_len_reg = (L) - 1; \
    *spi_buf_reg = (D); \
    *spi_cmd_reg = SPI_UPDATE; \
    while (*spi_cmd_reg & SPI_UPDATE); \
    *spi_cmd_reg = SPI_USR; \
    while (*spi_cmd_reg & SPI_USR)

static inline void spi_write_8bit(uint8_t data) {
    SPI_WRITE_BITS(data, 8);
}

static inline void spi_write_16bit(uint16_t data) {
    SPI_WRITE_BITS((data << 8) | (data >> 8), 16);
}

static void spi_start_write(void) {
    if (!spi_locked) {
        spi_locked = true;
        lcd_spi.beginTransaction(SPISettings(LCD_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
        LCD_CS_LOW;
        SET_SPI_WRITE_MODE;
    }
}

static void spi_end_write(void) {
    if (spi_locked) {
        spi_locked = false;
        LCD_CS_HIGH;
        SET_SPI_READ_MODE;
        lcd_spi.endTransaction();
    }
}

static void lcd_write_cmd(uint8_t cmd) {
    spi_start_write();
    LCD_DC_LOW;
    spi_write_8bit(cmd);
    spi_end_write();
}

static void lcd_write_data8(uint8_t data) {
    spi_start_write();
    LCD_DC_HIGH;
    spi_write_8bit(data);
    spi_end_write();
}

static void lcd_write_data16(uint16_t data) {
    spi_start_write();
    LCD_DC_HIGH;
    spi_write_16bit(data);
    spi_end_write();
}

void lcd_init(void) {
    // Configure pins
    pinMode(LCD_CS, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    pinMode(LCD_DC, OUTPUT);
    digitalWrite(LCD_DC, HIGH);
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    // Initialize SPI
    lcd_spi.begin(SPI_SCLK, SPI_MISO, SPI_MOSI, -1);

    // ILI9341V initialization sequence (from Hosyond official code)
    lcd_write_cmd(0xCF);
    lcd_write_data8(0x00);
    lcd_write_data8(0xC1);
    lcd_write_data8(0x30);

    lcd_write_cmd(0xED);
    lcd_write_data8(0x64);
    lcd_write_data8(0x03);
    lcd_write_data8(0x12);
    lcd_write_data8(0x81);

    lcd_write_cmd(0xE8);
    lcd_write_data8(0x85);
    lcd_write_data8(0x00);
    lcd_write_data8(0x78);

    lcd_write_cmd(0xCB);
    lcd_write_data8(0x39);
    lcd_write_data8(0x2C);
    lcd_write_data8(0x00);
    lcd_write_data8(0x34);
    lcd_write_data8(0x02);

    lcd_write_cmd(0xF7);
    lcd_write_data8(0x20);

    lcd_write_cmd(0xEA);
    lcd_write_data8(0x00);
    lcd_write_data8(0x00);

    lcd_write_cmd(0xC0);  // Power control
    lcd_write_data8(0x13);

    lcd_write_cmd(0xC1);  // Power control
    lcd_write_data8(0x13);

    lcd_write_cmd(0xC5);  // VCM control
    lcd_write_data8(0x22);
    lcd_write_data8(0x35);

    lcd_write_cmd(0xC7);  // VCM control2
    lcd_write_data8(0xBD);

    lcd_write_cmd(0x21);  // Inversion ON

    lcd_write_cmd(0x36);  // Memory Access Control - Landscape
    lcd_write_data8(0x68);  // MV=1, MX=1, BGR=1

    lcd_write_cmd(0xB6);
    lcd_write_data8(0x0A);
    lcd_write_data8(0xA2);

    lcd_write_cmd(0x3A);  // Pixel format
    lcd_write_data8(0x55);  // 16-bit

    lcd_write_cmd(0xF6);  // Interface Control
    lcd_write_data8(0x01);
    lcd_write_data8(0x30);

    lcd_write_cmd(0xB1);  // Frame rate
    lcd_write_data8(0x00);
    lcd_write_data8(0x1B);

    lcd_write_cmd(0xF2);  // 3Gamma disable
    lcd_write_data8(0x00);

    lcd_write_cmd(0x26);  // Gamma curve
    lcd_write_data8(0x01);

    // Positive gamma
    lcd_write_cmd(0xE0);
    lcd_write_data8(0x0F);
    lcd_write_data8(0x35);
    lcd_write_data8(0x31);
    lcd_write_data8(0x0B);
    lcd_write_data8(0x0E);
    lcd_write_data8(0x06);
    lcd_write_data8(0x49);
    lcd_write_data8(0xA7);
    lcd_write_data8(0x33);
    lcd_write_data8(0x07);
    lcd_write_data8(0x0F);
    lcd_write_data8(0x03);
    lcd_write_data8(0x0C);
    lcd_write_data8(0x0A);
    lcd_write_data8(0x00);

    // Negative gamma
    lcd_write_cmd(0xE1);
    lcd_write_data8(0x00);
    lcd_write_data8(0x0A);
    lcd_write_data8(0x0F);
    lcd_write_data8(0x04);
    lcd_write_data8(0x11);
    lcd_write_data8(0x08);
    lcd_write_data8(0x36);
    lcd_write_data8(0x58);
    lcd_write_data8(0x4D);
    lcd_write_data8(0x07);
    lcd_write_data8(0x10);
    lcd_write_data8(0x0C);
    lcd_write_data8(0x32);
    lcd_write_data8(0x34);
    lcd_write_data8(0x0F);

    lcd_write_cmd(0x11);  // Exit sleep
    delay(120);
    lcd_write_cmd(0x29);  // Display ON
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_write_cmd(0x2A);  // Column address set
    lcd_write_data16(x0);
    lcd_write_data16(x1);

    lcd_write_cmd(0x2B);  // Row address set
    lcd_write_data16(y0);
    lcd_write_data16(y1);

    lcd_write_cmd(0x2C);  // Memory write
}

void lcd_push_colors(uint16_t *data, uint32_t len) {
    spi_start_write();
    LCD_DC_HIGH;

    volatile uint32_t* wr_buf = spi_buf_reg;

    // Process 32 pixels at a time using pipelining
    // Start first transfer, then overlap buffer fill with SPI transmission
    // NOTE: No byte swap here - LVGL pre-swaps with LV_COLOR_16_SWAP=1
    if (len >= 32) {
        // Fill first buffer - just pack two 16-bit colors into 32-bit word
        for (int i = 0; i < 16; i++) {
            uint16_t c0 = data[i * 2];
            uint16_t c1 = data[i * 2 + 1];
            wr_buf[i] = c0 | ((uint32_t)c1 << 16);
        }
        data += 32;
        len -= 32;

        *spi_len_reg = 512 - 1;
        *spi_cmd_reg = SPI_UPDATE;
        while (*spi_cmd_reg & SPI_UPDATE);
        *spi_cmd_reg = SPI_USR;

        // Pipeline: fill next buffer while current transfer runs
        while (len >= 32) {
            // Prepare next data while SPI is busy
            uint32_t temp[16];
            for (int i = 0; i < 16; i++) {
                uint16_t c0 = data[i * 2];
                uint16_t c1 = data[i * 2 + 1];
                temp[i] = c0 | ((uint32_t)c1 << 16);
            }
            data += 32;
            len -= 32;

            // Wait for previous transfer
            while (*spi_cmd_reg & SPI_USR);

            // Copy prepared data to SPI buffer
            for (int i = 0; i < 16; i++) {
                wr_buf[i] = temp[i];
            }

            *spi_cmd_reg = SPI_UPDATE;
            while (*spi_cmd_reg & SPI_UPDATE);
            *spi_cmd_reg = SPI_USR;
        }

        // Wait for last full transfer
        while (*spi_cmd_reg & SPI_USR);
    }

    // Handle remaining pixels
    if (len > 0) {
        uint32_t words = (len + 1) / 2;
        for (uint32_t i = 0; i < words; i++) {
            uint16_t c0 = data[i * 2];
            uint16_t c1 = (i * 2 + 1 < len) ? data[i * 2 + 1] : 0;
            wr_buf[i] = c0 | ((uint32_t)c1 << 16);
        }

        *spi_len_reg = (len * 16) - 1;
        *spi_cmd_reg = SPI_UPDATE;
        while (*spi_cmd_reg & SPI_UPDATE);
        *spi_cmd_reg = SPI_USR;
        while (*spi_cmd_reg & SPI_USR);
    }

    spi_end_write();
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;

    lcd_set_window(x, y, x + w - 1, y + h - 1);

    uint32_t total = (uint32_t)w * h;
    uint32_t color32 = ((color << 8) | (color >> 8));
    color32 = (color32 << 16) | color32;

    spi_start_write();
    LCD_DC_HIGH;

    volatile uint32_t* wr_buf = spi_buf_reg;

    // Fill buffer with color
    for (int i = 0; i < 16; i++) {
        wr_buf[i] = color32;
    }

    // Write in chunks of 32 pixels
    while (total >= 32) {
        while (*spi_cmd_reg & SPI_USR);
        *spi_len_reg = 512 - 1;  // 32 pixels
        *spi_cmd_reg = SPI_UPDATE;
        while (*spi_cmd_reg & SPI_UPDATE);
        *spi_cmd_reg = SPI_USR;
        total -= 32;
    }

    // Remaining pixels
    if (total > 0) {
        while (*spi_cmd_reg & SPI_USR);
        *spi_len_reg = (total * 16) - 1;
        *spi_cmd_reg = SPI_UPDATE;
        while (*spi_cmd_reg & SPI_UPDATE);
        *spi_cmd_reg = SPI_USR;
        while (*spi_cmd_reg & SPI_USR);
    }

    spi_end_write();
}

void lcd_set_backlight(uint8_t level) {
    analogWrite(LCD_BL, level);
}
