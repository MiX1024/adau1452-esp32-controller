#include "logbuf.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* 8KB 足够放下最近几百行日志；太小会频繁丢历史 */
#define LOGBUF_SIZE   8192
/* 单行截断长度：超长行切掉，避免一条日志吃掉整个缓冲 */
#define LOGBUF_LINE   220

static char s_buf[LOGBUF_SIZE];
static volatile size_t s_wr;      /* 写入位置（下一个字节写这里） */
static volatile size_t s_rd;      /* 未读起点 */
static volatile uint32_t s_lost;  /* 因覆盖而丢弃的字节数 */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig;

/* 往缓冲写一个字节（调用者需持锁）。满则推进读指针，丢弃最旧数据。 */
static inline void put_locked(char c)
{
    size_t next = (s_wr + 1) % LOGBUF_SIZE;
    if (next == s_rd) {
        s_rd = (s_rd + 1) % LOGBUF_SIZE;
        s_lost++;
    }
    s_buf[s_wr] = c;
    s_wr = next;
}

static int log_hook(const char *fmt, va_list args)
{
    /* 先格式化一份副本进缓冲（不改动原 args，因为最后还要交给原输出） */
    char tmp[LOGBUF_LINE];
    va_list cpy;
    va_copy(cpy, args);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, cpy);
    va_end(cpy);

    size_t len;
    if (n < 0) {
        len = 0;
    } else if ((size_t)n < sizeof(tmp)) {
        len = (size_t)n;
    } else {
        /* 被截断：保留内容并补个省略提示 */
        len = sizeof(tmp) - 1;
        if (len >= 4) {
            tmp[len - 4] = '.';
            tmp[len - 3] = '.';
            tmp[len - 2] = '.';
            tmp[len - 1] = '\n';
        }
    }

    if (len > 0) {
        portENTER_CRITICAL_SAFE(&s_mux);
        for (size_t i = 0; i < len; i++) {
            put_locked(tmp[i]);
        }
        portEXIT_CRITICAL_SAFE(&s_mux);
    }

    /* 原样输出到 UART / USB 串口 */
    if (s_orig) {
        return s_orig(fmt, args);
    }
    return vprintf(fmt, args);
}

/*
 * ⚠️⚠️ UTF-8 边界安全 —— 漏了这一条会**直接把网页整条 WebSocket 打断**。
 *
 * 日志帧走的是 WebSocket **文本帧**，浏览器解码失败就关连接：
 *     "WebSocket connection to 'ws://.../ws' failed:
 *      Could not decode a text frame as UTF-8"
 * 而且一断就是整页失去状态推送，看起来像设备没反应。
 *
 * 环形缓冲是**按字节**回绕的，中文一个字符占 3 字节，所以有两个缺口：
 *   1) 窗口起点可能落在某个汉字中间（续接字节 0x80~0xBF）；
 *   2) 窗口终点可能把一个汉字切成两半。
 * 其中 (1) 不只发生在快照上 —— `put_locked()` 溢出时也是**逐字节**推进
 * 读指针的，所以增量路径同样可能从字符中间开始。
 */

