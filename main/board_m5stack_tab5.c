// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board implementation for M5Stack Tab5 (ESP32-P4)
// Post-Oct-2025 hardware revision: ST7123 combined display+touch IC
// 720x1280 MIPI-DSI 2-lane, RGB565
//
// Init sequence derived from M5Tab5-UserDemo (MIT, M5Stack Technology CO LTD)
// ST7123 LCD driver: Apache-2.0, Espressif Systems

#include "board_interface.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_codec_dev_defaults.h"
#include "es8388_codec.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BOARD_TAB5";

#define BOARD_NAME  "M5Stack Tab5"

// I2C (system bus — PI4IOE expanders and touch)
#define I2C_BUS_NUM     0
#define I2C_SDA_GPIO    31
#define I2C_SCL_GPIO    32
#define I2C_TIMEOUT_MS  50

// PI4IOE5V6408 IO expander I2C addresses
#define PI4IOE1_ADDR    0x43  // addr pin low
#define PI4IOE2_ADDR    0x44  // addr pin high

// PI4IOE register map
#define PI4IO_REG_CHIP_RESET  0x01
#define PI4IO_REG_IO_DIR      0x03
#define PI4IO_REG_OUT_SET     0x05
#define PI4IO_REG_OUT_H_IM    0x07
#define PI4IO_REG_IN_DEF_STA  0x09
#define PI4IO_REG_PULL_EN     0x0B
#define PI4IO_REG_PULL_SEL    0x0D
#define PI4IO_REG_INT_MASK    0x11

// I2S (ES8388 audio codec)
#define I2S_PORT            I2S_NUM_0
#define I2S_MCLK_GPIO       30
#define I2S_BCLK_GPIO       27
#define I2S_LRCK_GPIO       29
#define I2S_DOUT_GPIO       26
#define AUDIO_DMA_DESC_NUM  8
#define AUDIO_DMA_FRAME_NUM 960   // 480ms @ 16 kHz

// LDO channel for MIPI DSI PHY power
#define DSI_PHY_LDO_CHAN        3
#define DSI_PHY_LDO_VOLTAGE_MV  2500

// Backlight: LEDC PWM on GPIO 22
#define LCD_BACKLIGHT_GPIO  22
#define LCD_LEDC_TIMER      LEDC_TIMER_0
#define LCD_LEDC_CHAN       LEDC_CHANNEL_1
#define LCD_LEDC_FREQ_HZ    5000
#define LCD_LEDC_DUTY_RES   LEDC_TIMER_12_BIT
#define LCD_LEDC_DUTY_MAX   4095

// Display geometry and DSI parameters
#define LCD_W               720
#define LCD_H               1280
#define BPP                 2   // RGB565
#define FB_SIZE             (LCD_W * LCD_H * BPP)
#define DSI_LANE_BITRATE    965  // Mbps
#define DPI_CLOCK_MHZ       70

static esp_lcd_panel_handle_t  s_panel   = NULL;
static uint8_t                *s_fb      = NULL;  // hardware framebuffer (DPI)
static uint8_t                *s_backbuf = NULL;  // render buffer (PSRAM)

static i2c_master_bus_handle_t s_i2c_bus  = NULL;  // shared I2C bus 0
static i2c_master_dev_handle_t s_pi4ioe1  = NULL;  // PI4IOE1 (SPK_EN control)
static esp_codec_dev_handle_t  s_spk_dev  = NULL;

// --- ST7123 vendor init sequence (M5Stack Tab5, post-Oct-2025 hardware) ---
static const st7123_lcd_init_cmd_t s_st7123_init[] = {
    {0x60, (uint8_t[]){0x71, 0x23, 0xa2}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa3}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xD7, (uint8_t[]){0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80}, 6, 0},
    {0x90, (uint8_t[]){0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09}, 7, 0},
    {0xA3, (uint8_t[]){0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00,
                       0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
                       0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF},
     40, 0},
    {0xA6, (uint8_t[]){0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24,
                       0x55, 0x38, 0x00, 0x37, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00,
                       0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00, 0x03, 0x6E,
                       0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00, 0x00, 0x00},
     55, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44, 0x03, 0x6E, 0x6E, 0x91, 0xFF,
                       0x08, 0x80, 0x64, 0x40, 0x25, 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08,
                       0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80,
                       0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44},
     60, 0},
    {0xAC, (uint8_t[]){0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11, 0x08, 0x08, 0x0A, 0x0A, 0x1C,
                       0x1C, 0x07, 0x07, 0x00, 0x00, 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12,
                       0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07, 0x03, 0x03, 0x01, 0x01},
     44, 0},
    {0xAD, (uint8_t[]){0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0, 0x40, 0x06, 0x01,
                       0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
     25, 0},
    {0xAE, (uint8_t[]){0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00}, 7, 0},
    {0xB2, (uint8_t[]){0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C, 0xD2, 0xFF, 0x10, 0x20,
                       0xFD, 0x20, 0xC0, 0x00}, 17, 0},
    {0xE8, (uint8_t[]){0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC, 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E}, 14, 0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7, (uint8_t[]){0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1, 0x50, 0x00,
                       0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45,
                       0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77},
     36, 0},
    {0xEA, (uint8_t[]){0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C}, 8, 0},
    {0xB0, (uint8_t[]){0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43}, 7, 0},
    {0xb7, (uint8_t[]){0x00, 0x00, 0x73, 0x73}, 4, 0},
    {0xBF, (uint8_t[]){0xA6, 0xAA}, 2, 0},
    {0xA9, (uint8_t[]){0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03}, 10, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 100},
    {0x29, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 100},
};

