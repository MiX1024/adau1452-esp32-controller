#include "eq_store.h"
#include "adau1452.h"
#include "sigma_tcp.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eqstore";

#define EQ_NVS_NS    "eq"
#define EQ_NVS_KEY   "bands"

/* 落盘格式：一个 blob，前面是条数，后面是定长数组。
   ⚠️ 一定整体存取（和 bind 一样），否则会出现"改了一半"的中间状态。 */
typedef struct {
    uint16_t   count;
    uint16_t   pad;
    eq_band_t  b[EQ_STORE_MAX];
} eq_blob_t;

static eq_blob_t s_eq;
static int       s_applied = 0;       /* 上一次 apply 成功写回几段 */

/* ---------- NVS ---------- */

static void eq_save(void)
{
    nvs_handle_t h;
    if (nvs_open(EQ_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败，EQ 无法保存");
        return;
    }
    esp_err_t e = nvs_set_blob(h, EQ_NVS_KEY, &s_eq, sizeof(s_eq));
    if (e == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "EQ 保存失败: %s", esp_err_to_name(e));
    }
    nvs_close(h);
}

void eq_store_init(void)
{
    memset(&s_eq, 0, sizeof(s_eq));
    nvs_handle_t h;
    if (nvs_open(EQ_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_eq);
        if (nvs_get_blob(h, EQ_NVS_KEY, &s_eq, &sz) != ESP_OK || sz != sizeof(s_eq)) {
            memset(&s_eq, 0, sizeof(s_eq));
        }
        nvs_close(h);
    }
    if (s_eq.count > EQ_STORE_MAX) {
        s_eq.count = EQ_STORE_MAX;
    }
    /* 顺手丢掉 addr == 0 的脏项（比如写入过程中掉电） */
    int n = 0;
    for (int i = 0; i < s_eq.count; i++) {
        if (s_eq.b[i].addr != 0) {
            s_eq.b[n++] = s_eq.b[i];
        }
    }
    s_eq.count = (uint16_t)n;
    ESP_LOGI(TAG, "EQ 已恢复 %d 段", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  [%d] addr=%-6u key=%s", i, s_eq.b[i].addr, s_eq.b[i].key);
    }
}

/* ---------- 查询 ---------- */

int eq_store_count(void)
{
    return s_eq.count;
}

int eq_store_applied(void)
{
    return s_applied;
}

int eq_store_get(eq_band_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < s_eq.count && n < max; i++) {
        out[n++] = s_eq.b[i];
    }
    return n;
}

static int eq_index_of(const char *key, uint16_t addr)
{
    if (key && key[0]) {
        for (int i = 0; i < s_eq.count; i++) {
            if (strncmp(s_eq.b[i].key, key, EQ_STORE_KEY_LEN - 1) == 0) {
                return i;
            }
        }
    }
    if (addr) {
        for (int i = 0; i < s_eq.count; i++) {
            if (s_eq.b[i].addr == addr) {
                return i;
            }
        }
    }
    return -1;
}

/* ---------- 写 DSP ---------- */

/*
 * 一段 5 个字 = 20 字节，正好是 adau1452_write_block() 内存区一个分块的上限
 * （28 字节）之内，所以是**单次 I2C 事务**写完 —— 这一点很重要：
 * 模块本身是 EQS300 Multi-Channel HW Slew，目标系数由硬件插值过渡，
 * 一次写完才不会出现中间态爆音。
 *
 * ⚠️ 桥接服务开启时（或本地控制被关掉）绝不能写：
 *    那时地址可能已经指向完全不同的参数，写进去会破坏 SigmaStudio 正在调的程序。
 */
static bool eq_write_dsp(const eq_band_t *b)
{
    if (b->addr == 0) {
        return false;
    }
    const sigma_tcp_config_t sc = sigma_tcp_get_config();
    if (sc.enabled || !sc.local_control) {
        return false;
    }
    uint8_t buf[EQ_STORE_WORDS * 4];
    for (int i = 0; i < EQ_STORE_WORDS; i++) {
        uint32_t v = b->w[i];
        buf[i * 4 + 0] = (uint8_t)(v >> 24);
        buf[i * 4 + 1] = (uint8_t)(v >> 16);
        buf[i * 4 + 2] = (uint8_t)(v >> 8);
        buf[i * 4 + 3] = (uint8_t)v;
    }
    return adau1452_write_block(b->addr, buf, sizeof(buf)) == ESP_OK;
}

void eq_store_apply(void)
{
    if (s_eq.count == 0) {
        s_applied = 0;
        return;
    }
    int ok = 0;
    for (int i = 0; i < s_eq.count; i++) {
        if (eq_write_dsp(&s_eq.b[i])) {
            ok++;
        }
        taskYIELD();
    }
    s_applied = ok;
    ESP_LOGI(TAG, "EQ 写回 DSP：%d/%d 段成功", ok, s_eq.count);
}

/* ---------- 增删改 ---------- */

bool eq_store_put(const eq_band_t *b)
{
    if (b == NULL || b->addr == 0) {
        return false;
    }
    const int at = eq_index_of(b->key, b->addr);
    if (at >= 0) {
        s_eq.b[at] = *b;
    } else {
        if (s_eq.count >= EQ_STORE_MAX) {
            /* 满了：丢掉最老的一段，腾位置（网页端每段都是独立 key，不会误伤） */
            memmove(&s_eq.b[0], &s_eq.b[1], sizeof(eq_band_t) * (EQ_STORE_MAX - 1));
            s_eq.count = EQ_STORE_MAX - 1;
            ESP_LOGW(TAG, "EQ 存储已满，丢弃最旧的一段");
        }
        s_eq.b[s_eq.count++] = *b;
    }
    const bool wrote = eq_write_dsp(b);
    eq_save();
    ESP_LOGI(TAG, "EQ 已保存 addr=%u (%s) 写 DSP %s", b->addr,
             b->key, wrote ? "成功" : "未执行/失败");
    return wrote;
}

bool eq_store_del(const char *key, uint16_t addr)
{
    const int at = eq_index_of(key, addr);
    if (at < 0) {
        return false;
    }
    for (int i = at; i < s_eq.count - 1; i++) {
        s_eq.b[i] = s_eq.b[i + 1];
    }
    s_eq.count--;
    memset(&s_eq.b[s_eq.count], 0, sizeof(eq_band_t));
    /* 删掉的那段本来就没写进 DSP，计数要跟着收一下，
       否则主循环的"未完全写回"判断会把已删的段也算进去 */
    if (s_applied > s_eq.count) {
        s_applied = s_eq.count;
    }
    eq_save();
    ESP_LOGI(TAG, "EQ 已移除 addr=%u key=%s", addr, key ? key : "");
    return true;
}

void eq_store_clear(void)
{
    memset(&s_eq.b, 0, sizeof(s_eq.b));
    s_eq.count = 0;
    s_applied = 0;
    eq_save();
    ESP_LOGI(TAG, "EQ 保存表已清空（DSP 当前值未改动）");
}
