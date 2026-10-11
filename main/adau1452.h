#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ADAU1452 I2C 地址（7 位 0x3B，写 0x76 / 读 0x77） */
#define ADAU1452_I2C_ADDR_7BIT   0x3B

/*
 * 音量参数地址（来自 SigmaStudio 导出的 _PARAM.h）
 * 工程文件说明.txt：
 *   Single 1   -> 总音量 (MOD_SINGLE1_ALG0_TARGET_ADDR)
 *   Single 1_7 -> 低音音量 (MOD_SINGLE1_7_ALG0_TARGET_ADDR)
 *   Single 1_5 -> 高音音量 (MOD_SINGLE1_5_ALG0_TARGET_ADDR)
 *   Single 1_2 -> 中音音量 (MOD_SINGLE1_2_ALG0_TARGET_ADDR)
 */
#define ADAU_ADDR_MASTER_TARGET   29     /* 总音量 */
#define ADAU_ADDR_BASS_TARGET     226    /* 低音 */
#define ADAU_ADDR_TREBLE_TARGET   164    /* 高音 */
#define ADAU_ADDR_MID_TARGET      161    /* 中音 */

/*
 * 电平显示回读地址（Single Band Level Detector 的线性值 X，8.24 格式）
 * Level1   -> 左声道 (MOD_LEVEL1_ALG0_SINGLEBANDLEVELDETS3001X_ADDR)
 * Level1_2 -> 右声道 (MOD_LEVEL1_2_ALG0_SINGLEBANDLEVELDETS3002X_ADDR)
 */
#define ADAU_ADDR_LEVEL_L         52     /* 左声道线性电平 */
#define ADAU_ADDR_LEVEL_R         66     /* 右声道线性电平 */

/* 初始化 I2C 总线并探测设备 */
esp_err_t adau1452_init(void);

/* 探测设备是否在线（读总音量参数）；失败时静默，用于上电后重试等待 DSP 就绪 */
esp_err_t adau1452_probe(void);

/* 写一个 32 位参数（地址 + 4 字节大端数据） */
esp_err_t adau1452_write_param(uint16_t addr, uint32_t value);

/* 读一个 32 位参数 */
esp_err_t adau1452_read_param(uint16_t addr, uint32_t *value);

/* 按 dB 设置音量（内部转换为 8.24 线性增益写入） */
esp_err_t adau1452_set_volume_db(uint16_t addr, float db);

/* 读取某音量参数的当前 dB 值；失败返回 -999.0f */
float adau1452_read_volume_db(uint16_t addr);

/* 读取电平（线性 X -> dB），失败返回 -999.0f */
float adau1452_read_level_db(uint16_t addr);

/* ================= SigmaStudio 地址语义（供 TCP 桥接使用） =================
 * ADAU1452 的 I2C 从口地址空间有两种语义（mzulus / AD sigma-tcp 实测结论）：
 *   - 地址 >= 0xF000：核心寄存器，**按字节**寻址；子地址 = addr + 字节偏移
 *   - 地址 <  0xF000：DSP 内存，  **按字（1 字 = 4 字节）**寻址；
 *                     子地址 = addr + 字节偏移 / 4
 * 导出文件印证：PARAM_ADDR=0x0000、DM1_DATA_ADDR=0x6000、PROGRAM_ADDR=0xC000、
 * 寄存器全部在 0xF0xx。
 */
#define ADAU1452_ADDR_REG_BASE    0xF000
#define ADAU1452_ADDR_PROG_BASE   0xC000   /* >= 0xC000 为程序 RAM，写入最挑时序 */

