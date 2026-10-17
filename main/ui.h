#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音量通道定义（显示名 + 参数地址 + dB 范围 + 是否按百分比显示） */
typedef struct {
    const char *name;
    uint16_t addr;
    float db;
    float min_db;
    float max_db;
    bool show_percent;
} volume_channel_t;

/* DSP 状态（供状态栏显示） */
typedef enum {
    UI_DSP_OFFLINE   = 0,   /* 红点 + 1452 OFFLINE */
    UI_DSP_ONLINE    = 1,   /* 绿点 + 1452 ONLINE  */
    UI_DSP_BRIDGE    = 2,   /* 青点 + SIGMASTUDIO（在线调试中，本地控制让位） */
    /*
     * 本地控制被用户关掉了（web 里「本地控制」未勾选）。
     * ⚠️ 这时 DSP 其实是好的，只是本地控制器不去读它，所以电平表拿不到数据。
     * 早期实现会归为 UI_DSP_OFFLINE，屏幕上显示 "1452 OFFLINE" 且两条空电平条 ——
     * 很容易被误判为 DSP 掉线。现在单独一类，屏幕直接告知原因。
     */
    UI_DSP_LOCAL_OFF = 3,   /* 橙点 + LOCAL CTL OFF */
} ui_dsp_state_t;

/*
 * 桥接调试页要显示的信息（仅在 dsp_state == UI_DSP_BRIDGE 时用到）。
 * 设计目标：**一眼就能判断"SigmaStudio 还连着吗、跑得顺不顺"**，
 * 所以在本地电平表完全让位的同时，把链路与转发状态全屏展示出来。
 */
typedef struct {
    /* --- SigmaStudio 链接 --- */
    char     client_ip[16];   /* 连接方 IP */
    uint16_t port;            /* 监听端口 */
    uint8_t  clients;         /* 当前活跃连接数 */
    /* --- 转发统计 --- */
    uint32_t pkt_write;       /* 已转发的写报文数 */
    uint32_t pkt_read;        /* 已回包的读报文数 */
    uint16_t last_addr;       /* 最近访问的 DSP 地址 */
    uint32_t last_len;        /* 最近一次的长度（字节） */
    bool     last_is_read;    /* 最近一次是读还是写 */
    uint32_t lat_avg_us;      /* 读响应平均耗时 */
    uint32_t errors;          /* 出错次数 */
    /* --- 本机网络 --- */
    char     local_ip[16];    /* 本机 STA IP（未连接时为空） */
    int16_t  rssi;            /* dBm（0 = 无） */
    uint8_t  channel;         /* WiFi 信道（0 = 无） */

    /* --- USBi(USB) 模式专用 --- */
    bool     is_usb;          /* true = USB(USBi) 调试页，false = TCP 桥接 */
    bool     usb_mounted;     /* USB 已连接（主机已复位总线） */
    bool     usb_configured;  /* 主机已配置（Windows CYUSB 驱动已接管） */
    uint8_t  usb_format;      /* SigmaStudio 选的通信模式：0=I2C，1=SPI */
} ui_bridge_info_t;

/* 初始化 UI（重置内部电平平滑 / 峰值保持 / 刷新快照状态） */
void ui_init(void);

/* 每帧渲染 UI。内部完成电平平滑、峰值保持、局部刷新。
 * channels 固定 4 个（MASTER/LOW/HIGH/MID），sel 为当前选中通道索引。
 * dsp_state 为 DSP 状态（离线/在线/桥接中）；wifi_ok 为家庭 WiFi 连接状态；
 * lv_l / lv_r 为左右声道瞬时电平（dB）。
 * 电平读取失败时传 < -200 的值，内部按静音处理。
 * br 为桥接调试信息（可为 NULL）；dsp_state == UI_DSP_BRIDGE 时显示全屏调试页。 */
void ui_render(const volume_channel_t *channels, int sel, ui_dsp_state_t dsp_state,
               bool wifi_ok, float lv_l, float lv_r,
               const ui_bridge_info_t *br);

/* 百分比 <-> dB 映射（主逻辑的百分比步进会用到） */
float ui_db_to_percent_float(float db);
float ui_percent_to_db(float percent);

#ifdef __cplusplus
}
#endif
