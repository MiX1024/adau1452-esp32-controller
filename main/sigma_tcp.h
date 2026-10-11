#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SigmaStudio TCP/IP 通道桥接服务
 *
 * SigmaStudio 的 "TCP/IP" 通信通道（ADAU144x/145x/146x）会在 PC 上当作调试探头，
 * 把 I2C 事务打包成 TCP 报文发给我们。AD 官方 sigma-tcp 的 i2c 后端就是**纯透传**：
 * 报文里的 paramAddr 直接当作 2 字节大端 I2C 子地址，后面跟裸数据。
 *
 * 报文格式（14 字节头，来自 AD wiki "TCP/IP Channels"）：
 *   写  0x09: [0]=0x09 [1]=safeload [2]=chan [3..6]=totalLen(BE32)
 *              [7]=chipAddr [8..11]=dataLen(BE32) [12..13]=addr(BE16) + data
 *   读  0x0A: [0]=0x0A [1..4]=totalLen(BE32) [5]=chipAddr
 *              [6..9]=dataLen(BE32) [10..11]=addr(BE16) [12..13]=reserved
 *   响应 0x0B: [0]=0x0B [1..4]=totalLen [5]=chipAddr [6..9]=dataLen
 *              [10..11]=addr [12]=success(0 OK/1 失败) [13]=0 + data
 * 写报文不需要回包。
 */

#define SIGMA_TCP_DEFAULT_PORT  8086

typedef struct {
    bool     enabled;        /* 是否随上电自动启动桥接服务 */
    bool     local_control;  /* 本地控制器（编码器/网页音量、电平）是否允许访问 DSP */
    /*
     * 逐报文日志：把每一条读/写报文（地址、长度）记进网页日志缓冲，
     * 用于在线排查 SigmaStudio 的访问行为。
     *
     * ⚠️ 只写内存环形缓冲、**不走 UART**（115200 下每行约 6ms，会人为制造延迟），
     * 但高频轮询时依然会产生大量日志，所以默认关闭。
     *
     * 注：「上电自动加载 DSP 程序」已移到 DSP 程序映像模块（dsp_image）。
     */
    bool     verbose;
    /*
     * ⚠️ 读应答之间的强制间隔（微秒）—— **这是修复 SigmaStudio 崩溃的关键参数**。
     *
     * SigmaStudio 把读请求**流水线**发出（实测一个 TCP 段里塞 3 个读请求），
     * 但它按"一次 recv 恰好拿到一条应答"解析。我们多条应答紧挨着发出时，
     * 它们会一起躺在它的接收缓冲里 → 它一次 recv 全拿走 → 错位解析
     * → 野指针崩溃（0xc0000005，模块 unknown，直接闪退）。
     *
     * ⚠️ 关键：TCP 是字节流，**间隔必须大于"SigmaStudio 处理一条应答的时间"**，
     * 才能真正把它分开。探针实验里 `time.sleep(0.002)` 在 Windows 上实际
     * 睡了约 15ms（系统定时器精度），因此生效值在 **10~20ms** 量级；
     * 固件里试 2ms / 5ms 都不够，就是这个原因。
     *
     * 代价：每轮轮询的应答数 × 间隔 = 额外耗时（如 4 × 15ms = 60ms → 约 16Hz 刷新）。
     * 调小可提高刷新率，但低于临界值就会重新崩溃。0 = 关闭。
     */
    uint32_t resp_gap_us;    /* ⚠️ 必须 32 位：上限 200000 超出 uint16_t 范围 */
    uint16_t port;
} sigma_tcp_config_t;