/* 核心控制寄存器（ADAU1452 固定地址，来自导出的 _REG.h） */
#define ADAU1452_REG_PLL_CTRL0     0xF000
#define ADAU1452_REG_PLL_CTRL1     0xF001
#define ADAU1452_REG_PLL_CLK_SRC   0xF002
#define ADAU1452_REG_PLL_ENABLE    0xF003
#define ADAU1452_REG_PLL_LOCK      0xF004  /* 只读：1 = PLL 已锁定 */
#define ADAU1452_REG_MCLK_OUT      0xF005
#define ADAU1452_REG_CLK_GEN3_M    0xF024
#define ADAU1452_REG_CLK_GEN3_LOCK 0xF027  /* 只读：1 = 音频时钟发生器已锁定 */
#define ADAU1452_REG_POWER_ENABLE0 0xF050
#define ADAU1452_REG_POWER_ENABLE1 0xF051
#define ADAU1452_REG_MP7_MODE      0xF517
#define ADAU1452_REG_MP11_MODE     0xF51B
#define ADAU1452_REG_HIBERNATE     0xF400
#define ADAU1452_REG_START_PULSE   0xF401
#define ADAU1452_REG_START_CORE    0xF402
#define ADAU1452_REG_KILL_CORE     0xF403
#define ADAU1452_REG_START_ADDRESS 0xF404
#define ADAU1452_REG_CORE_STATUS   0xF405  /* 只读：0 = 核心已停；非 0 = 运行中
                                            * ⚠️ 实测会在 1 与 3 之间跳动，
                                            *    只能判"是否等于 0"，不能比固定值 */
#define ADAU1452_REG_SOFT_RESET    0xF890

/* 音频串口配置区（SOUT_SOURCE0..23 + SERIAL_BYTE_1..7），按字节寻址 */
#define ADAU1452_REG_AUDIO_BASE    0xF180
#define ADAU1452_REG_AUDIO_END     0xF2FF

/* 原始 I2C 事务（不做地址翻译），data/len 对应芯片端字节流 */
esp_err_t adau1452_raw_write(uint16_t subaddr, const uint8_t *data, size_t len);
esp_err_t adau1452_raw_read(uint16_t subaddr, uint8_t *data, size_t len);

/* 按 SigmaStudio 地址语义做块写入/读出（自动分块，保证时序可靠） */
esp_err_t adau1452_write_block(uint16_t addr, const uint8_t *data, size_t len);
esp_err_t adau1452_read_block(uint16_t addr, uint8_t *data, size_t len);

/* 写 16 位核心寄存器 */
esp_err_t adau1452_write_reg16(uint16_t addr, uint16_t value);

/* 软复位 DSP（写 SOFT_RESET 触发复位脉冲；复位后需要用映像重新加载程序） */
esp_err_t adau1452_soft_reset(void);

/* ================= I2C 调优 =================
 *
 * 频率与上拉是硬件级参数，做成运行时可调、并掉电保留 ——
 * 主要用途是**加快固件下载**（PROGRAM 写时序是纯 I2C 线速限制，
 * 提到 600kHz/1MHz 实测 0 错误，下载时间可从 0.77s 降到约 0.5s）。
 *
 * ⚠️ ADAU1452 官方上限是 400kHz（当前默认）。超频属越规操作，
 * 提高后务必重新下载并回读校验。
 */

/* 当前 SCL 频率（Hz） */
uint32_t adau1452_i2c_speed(void);

/* 运行时改 SCL 频率（Hz，范围 10000~1000000）。失败会回滚到原频率。 */
esp_err_t adau1452_i2c_set_speed(uint32_t hz);

/* 开关**内部弱上拉**（约 45kΩ）。
 * ⚠️ ESP32 内部上拉对 I2C 来说偏弱：400kHz 快速模式下通常需要 2~5kΩ 外部上拉。
 * 关掉它可用来判断"板子到底有没有外部上拉"：
 *   关了以后时序基本不变 → 板上有外部上拉（内部这个可以不用）
 *   关了以后变慢/出错     → 板子靠内部弱上拉，这就是慢的根因，需要补外部电阻 */
esp_err_t adau1452_i2c_set_internal_pullup(bool on);
bool adau1452_i2c_internal_pullup(void);

#ifdef __cplusplus
}
#endif