// --- PI4IOE5V6408 IO expander init ---
// Two chips: PI4IOE1 (0x43) controls LCD_RST, TP_RST, SPK_EN, EXT5V, CAM_RST
//            PI4IOE2 (0x44) controls WLAN_PWR, USB5V, CHG_EN
static void pi4ioe_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source              = I2C_CLK_SRC_DEFAULT,
        .i2c_port                = I2C_BUS_NUM,
        .sda_io_num              = I2C_SDA_GPIO,
        .scl_io_num              = I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    i2c_master_dev_handle_t dev2;
    i2c_device_config_t dev_cfg1 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PI4IOE1_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg1, &s_pi4ioe1));

    uint8_t wb[2], rb[1];

    // PI4IOE1: P0=NC, P1=SPK_EN, P2=EXT5V_EN, P3=NC, P4=LCD_RST, P5=TP_RST, P6=CAM_RST, P7=IN
    wb[0] = PI4IO_REG_CHIP_RESET; wb[1] = 0xFF;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_CHIP_RESET;
    i2c_master_transmit_receive(s_pi4ioe1, wb, 1, rb, 1, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_IO_DIR; wb[1] = 0b01111111;  // P0-P6 output, P7 input
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_OUT_H_IM; wb[1] = 0b00000000;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_SEL; wb[1] = 0b01111111;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_EN; wb[1] = 0b01111111;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    // Drive EXT5V_EN(P2), LCD_RST(P4), TP_RST(P5), CAM_RST(P6) high; leave SPK_EN(P1) low
    wb[0] = PI4IO_REG_OUT_SET; wb[1] = 0b01110100;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);

    i2c_device_config_t dev_cfg2 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PI4IOE2_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg2, &dev2));

    // PI4IOE2: P0=WLAN_PWR_EN, P3=USB5V_EN, P7=CHG_EN
    wb[0] = PI4IO_REG_CHIP_RESET; wb[1] = 0xFF;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_CHIP_RESET;
    i2c_master_transmit_receive(dev2, wb, 1, rb, 1, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_IO_DIR; wb[1] = 0b10111001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_OUT_H_IM; wb[1] = 0b00000110;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_SEL; wb[1] = 0b10111001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_EN; wb[1] = 0b11111001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_IN_DEF_STA; wb[1] = 0b01000000;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_INT_MASK; wb[1] = 0b10111111;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    // Drive WLAN_PWR_EN(P0), USB5V_EN(P3) high
    wb[0] = PI4IO_REG_OUT_SET; wb[1] = 0b00001001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
}

// --- RGB565 helpers ---

static inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
}

static inline void rgb565_to_rgb888(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >>  5) & 0x3F) << 2;
    *b = ((c >>  0) & 0x1F) << 3;
}

// --- board_interface.h implementation ---

void board_init(void)
{
    ESP_LOGI(TAG, "%s init", BOARD_NAME);

    // 1. I2C + IO expander (must happen before display init to assert LCD_RST)
    pi4ioe_init();

    // 2. LDO for MIPI DSI PHY
    esp_ldo_channel_handle_t phy_ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo));

    // 3. Backlight PWM (LEDC, 12-bit, 5 kHz)
    const ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_LEDC_DUTY_RES,
        .timer_num       = LCD_LEDC_TIMER,
        .freq_hz         = LCD_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    const ledc_channel_config_t bl_chan = {
        .gpio_num   = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LCD_LEDC_CHAN,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LCD_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_chan));

    // 4. MIPI DSI bus (2 lanes, 965 Mbps)
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t dsi_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = 2,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_BITRATE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_cfg, &dsi_bus));

    // 5. DBI panel IO (command channel over DSI)
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io));

    // 6. DPI panel (pixel data)
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = LCD_W,
            .v_size            = LCD_H,
            .hsync_pulse_width = 2,
            .hsync_back_porch  = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 2,
            .vsync_back_porch  = 8,
            .vsync_front_porch = 220,
        },
        .flags.use_dma2d = true,
    };

    // 7. ST7123 vendor config
    st7123_vendor_config_t vendor_cfg = {
        .init_cmds      = s_st7123_init,
        .init_cmds_size = sizeof(s_st7123_init) / sizeof(s_st7123_init[0]),
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = 2,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
        .vendor_config  = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(io, &dev_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // 8. Get hardware framebuffer and allocate render buffer
    void *fb0 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb0));
    s_fb = (uint8_t *)fb0;

    s_backbuf = heap_caps_aligned_calloc(64, FB_SIZE, 1,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(s_backbuf);
    memset(s_fb, 0, FB_SIZE);
    esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // 9. Backlight on (100%)
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHAN, LCD_LEDC_DUTY_MAX);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHAN);

    ESP_LOGI(TAG, "%s init done", BOARD_NAME);
}

