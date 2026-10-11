#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 EC11 编码器（A/B 旋转 + PUSH 按键） */
void ec11_init(void);

/* 读取并清零累计的编码器计数（正转/反转），随后清零 */
int32_t ec11_read_and_clear(void);

/* 返回自上次调用以来是否有按键按下（读取后自动清除） */
bool ec11_button_pressed(void);

#ifdef __cplusplus
}
#endif
