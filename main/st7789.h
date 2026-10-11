#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_WIDTH   320
#define ST7789_HEIGHT  240

/* 颜色（RGB565） */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_GRAY    0x8410
#define COLOR_DKGRAY  0x4208
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_ORANGE  0xFD20
#define COLOR_RED     0xF800

/* 初始化 SPI 与 ST7789 */
esp_err_t st7789_init(void);

/* 把整个帧缓冲刷到屏幕 */
void st7789_flush(void);

/* 局部刷新：只把帧缓冲中指定矩形区域刷到屏幕（提高动态画面流畅度） */
void st7789_flush_rect(int x, int y, int w, int h);

/* 绘制基础图元 */
void st7789_fill(uint16_t color);
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
void st7789_draw_pixel(int x, int y, uint16_t color);

/* 高性能批量像素写入：返回帧缓冲首指针（像素需先做字节序交换）。
 * 未初始化时返回 NULL；调用方需自行边界检查。行主序，每行 ST7789_WIDTH 像素。 */
uint16_t *st7789_framebuffer(void);

/* 颜色字节序交换（帧缓冲要求高字节在前） */
uint16_t st7789_color_swap(uint16_t color);

/* 文本绘制（scale 为整数放大倍数） */
void st7789_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void st7789_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);

/* 文本尺寸辅助 */
int st7789_text_width(const char *s, int scale);
int st7789_text_height(int scale);

#ifdef __cplusplus
}
#endif
