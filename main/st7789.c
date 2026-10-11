#include "st7789.h"
#include "font5x7.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SCL   GPIO_NUM_14
#define PIN_SDA   GPIO_NUM_13   /* MOSI */
#define PIN_RES   GPIO_NUM_12
#define PIN_DC    GPIO_NUM_11
#define PIN_CS    GPIO_NUM_10
#define PIN_BLK   GPIO_NUM_9

/* 背光亮度：0~100（百分比） */
#define BACKLIGHT_PERCENT   40

/* 旋转/颜色方向：若画面上下颠倒可改成 0xA0；若颜色红蓝反了可改成 0x68 */
#define ST7789_MADCTL  0x60

#define SPI_HOST_DEV   SPI2_HOST
#define SPI_CLK_HZ     (80 * 1000 * 1000)   /* ST7789 支持 80MHz SPI */

/* ESP32-S3 SPI DMA 单次传输最大 32KB（SPI_LL_DMA_MAX_BIT_LEN = 1<<18 bit） */
#define SPI_MAX_CHUNK   (32 * 1024)

static const char *TAG = "st7789";

static spi_device_handle_t s_spi = NULL;
static uint16_t *s_fb = NULL;

static void st7789_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_data[0] = cmd;
    t.flags = SPI_TRANS_USE_TXDATA;
    spi_device_polling_transmit(s_spi, &t);
}

static void st7789_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(s_spi, &t);
}

static void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t d[4];

    st7789_cmd(0x2A); /* CASET */
    d[0] = (x0 >> 8) & 0xFF; d[1] = x0 & 0xFF;
    d[2] = (x1 >> 8) & 0xFF; d[3] = x1 & 0xFF;
    st7789_data(d, 4);

    st7789_cmd(0x2B); /* RASET */
    d[0] = (y0 >> 8) & 0xFF; d[1] = y0 & 0xFF;
    d[2] = (y1 >> 8) & 0xFF; d[3] = y1 & 0xFF;
    st7789_data(d, 4);

    st7789_cmd(0x2C); /* RAMWR */
}

esp_err_t st7789_init(void)
{
    esp_err_t ret;

    /* 背光：LEDC PWM 控制亮度 */
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_ch = {
        .gpio_num   = PIN_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = (BACKLIGHT_PERCENT * 255) / 100,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_ch);

    /* DC / RES */
    gpio_config_t ctrl = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RES),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ctrl);

    /* 硬件复位 */
    gpio_set_level(PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* SPI 总线 */
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_SDA,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_CHUNK,   /* 单次 DMA 传输最大 32KB */
    };
    ret = spi_bus_initialize(SPI_HOST_DEV, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLK_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 4,
    };
    ret = spi_bus_add_device(SPI_HOST_DEV, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 初始化序列 */
    st7789_cmd(0x01); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));

    st7789_cmd(0x11); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t madctl = ST7789_MADCTL;
    st7789_cmd(0x36); st7789_data(&madctl, 1);

    uint8_t colmod = 0x55; /* 16bit/pixel */
    st7789_cmd(0x3A); st7789_data(&colmod, 1);

    static const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    st7789_cmd(0xB2); st7789_data(porctrl, sizeof(porctrl));

    uint8_t gctrl = 0x35;
    st7789_cmd(0xB7); st7789_data(&gctrl, 1);

    uint8_t vcoms = 0x19;
    st7789_cmd(0xBB); st7789_data(&vcoms, 1);

    uint8_t lcmctrl = 0x2C;
    st7789_cmd(0xC0); st7789_data(&lcmctrl, 1);

    uint8_t vdv = 0x01;
    st7789_cmd(0xC2); st7789_data(&vdv, 1);

    uint8_t vrsh = 0x12;
    st7789_cmd(0xC3); st7789_data(&vrsh, 1);

    uint8_t vdvs = 0x20;
    st7789_cmd(0xC4); st7789_data(&vdvs, 1);

    uint8_t frctrl2 = 0x0F;
    st7789_cmd(0xC6); st7789_data(&frctrl2, 1);

    static const uint8_t pwctrl1[] = {0xA4, 0xA1};
    st7789_cmd(0xD0); st7789_data(pwctrl1, sizeof(pwctrl1));

    static const uint8_t pvgam[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
    st7789_cmd(0xE0); st7789_data(pvgam, sizeof(pvgam));

    static const uint8_t nvgam[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
    st7789_cmd(0xE1); st7789_data(nvgam, sizeof(nvgam));

    st7789_cmd(0x20); /* INVOFF：关闭反色，保证黑底黑、绿色绿 */
    st7789_cmd(0x29); /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 分配帧缓冲（内部 RAM，DMA 可访问） */
    s_fb = heap_caps_malloc(ST7789_WIDTH * ST7789_HEIGHT * 2, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    st7789_fill(COLOR_BLACK);
    st7789_flush();

    ESP_LOGI(TAG, "ST7789 initialized");
    return ESP_OK;
}

/* ST7789 颜色数据要求高字节在前（big-endian），而 ESP32 是小端。
 * 写入帧缓冲时交换字节，flush 直接按字节发送即可得到正确颜色。 */
static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

uint16_t st7789_color_swap(uint16_t color)
{
    return swap16(color);
}

uint16_t *st7789_framebuffer(void)
{
    return s_fb;
}

void st7789_fill(uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }
    uint16_t c = swap16(color);
    size_t n = ST7789_WIDTH * ST7789_HEIGHT;
    for (size_t i = 0; i < n; i++) {
        s_fb[i] = c;
    }
}

void st7789_flush(void)
{
    if (s_fb == NULL || s_spi == NULL) {
        return;
    }

    st7789_set_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);

    gpio_set_level(PIN_DC, 1);
    uint8_t *p = (uint8_t *)s_fb;
    uint32_t remaining = ST7789_WIDTH * ST7789_HEIGHT * 2;
    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > SPI_MAX_CHUNK) {
            chunk = SPI_MAX_CHUNK;
        }
        spi_transaction_t t = {0};
        t.length = chunk * 8;
        t.tx_buffer = p;
        spi_device_polling_transmit(s_spi, &t);
        p += chunk;
        remaining -= chunk;
    }
}

