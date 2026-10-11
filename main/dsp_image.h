#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DSP 程序映像：录制 / 保存 / 回放
 *
 * 背景（结合 mzulus 的 ESP32_ADAU1452_RAM_Loader 思路）：
 *   1452 正常情况下靠外挂自举 EEPROM 启动。这里让 ESP32 在 TCP 桥接时"顺手"
 *   把 SigmaStudio 下载的全部写入录下来，存进 flash 的自定义分区（dspimg）。
 *   以后上电时由 ESP32 直接回放这份映像，就能**不需要 EEPROM** 启动 DSP，
 *   而且换程序不用重新编译 ESP32 固件 —— 在 SigmaStudio 里改完点一下"保存映像"即可。
 *
 * 录制数据结构：
 *   - 寄存器（>= 0xF000）：末次生效的稀疏数组，8 KB
 *   - DSP 内存（< 0xF000）：按字（4 字节）寻址的区段表，可动态扩容/合并
 * 回放顺序（复刻 SigmaStudio default_download 的已知安全序列）：
 *   软复位 -> 休眠/停核 -> 其它寄存器 -> 内存(PROGRAM/PARAM/DM) -> 启动核心 -> 唤醒
 */

/* dspimg 分区 1MB = 4 个 256KB 槽位，每个槽位放一份**独立**的命名映像 */
#define DSP_IMAGE_SLOT_COUNT   4
/* 名称缓冲（含结尾 NUL，最多 31 字节 UTF-8，中文约 10 个字） */
#define DSP_IMAGE_NAME_MAX     32

/* 单个槽位的概览 */
typedef struct {
    int      index;                       /* 0..DSP_IMAGE_SLOT_COUNT-1 */
    bool     valid;                       /* 是否有有效映像（槽头 + CRC 均通过） */
    uint32_t seq;                         /* 保存序号（单调递增） */
    uint32_t bytes;                       /* payload 字节数 */
    char     name[DSP_IMAGE_NAME_MAX];    /* 用户起的名字，可能为空串 */
} dsp_image_slot_t;

/* 上电自动加载的执行状态（网页「本次开机」一行用）
 *
 * ⚠️ 这个功能**不是一次性**的：开启后会持续监控 1452。
 *    1452 每次上线（含断电重启后重新上电）都会自动推送一次固件。 */
typedef enum {
    DSP_BOOT_IDLE = 0,   /* 没启用 / 没选固件 / 没选中的槽位为空 */
    DSP_BOOT_WAIT,       /* 等 1452 上线（掉线后也回到这个状态） */
    DSP_BOOT_SETTLE,     /* 1452 刚上线，等 settle_ms 再推（给供电/时钟一点稳定时间） */
    DSP_BOOT_OK,         /* 已成功推送给 1452 */
    DSP_BOOT_FAIL,       /* 推送失败或发现程序没在跑，下一轮会重推 */
} dsp_boot_state_t;

typedef struct {
    dsp_boot_state_t state;
    int      slot;        /* 目标槽位，-1 = 未选择 */
    uint32_t uptime_ms;   /* 结果产生时刻（开机毫秒） */
    uint32_t attempts;    /* 等待/回放已经尝试的次数 */
    char     msg[80];     /* 中文说明，网页直接显示 */
} dsp_image_boot_result_t;

/* 初始化：定位 dspimg 分区、扫描全部槽位、载入命名与开机推送配置 */
esp_err_t dsp_image_init(void);

/* ---------- 录制开关 ----------
 * 默认关闭：只有从网页点了「开始录制」之后，桥接转发进来的写入才会被录制。
 * 开始录制时会顺带清空上一次的录制内容。 */
void dsp_image_record_start(void);   /* 清空 + 开始录制 */
void dsp_image_record_stop(void);    /* 停止录制（已录内容保留） */
bool dsp_image_recording(void);

/* 录制一次写入（由 TCP 桥接层在转发前调用）。
 * 未处于录制状态时直接忽略。 */
void dsp_image_record(uint16_t addr, const uint8_t *data, size_t len);

/* ---------- RAM 录制状态 ---------- */
bool   dsp_image_ram_valid(void);      /* RAM 中是否有任何录制内容 */
bool   dsp_image_ram_has_program(void);/* 是否录到了程序 RAM（>= 0xC000） */
uint32_t dsp_image_ram_regs(void);     /* 已录寄存器个数 */
uint32_t dsp_image_ram_exts(void);     /* 已录内存区段个数 */
uint32_t dsp_image_ram_bytes(void);    /* 已录数据字节数（不含管理开销） */
void   dsp_image_ram_clear(void);