typedef struct {
    bool     running;        /* 服务是否已启动 */
    bool     client;         /* 是否有客户端连接 */
    char     client_ip[16];
    uint16_t port;
    uint32_t pkt_write;      /* 收到的写报文数 */
    uint32_t pkt_read;       /* 收到的读报文数 */
    uint32_t errors;         /* 出错次数 */
    uint32_t conn_count;     /* 累计连接次数 —— 若持续增长说明 SigmaStudio 在反复重连 */
    uint32_t active;         /* 当前活跃连接数（SigmaStudio 会并发开多条） */
    uint32_t disconnects;    /* 已结束的会话数 */
    uint32_t resync_bytes;   /* 累计滑动重同步字节数 —— 若增长说明流真的错位了 */
    uint32_t bytes;          /* 转发的数据字节数 */
    uint16_t last_addr;      /* 最近访问的 DSP 地址 */
    uint32_t last_len;
    bool     last_is_read;
    uint16_t last_safeload;

    /*
     * 读响应耗时统计（微秒）。用来把"慢"拆成两部分：
     *   i2c_us = I2C 事务本身；lat_us = 端到端（含调度 + TCP 发送）
     * 若 i2c_us 远小于 lat_us，说明瓶颈在调度/网络而不是 I2C；
     * lat_over_10ms 是抖动计数 —— SigmaStudio 超时崩溃多半是抖动而不是均值。
     */
    uint32_t lat_last_us;    /* 最近一次读的端到端耗时 */
    uint32_t lat_max_us;     /* 本次连接内端到端最大值 */
    uint32_t lat_avg_us;     /* 本次连接内端到端平均值 */
    uint32_t i2c_last_us;    /* 最近一次读里纯 I2C 事务耗时 */
    uint32_t i2c_max_us;     /* 本次连接内 I2C 最大值 */
    uint32_t lat_over_10ms;  /* 端到端超过 10ms 的次数（网络抖动指标） */
} sigma_tcp_status_t;

/* 从 NVS 载入配置到内存缓存（不启动服务）；可在 dsp_image 回放前先调用 */
esp_err_t sigma_tcp_config_load(void);

/* 从 NVS 载入配置；若 enabled 则同时启动监听。
 * ⚠️ 必须在 lwIP 初始化之后调用（即 net_init() 之后），
 *    否则 socket() 会触发 "tcpip_send_msg_wait_sem Invalid mbox" 断言复位。 */
esp_err_t sigma_tcp_init(void);

/* 读取当前配置（内存缓存） */
sigma_tcp_config_t sigma_tcp_get_config(void);

/* 应答间隔默认值（微秒）。
 *
 * ⚠️ 实测边界（用户实测，很重要）：
 *   15000（15ms）→ 坚持一会儿会崩（**不够，在临界值附近**）
 *   30000（30ms）→ 长时间不崩 ✓
 * 因为间隔必须**大于 SigmaStudio 处理一条应答的时间**（它的周期就在 15~30ms 间），
 * 15ms 只是偶尔踩线。默认取 30ms 留出余量。
 *
 * 上限 200000（200ms），便于必要时继续往上调。 */
#define SIGMA_RESP_GAP_DEFAULT_US  30000
#define SIGMA_RESP_GAP_MAX_US      200000

/* 更新配置：写入 NVS 并按需要启动/停止服务。
 * 注意：「上电自动加载」已移到 dsp_image 模块，不再属于桥接配置。 */
esp_err_t sigma_tcp_set_config(bool enabled, uint16_t port, bool local_control,
                               uint32_t resp_gap_us);

/* 逐报文日志开关（默认关） */
esp_err_t sigma_tcp_set_verbose(bool on);
bool      sigma_tcp_verbose(void);

/* 启动监听（port 为 0 时用默认 8086）。⚠️ 需 lwIP 已初始化 */
esp_err_t sigma_tcp_start(uint16_t port);

/* 停止服务并断开客户端 */
esp_err_t sigma_tcp_stop(void);

void sigma_tcp_get_status(sigma_tcp_status_t *out);

/* 桥接正占用 I2C：已启动且有客户端连接。此时本地控制器必须停止访问 DSP */
bool sigma_tcp_busy(void);

#ifdef __cplusplus
}
#endif