/* 该字节作为 UTF-8 起始字节时序列总长：1=ASCII，0=续接字节/非法 */
static int utf8_seq_len(uint8_t c)
{
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

/* 把环形读指针从字符中间推进到字符边界（调用者需持锁） */
static void utf8_skip_continuation(void)
{
    int guard = 3;                        /* UTF-8 最长 4 字节，最多跳 3 个续接字节 */
    while (s_rd != s_wr && guard-- > 0 &&
           utf8_seq_len((uint8_t)s_buf[s_rd]) == 0) {
        s_rd = (s_rd + 1) % LOGBUF_SIZE;
    }
}

/* 返回 out 末尾不完整的 UTF-8 序列占了几个字节（0 = 结尾合法） */
static size_t utf8_incomplete_tail(const char *out, size_t len)
{
    for (size_t back = 1; back <= 4 && back <= len; back++) {
        const uint8_t c = (uint8_t)out[len - back];
        const int need = utf8_seq_len(c);
        if (need == 1) {
            return 0;                     /* ASCII 收尾，安全 */
        }
        if (need > 1) {
            return (back < (size_t)need) ? back : 0;   /* 起始字节找到了，但不够长 */
        }
        /* need == 0：还是续接字节，继续往前找起始字节 */
    }
    return (len < 4) ? len : 0;            /* 连续 4 个续接字节 = 脏数据，原样放过 */
}

void logbuf_init(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    s_wr = 0;
    s_rd = 0;
    s_lost = 0;

    if (s_orig == NULL) {
        s_orig = esp_log_set_vprintf(log_hook);
    }
}

size_t logbuf_take(char *out, size_t cap)
{
    if (out == NULL || cap < 2) {
        return 0;
    }

    size_t got = 0;
    portENTER_CRITICAL_SAFE(&s_mux);
    utf8_skip_continuation();
    while (s_rd != s_wr && got < cap - 1) {
        out[got++] = s_buf[s_rd];
        s_rd = (s_rd + 1) % LOGBUF_SIZE;
    }
    /*
     * 末尾若是半个汉字，**退还回去**而不是切掉 —— 下一轮补齐后整字发出。
     * 切掉虽然也能保证合法，但那个字就永远丢了。
     */
    const size_t back = utf8_incomplete_tail(out, got);
    if (back > 0) {
        got -= back;
        s_rd = (s_rd + LOGBUF_SIZE - back) % LOGBUF_SIZE;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);

    out[got] = '\0';
    return got;
}

void logbuf_clear(void)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    s_rd = s_wr;
    s_lost = 0;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

/*
 * 轻量日志：只进环形缓冲，不碰 UART。
 *（桥接的逐报文日志用；走 ESP_LOGI 会因为串口 115200 而人为制造 6ms/行 的延迟）
 */
void logbuf_trace(const char *fmt, ...)
{
    if (fmt == NULL) {
        return;
    }
    char tmp[LOGBUF_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = ((size_t)n < sizeof(tmp)) ? (size_t)n : sizeof(tmp) - 1;

    portENTER_CRITICAL_SAFE(&s_mux);
    for (size_t i = 0; i < len; i++) {
        put_locked(tmp[i]);
    }
    put_locked('\n');
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void logbuf_sync_read(void)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    s_rd = s_wr;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

/* 只读快照：返回「最近写入」的 tail 数据（结尾对齐 s_wr），不推进读指针。
 * 注意不能只返回未读部分 —— 网页实时推送会不断消费未读数据，
 * 那样这里就永远是空的。调试查看要的是"最新尾巴"。
 * 用于 /api/log 的 GET 快速查看。 */
size_t logbuf_snapshot(char *out, size_t cap)
{
    if (out == NULL || cap < 2) {
        return 0;
    }

    size_t got = 0;
    portENTER_CRITICAL_SAFE(&s_mux);
    size_t room = (cap - 1 < LOGBUF_SIZE - 1) ? (cap - 1) : (LOGBUF_SIZE - 1);
    size_t start = (s_wr + LOGBUF_SIZE - room) % LOGBUF_SIZE;

    /* 起点若落在汉字中间就往后挪到字符边界（否则整段不是合法 UTF-8） */
    for (int i = 0; i < 3 && room > 0 &&
                    utf8_seq_len((uint8_t)s_buf[start]) == 0; i++) {
        start = (start + 1) % LOGBUF_SIZE;
        room--;
    }
    for (size_t i = 0; i < room; i++) {
        out[i] = s_buf[(start + i) % LOGBUF_SIZE];
    }
    got = room - utf8_incomplete_tail(out, room);
    portEXIT_CRITICAL_SAFE(&s_mux);

    out[got] = '\0';
    return got;
}

size_t logbuf_used(void)
{
    size_t used;
    portENTER_CRITICAL_SAFE(&s_mux);
    used = (s_wr >= s_rd) ? (s_wr - s_rd) : (LOGBUF_SIZE - s_rd + s_wr);
    portEXIT_CRITICAL_SAFE(&s_mux);
    return used;
}