const char *board_get_name(void) { return BOARD_NAME; }
bool        board_has_lcd(void)  { return s_panel != NULL; }

int board_lcd_width(void)  { return LCD_W; }
int board_lcd_height(void) { return LCD_H; }

void board_lcd_clear(void)
{
    if (s_backbuf) memset(s_backbuf, 0, FB_SIZE);
}

void board_lcd_flush(void)
{
    if (!s_backbuf || !s_fb) return;
    memcpy(s_fb, s_backbuf, FB_SIZE);
    esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

void board_lcd_fill(uint16_t color)
{
    if (!s_backbuf) return;
    uint16_t *p = (uint16_t *)s_backbuf;
    for (int i = 0; i < LCD_W * LCD_H; i++) p[i] = color;
    board_lcd_flush();
}

void board_lcd_set_pixel_raw(int x, int y, uint16_t color)
{
    if (!s_backbuf || x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    ((uint16_t *)s_backbuf)[y * LCD_W + x] = color;
}

void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    board_lcd_set_pixel_raw(x, y, rgb888_to_rgb565(r, g, b));
}

uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return rgb888_to_rgb565(r, g, b);
}

uint16_t board_lcd_get_pixel_raw(int x, int y)
{
    if (!s_backbuf || x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return 0;
    return ((uint16_t *)s_backbuf)[y * LCD_W + x];
}

void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    rgb565_to_rgb888(color, r, g, b);
}

uint8_t *board_lcd_framebuffer(void)      { return s_backbuf; }
size_t   board_lcd_framebuffer_size(void) { return FB_SIZE; }
uint8_t *board_lcd_hw_framebuffer(void)   { return s_fb; }
void     board_lcd_commit(void)
{
    if (s_fb) esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

void board_lcd_sanity_test(void)
{
    if (!s_panel) return;
    ESP_LOGI(TAG, "Sanity test: cycling colors");
    static const struct { uint8_t r, g, b; } colors[] = {
        {255,   0,   0},
        {  0, 255,   0},
        {  0,   0, 255},
        {255, 255, 255},
        {  0,   0,   0},
    };
    for (int i = 0; i < 5; i++) {
        uint16_t px = rgb888_to_rgb565(colors[i].r, colors[i].g, colors[i].b);
        uint16_t *p = (uint16_t *)s_backbuf;
        for (int j = 0; j < LCD_W * LCD_H; j++) p[j] = px;
        board_lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

// ---------------------------------------------------------------------------
// Audio — ES8388 via I2S0 + shared I2C bus
// ---------------------------------------------------------------------------

esp_codec_dev_handle_t board_audio_init(void)
{
    if (s_spk_dev) return s_spk_dev;
    if (!s_i2c_bus) {
        ESP_LOGE(TAG, "board_audio_init: I2C bus not ready");
        return NULL;
    }

    // Enable speaker: set PI4IOE1 P1 (SPK_EN) high — was 0b01110100, now 0b01110110
    uint8_t wb[2];
    wb[0] = PI4IO_REG_OUT_SET; wb[1] = 0b01110110;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    ESP_LOGI(TAG, "SPK_EN asserted");

    // I2S TX channel with 480 ms DMA buffer
    i2s_chan_handle_t tx_chan;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear     = true;
    chan_cfg.dma_desc_num   = AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num  = AUDIO_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    // Codec data interface
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = (uint8_t)I2S_PORT,
        .tx_handle = tx_chan,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    // ES8388 I2C control interface — reuse existing bus handle
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = (uint8_t)I2C_BUS_NUM,
        .addr       = ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .hw_gain     = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&codec_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_spk_dev = esp_codec_dev_new(&dev_cfg);
    ESP_LOGI(TAG, "Audio init done: ES8388 @ I2C 0x%02x, I2S%d",
             ES8388_CODEC_DEFAULT_ADDR, I2S_PORT);
    return s_spk_dev;
}

esp_codec_dev_handle_t board_audio_speaker(void)
{
    return s_spk_dev;
}
