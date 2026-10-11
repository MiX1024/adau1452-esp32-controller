#include "ec11.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "portmacro.h"

#define PIN_A    GPIO_NUM_35
#define PIN_B    GPIO_NUM_36
#define PIN_BTN  GPIO_NUM_37

static const char *TAG = "ec11";

static volatile int32_t s_enc_count = 0;
static volatile bool s_button_pressed = false;
static volatile int s_state = 0;
static volatile int s_btn_state = 1;   /* 1 = 释放, 0 = 按下 */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 正交编码状态机：状态 = (A<<1)|B，00->01->11->10->00 为正转 */
static void IRAM_ATTR enc_isr(void *arg)
{
    int a = gpio_get_level(PIN_A);
    int b = gpio_get_level(PIN_B);
    int s = (a << 1) | b;

    static const int8_t trans[4][4] = {
        {  0,  1, -1,  0 },  /* from 00 */
        { -1,  0,  0,  1 },  /* from 01 */
        {  1,  0,  0, -1 },  /* from 10 */
        {  0, -1,  1,  0 },  /* from 11 */
    };
    int d = trans[s_state][s];
    s_state = s;

    if (d != 0) {
        portENTER_CRITICAL_ISR(&s_mux);
        s_enc_count += d;
        portEXIT_CRITICAL_ISR(&s_mux);
    }
}

static void IRAM_ATTR btn_isr(void *arg)
{
    static uint32_t last = 0;
    uint32_t now = xTaskGetTickCountFromISR();

    int level = gpio_get_level(PIN_BTN);

    if (level == 0 && s_btn_state == 1) {
        /* 释放 -> 按下（下降沿）：触发一次 */
        if (now - last < pdMS_TO_TICKS(30)) {
            return; /* 去抖，忽略按键抖动 */
        }
        last = now;
        portENTER_CRITICAL_ISR(&s_mux);
        s_button_pressed = true;
        portEXIT_CRITICAL_ISR(&s_mux);
        s_btn_state = 0;
    } else if (level == 1 && s_btn_state == 0) {
        /* 按下 -> 释放（上升沿）：仅复位状态，不触发 */
        s_btn_state = 1;
    }
}

void ec11_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_A) | (1ULL << PIN_B) | (1ULL << PIN_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_state = (gpio_get_level(PIN_A) << 1) | gpio_get_level(PIN_B);
    s_btn_state = gpio_get_level(PIN_BTN);

    gpio_install_isr_service(0);

    gpio_set_intr_type(PIN_A, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(PIN_B, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_A, enc_isr, NULL);
    gpio_isr_handler_add(PIN_B, enc_isr, NULL);

    gpio_set_intr_type(PIN_BTN, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_BTN, btn_isr, NULL);

    ESP_LOGI(TAG, "EC11 initialized (A=%d B=%d BTN=%d)", PIN_A, PIN_B, PIN_BTN);
}

int32_t ec11_read_and_clear(void)
{
    int32_t c;
    portENTER_CRITICAL(&s_mux);
    c = s_enc_count;
    s_enc_count = 0;
    portEXIT_CRITICAL(&s_mux);
    return c;
}

bool ec11_button_pressed(void)
{
    bool p;
    portENTER_CRITICAL(&s_mux);
    p = s_button_pressed;
    s_button_pressed = false;
    portEXIT_CRITICAL(&s_mux);
    return p;
}
