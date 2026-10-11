#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 日志环形缓冲：把 ESP-IDF 的日志额外抄一份到内存，供网页实时查看。
 *
 * 实现方式：用 esp_log_set_vprintf() 挂钩子，既原样输出到 UART（不影响串口调试），
 * 又追加进环形缓冲。网页开启「显示日志」后，主循环把新增内容通过 WebSocket 推过去。
 *
 * 注意：钩子可能在任意任务/中断里被调用，写入用自旋锁保护且**绝不阻塞、绝不分配内存**，
 * 缓冲满时直接覆盖最旧数据（调试日志丢旧的没关系）。
 */

/* 8KB 环形缓冲：足够放下最近几百行日志。开启网页日志时会先把这段历史推过去。 */
void logbuf_init(void);

/* 复制「上次读取之后」的新增日志到 out（最多 cap-1 字节，自动补 '\0'），
 * 返回复制的字节数。无人读取时缓冲会自然回绕。 */
size_t logbuf_take(char *out, size_t cap);

/* 只读快照最近 cap-1 字节日志（不推进读指针，不影响实时推送） */
size_t logbuf_snapshot(char *out, size_t cap);

/* 把读指针推到写指针（丢弃"未读"标记），用于先补历史快照、之后只收新增的场景 */
void logbuf_sync_read(void);

/* 清空缓冲 */
void logbuf_clear(void);

/* 只写网页日志环、**不走 UART** 的轻量日志（自动补换行）。
 *
 * 用途：桥接的逐报文日志这类高频输出。走 ESP_LOGI 的话 115200 下每行
 * 约 6ms，会人为制造延迟（以前排查 SigmaStudio 崩溃时就这么被坑过），
 * 而且串口会被刷爆。这里只拷进环形缓冲，代价极低。
 * 注意：钩子可能在任意任务里被调用，本函数绝不阻塞、绝不分配内存。 */
void logbuf_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 缓冲内当前可读字节数（用于显示占用） */
size_t logbuf_used(void);

#ifdef __cplusplus
}
#endif