void st7789_flush_rect(int x, int y, int w, int h)
{
    if (s_fb == NULL || s_spi == NULL) {
        return;
    }

    /* 边界裁剪 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ST7789_WIDTH)  { w = ST7789_WIDTH - x; }
    if (y + h > ST7789_HEIGHT) { h = ST7789_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    st7789_set_window((uint16_t)x, (uint16_t)y,
                      (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));

    gpio_set_level(PIN_DC, 1);

    if (x == 0 && w == ST7789_WIDTH) {
        /* 全宽区域：帧缓冲连续，按 32KB 分块 DMA 发送 */
        uint8_t *p = (uint8_t *)&s_fb[y * ST7789_WIDTH];
        uint32_t remaining = (uint32_t)w * h * 2;
        while (remaining > 0) {
            uint32_t chunk = remaining;
            if (chunk > SPI_MAX_CHUNK) {
                chunk = SPI_MAX_CHUNK;
            }
            spi_transaction_t t = {0};
            t.length = chunk * 8;
            t.tx_buffer = p;
            spi_device_polling_transmit(s_spi, &t);
            p += chunk;
            remaining -= chunk;
        }
    } else {
        /* 非全宽区域：逐行发送 */
        for (int row = 0; row < h; row++) {
            uint8_t *p = (uint8_t *)&s_fb[(y + row) * ST7789_WIDTH + x];
            spi_transaction_t t = {0};
            t.length = (size_t)w * 2 * 8;
            t.tx_buffer = p;
            spi_device_polling_transmit(s_spi, &t);
        }
    }
}

void st7789_draw_pixel(int x, int y, uint16_t color)
{
    if (s_fb == NULL || x < 0 || y < 0 || x >= ST7789_WIDTH || y >= ST7789_HEIGHT) {
        return;
    }
    s_fb[y * ST7789_WIDTH + x] = swap16(color);
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }

    /* 边界裁剪 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ST7789_WIDTH)  { w = ST7789_WIDTH - x; }
    if (y + h > ST7789_HEIGHT) { h = ST7789_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    /* 快路径：直接写帧缓冲，避免逐像素函数调用开销 */
    uint16_t c = swap16(color);
    uint16_t *row = &s_fb[y * ST7789_WIDTH + x];
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            row[i] = c;
        }
        row += ST7789_WIDTH;
    }
}

void st7789_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    const uint8_t *g = font5x7_glyph(c);
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            uint16_t color = (bits & (0x10 >> col)) ? fg : bg;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    st7789_draw_pixel(x + col * scale + dx, y + row * scale + dy, color);
                }
            }
        }
    }
}

void st7789_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (s == NULL) {
        return;
    }
    int cx = x;
    while (*s) {
        st7789_draw_char(cx, y, *s, fg, bg, scale);
        cx += 6 * scale;
        s++;
    }
}

int st7789_text_width(const char *s, int scale)
{
    if (s == NULL) {
        return 0;
    }
    return (int)strlen(s) * 6 * scale;
}

int st7789_text_height(int scale)
{
    return 8 * scale;
}
