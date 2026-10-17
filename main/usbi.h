#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ================= USBi（USB 直连 SigmaStudio）=================
 *
 * 把 ESP32-S3 的原生 USB 口（GPIO19/20，板子上标 "USB" 的 Type-C）枚举成
 * Analog Devices 的 USBi 调试器（EVAL-ADUSB2EBZ，VID 0x0456 / PID 0x7031）。
 *
 * 这样 PC 上的 SigmaStudio 在 Hardware Configuration 里选 USBi 就能直接找到
 * ESP32，用 "Link Compile Download" 下载/在线调参，完全不需要 WiFi + TCP 桥接。
 *
 * 协议（从 SigmaStudio 4.7 的 ADIUSBi.dll 反推，控制端点 vendor 请求）：
 *   0xB2 CMD_WRITE         OUT  data = [addrH addrL payload...]   写 DSP
 *   0xB3 CMD_WRITE_READ    OUT  data = [addrH addrL]              取读地址
 *   0xB4 CMD_READ          IN   data = payload                    读 DSP
 *   0xB5 CMD_CHECK_WRITE   IN   1 字节状态（1=成功）              写完成查询
 *   0xB6 CMD_CHECK_READ    IN   1 字节状态（1=成功）              读完成查询
 *   0xB7 CMD_RESET         OUT  无数据                            复位（忽略）
 *   0xB8 CMD_POWERBOARD    OUT  无数据，wValue=0/1                目标板 5V 开关（忽略）
 *   0xB9 CMD_LED           OUT  无数据，wValue=0/1                指示灯（忽略）
 *   0xBA CMD_RESET_TOGGLE  OUT  无数据，wValue=0/1                复位翻转（忽略）
 *
 *   wValue = chipAddress >> 1（I2C 7 位地址，ADAU1452 固定 0x3B）
 *   wIndex = (clatch << 8) | format（format: 0=I2C, 1=SPI）
 *
 * 数据语义与 TCP 桥接完全一致：2 字节大端子地址 + 数据（<0xF000 按字寻址），
 * 所以直接复用 adau1452_write_block()/adau1452_read_block()。
 *
 * ⚠️ 模式互斥：USBi 与 TCP 桥接都会独占 DSP，两者不能同时开；
 *    开启 USBi 时本地控制（编码器/网页音量/电平）与上电自动推 DSP 程序
 *    都必须让位，行为与「TCP 桥接开关」一致。
 */

typedef struct {
    /* --- 开关 / USB 状态 --- */
    bool     enabled;        /* 网页开关（掉电保存） */
    bool     installed;      /* TinyUSB 是否已安装 */
    bool     mounted;        /* USB 已连接（主机已复位总线） */
    bool     configured;     /* 主机已完成配置（Windows 驱动已接管） */
    uint32_t attach_count;   /* 累计被主机配置的次数（插拔计数） */

    /* --- 转发统计 --- */
    uint32_t pkt_write;      /* 0xB2 写命令数 */
    uint32_t pkt_read;       /* 0xB4 读命令数 */
    uint32_t cmd_other;      /* 复位/电源/LED 等命令数 */
    uint32_t errors;         /* I2C 失败 / 协议异常次数 */
    uint16_t last_addr;      /* 最近访问的 DSP 地址 */
    uint32_t last_len;       /* 最近一条的长度（字节） */
    bool     last_is_read;   /* 最近一条是读还是写 */
    uint8_t  last_format;    /* USBi 协议的 format（0=I2C, 1=SPI） */
    uint16_t last_value;     /* USBi 协议的 wValue */
    uint16_t last_index;     /* USBi 协议的 wIndex */
    uint32_t last_us;        /* 最近一条命令的处理耗时（微秒） */
    uint32_t max_us;         /* 最大处理耗时（微秒） */
    uint8_t  write_status;   /* 0xB5 返回的状态（1=成功，3=失败） */
    uint8_t  read_status;    /* 0xB6 返回的状态（1=成功，3=失败） */
} usbi_status_t;

/* 从 NVS 读开关；若上次是开着的，直接安装 USB。app_main 里调用一次。 */
esp_err_t usbi_init(void);

/* 网页开关：安装/卸载 USB 设备并把开关存进 NVS。
 * ⚠️ 调用方（net.c）负责先关掉 TCP 桥接（两者互斥）。 */
esp_err_t usbi_set_enabled(bool on);

/* 当前开关状态（掉电保存的那个） */
bool usbi_enabled(void);

/* 取一份状态快照（网页/屏幕显示用） */
void usbi_get_status(usbi_status_t *out);

/* 主机是否已经接管（已启用且已配置）：为 true 时本地控制必须让位 */
bool usbi_busy(void);

#ifdef __cplusplus
}
#endif
