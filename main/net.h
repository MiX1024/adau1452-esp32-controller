#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 首页控件的「绑定」：把页面上的 4 个音量通道 + 左右电平表，
 * 对到 SigmaStudio 工程里某个具体参数上。
 *
 * 之所以需要它：DSP 程序一改，地址就整体平移。把绑定存成
 * **名字（cell|parameter）**而不是地址，重新导入工程文件后就能自动重新解析并写回。
 *
 * 槽位：0..3 = 音量通道，4 = 左声道电平，5 = 右声道电平。
 */
#define NET_BIND_SLOTS   6
#define NET_BIND_NAME_LEN 20
#define NET_BIND_KEY_LEN  72

typedef struct {
    uint16_t addr;                     /* 0 = 未绑定，退回固件默认地址 */
    char     name[NET_BIND_NAME_LEN];  /* 首页上显示的名字 */
    char     key[NET_BIND_KEY_LEN];    /* 来源 "cell|parameter"，供重新解析用 */
} net_bind_slot_t;

/* 网络模块回调：由主逻辑（main.c）提供，运行环境为 httpd 任务。 */
typedef struct {
    /* 设置某通道音量（dB），内部负责钳位 + 写 DSP。返回是否成功。 */
    bool (*set_volume_db)(int idx, float db);

    /* 读取当前状态：4 通道 dB 值 / 范围，选中通道，DSP 状态，左右电平。 */
    void (*get_state)(float db[4], float min_db[4], float max_db[4],
                      int *sel, bool *dsp_ok, float *lv_l, float *lv_r);

    /* 通道显示名（idx 0..3） */
    const char *(*channel_name)(int idx);

    /* 该通道是否以百分比显示（仅主音量） */
    bool (*channel_is_percent)(int idx);

    /* 绑定表读取/写入（首页控件 ↔ SigmaStudio 模块）。可为 NULL。 */
    void (*bind_get)(net_bind_slot_t out[NET_BIND_SLOTS]);
    bool (*bind_set)(const net_bind_slot_t in[NET_BIND_SLOTS]);

    /* 安全模式读取/写入。开启后上电不主动写 DSP 参数（防 SigmaStudio
     * 改过程序后 ESP32 写旧地址导致爆音）。可为 NULL。 */
    bool (*safe_get)(void);
    bool (*safe_set)(bool on);
} net_callbacks_t;

/* 初始化 WiFi（AP 常开 + 可选 STA）、HTTP 服务器、WebSocket、mDNS、OTA。 */
esp_err_t net_init(const net_callbacks_t *cb);

/* 由主循环周期调用：向 Web 客户端推送状态（内部限频约 100ms）。
 * 若网页订阅了日志，日志会附在同一帧里（同一 fd 不能并发发送两帧）。 */
void net_push_state(void);

/* 清空日志缓冲 */
void net_log_clear(void);

/* 是否已连接家庭 WiFi（STA 拿到 IP）。供 UI 显示 WiFi 图标。 */
bool net_wifi_connected(void);

/* 本机网络链路信息（供屏幕显示 STA IP / RSSI / 信道）*/
typedef struct {
    bool    sta_connected;
    char    ip[16];       /* STA IP，未连接时为空串 */
    int16_t rssi;         /* dBm，未连接时为 0 */
    uint8_t channel;      /* 信道，未连接时为 0 */
} net_link_info_t;

void net_get_link_info(net_link_info_t *out);

/* 在线切换 AP / STA 开关（只改开关，不改账号密码；立即生效不需重启）。
 * 两个都关会彻底失联，此时返回 ESP_ERR_INVALID_ARG。 */
esp_err_t net_wifi_set_enabled(bool ap_en, bool sta_en);

#ifdef __cplusplus
}
#endif