/* ---------- flash 槽位（每份录制固件） ---------- */
int       dsp_image_slot_count(void);
/* 读取某个槽位的概览（含用户起的名字），槽位号非法返回 INVALID_ARG */
esp_err_t dsp_image_slot_get(int idx, dsp_image_slot_t *out);
/* 重命名（空串 = 清除名字）；名字只影响网页显示 */
esp_err_t dsp_image_slot_rename(int idx, const char *name);
/* 只擦除某个槽位（该槽位的那一份固件） */
esp_err_t dsp_image_slot_erase(int idx);
/* 把某个槽位的映像回放到 DSP */
esp_err_t dsp_image_slot_replay(int idx);

/* 是否还有任何有效槽位；seq/bytes 指最新（序号最大）的那一份 */
bool     dsp_image_flash_valid(void);
uint32_t dsp_image_flash_seq(void);
uint32_t dsp_image_flash_bytes(void);

/* 把当前 RAM 录制保存到 flash（自动选最旧槽位轮换 + CRC32 校验，掉电安全）。
 * slot_out 非空时回填实际写入的槽位号。 */
esp_err_t dsp_image_save_to_flash(int *slot_out);

/* 擦除 flash 中保存的**全部**映像 */
esp_err_t dsp_image_flash_erase(void);

/* 把 flash 映像回放到 DSP：用「开机推送」选中的槽位，未选择时退回最新的一份 */
esp_err_t dsp_image_replay(void);

/* ---------- 上电自动加载（配置与执行都在本模块内） ---------- */
bool      dsp_image_auto_load(void);          /* 是否启用上电自动加载 */
esp_err_t dsp_image_set_auto_load(bool on);
int       dsp_image_boot_slot(void);          /* 开机推送到 1452 的槽位，-1 = 不推送 */
esp_err_t dsp_image_set_boot_slot(int idx);   /* idx = -1 表示「不推送」 */

/* 探测到 1452 上线后等多久才推送（毫秒，网页可调，掉电保留）。
 * selfboot 关闭时通常不需要等太久，默认 500ms；0 表示一探测到就推。
 * （推送失败会自动在当前值上递增重试，成功后回到这个配置值。） */
uint32_t  dsp_image_settle_ms(void);
esp_err_t dsp_image_set_settle_ms(uint32_t ms);

/* 开机开始时调用一次：决定这次要不要监控 1452（立即试一轮） */
void dsp_image_boot_begin(void);
/* 主循环周期调用（**要一直调**，不只是开机那一会儿）。会做三件事：
 *   ① 等 1452 上线（离线时每 250ms 探一次）→ 稳 settle_ms → 推送；
 *   ② 掉线（连续几次探测失败）→ 回到等待，重新上线后再推；
 *   ③ 在线时巡检：发现程序没在跑（见证寄存器被改 / PLL 未锁 / 核心已停）
 *      → 自动重推（每开机限次）。
 * allow=false 时暂停（例如 SigmaStudio 正在调试，不能抢总线）。 */
void dsp_image_boot_tick(bool allow);
/* 是否仍在等待/重试回放（主循环用它推迟音量同步，避免写到旧程序的地址） */
bool dsp_image_autoload_pending(void);
void dsp_image_boot_result(dsp_image_boot_result_t *out);
/* 累计成功推送次数。
 * ⚠️ 主循环必须盯着这个值：**每次推送都会把 DSP 的参数恢复成录制时的初值**
 *    （映像里带的就是那些值），所以推送后必须把本地保存的音量重放一遍，
 *    否则音量会"跳回"录制时的值。 */
uint32_t dsp_image_push_count(void);
/* 是否正在回放（回放期间本地控制器必须停止访问 I2C） */
bool dsp_image_busy(void);


/* 流式导出映像：逐块回调，调用者无需一次性申请大块内存
 * （内部堆碎片严重时 free 够但最大连续块不够，故一律流式处理）。
 * 回调返回非 ESP_OK 会中止导出。 */
typedef esp_err_t (*dsp_image_sink_t)(const uint8_t *data, size_t len, void *ctx);
esp_err_t dsp_image_stream(dsp_image_sink_t sink, void *ctx);

#ifdef __cplusplus
}
#endif
