#include "adau1452.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define I2C_PORT         0
#define I2C_SDA_PIN      GPIO_NUM_19
#define I2C_SCL_PIN      GPIO_NUM_20
#define I2C_FREQ_HZ      400000

/* 程序 RAM 写入时序：必须 4 字节一次 + 间隔，否则 I2C 有 ACK 但指令没进 DSP（无声） */
#define PROG_WORD_DELAY_US   300
#define I2C_IO_TIMEOUT_MS    100
#define CHUNK_RETRY          3

static const char *TAG = "adau1452";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
/* 递归互斥：保证块写入不会被单参数读写从中间插断 */
static SemaphoreHandle_t s_mtx = NULL;

/* 当前生效的 SCL 频率与内部弱上拉状态（供诊断接口查询/修改） */
static uint32_t s_i2c_hz = I2C_FREQ_HZ;
static bool     s_i2c_pullup = true;

/* 上一次 adau1452_probe() 是否成功：只用于"在线状态变化"时打一条日志 */
static bool     s_probe_ok = false;

/* 按指定频率挂上 ADAU1452 设备（ESP-IDF v5 的频率是设备级参数） */
static esp_err_t dev_attach(uint32_t hz)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADAU1452_I2C_ADDR_7BIT,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

static inline void lock_bus(void)
{
    if (s_mtx) {
        xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    }
}

static inline void unlock_bus(void)
{
    if (s_mtx) {
        xSemaphoreGiveRecursive(s_mtx);
    }
}

esp_err_t adau1452_init(void)
{
    esp_err_t ret;

    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateRecursiveMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = dev_attach(I2C_FREQ_HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return adau1452_probe();
}

esp_err_t adau1452_probe(void)
{
    uint32_t probe = 0;
    const esp_err_t ret = adau1452_read_param(ADAU_ADDR_MASTER_TARGET, &probe);
    /*
     * ⚠️ 这个函数会被「上电自动加载」的巡检每 1.5 秒调一次，
     * 每次都打 INFO 会刷爆日志（也白占串口带宽）。
     * 所以只在**在线状态发生变化**时打 —— 这恰好也是最有用的一条信息
     * （1452 上线/掉线的时刻，正是排查"有没有推送成功"的关键线索）。
     */
    if (ret == ESP_OK) {
        if (!s_probe_ok) {
            s_probe_ok = true;
            ESP_LOGI(TAG, "ADAU1452 detected, master target raw = 0x%08lX",
                     (unsigned long)probe);
        }
    } else if (s_probe_ok) {
        s_probe_ok = false;
        ESP_LOGW(TAG, "ADAU1452 无响应（%s）", esp_err_to_name(ret));
    }
    return ret;
}

/* ---------- 原始 I2C 事务 ---------- */

esp_err_t adau1452_raw_write(uint16_t subaddr, const uint8_t *data, size_t len)
{
    if (s_dev == NULL || data == NULL || len == 0 || len > 250) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[2 + 250];
    buf[0] = (uint8_t)((subaddr >> 8) & 0xFF);
    buf[1] = (uint8_t)(subaddr & 0xFF);
    memcpy(buf + 2, data, len);

    return i2c_master_transmit(s_dev, buf, len + 2, I2C_IO_TIMEOUT_MS);
}

esp_err_t adau1452_raw_read(uint16_t subaddr, uint8_t *data, size_t len)
{
    if (s_dev == NULL || data == NULL || len == 0 || len > 250) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t wbuf[2];
    wbuf[0] = (uint8_t)((subaddr >> 8) & 0xFF);
    wbuf[1] = (uint8_t)(subaddr & 0xFF);

    return i2c_master_transmit_receive(s_dev, wbuf, sizeof(wbuf), data, len,
                                       I2C_IO_TIMEOUT_MS);
}

esp_err_t adau1452_write_param(uint16_t addr, uint32_t value)
{
    uint8_t d[4] = {
        (uint8_t)((value >> 24) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
    };
    lock_bus();
    esp_err_t ret = adau1452_raw_write(addr, d, sizeof(d));
    unlock_bus();
    return ret;
}

esp_err_t adau1452_read_param(uint16_t addr, uint32_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rbuf[4] = {0};
    lock_bus();
    esp_err_t ret = adau1452_raw_read(addr, rbuf, sizeof(rbuf));
    unlock_bus();
    if (ret != ESP_OK) {
        return ret;
    }

    *value = ((uint32_t)rbuf[0] << 24) |
             ((uint32_t)rbuf[1] << 16) |
             ((uint32_t)rbuf[2] << 8) |
             ((uint32_t)rbuf[3]);

    return ESP_OK;
}

/* ---------- 按 SigmaStudio 地址语义的块读写 ---------- */

esp_err_t adau1452_write_block(uint16_t addr, const uint8_t *data, size_t len)
{
    if (s_dev == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool is_reg  = (addr >= ADAU1452_ADDR_REG_BASE);
    const bool is_prog = (!is_reg && addr >= ADAU1452_ADDR_PROG_BASE);

    if (!is_reg && (len % 4) != 0) {
        ESP_LOGW(TAG, "mem block len=%u 不是 4 的倍数 (addr=0x%04X)", (unsigned)len, addr);
        return ESP_ERR_INVALID_SIZE;
    }

    lock_bus();
    esp_err_t result = ESP_OK;
    size_t off = 0;

    while (off < len) {
        size_t chunk;
        uint16_t sub;

        if (is_reg) {
            /* 寄存器：字节寻址，小块写 */
            chunk = len - off;
            if (chunk > 24) chunk = 24;
            sub = (uint16_t)(addr + off);
        } else if (is_prog) {
            /* 程序 RAM：一次一个 32 位字 + 间隔 */
            chunk = 4;
            sub = (uint16_t)(addr + off / 4);
        } else {
            /* 参数 / DM 内存：按字对齐的较大块 */
            chunk = len - off;
            if (chunk > 28) chunk = 28;
            chunk = (chunk / 4) * 4;
            if (chunk == 0) chunk = 4;
            sub = (uint16_t)(addr + off / 4);
        }

        esp_err_t ret = ESP_FAIL;
        for (int attempt = 0; attempt < CHUNK_RETRY; attempt++) {
            ret = adau1452_raw_write(sub, data + off, chunk);
            if (ret == ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "block write 失败 @0x%04X (sub=0x%04X, %u 字节): %s",
                     addr + (unsigned)(is_reg ? off : off / 4), sub,
                     (unsigned)chunk, esp_err_to_name(ret));
            result = ret;
            break;
        }

        off += chunk;
        if (is_prog) {
            esp_rom_delay_us(PROG_WORD_DELAY_US);
        } else {
            taskYIELD();
        }
    }

    unlock_bus();
    return result;
}

esp_err_t adau1452_read_block(uint16_t addr, uint8_t *data, size_t len)
{
    if (s_dev == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool is_reg = (addr >= ADAU1452_ADDR_REG_BASE);

    /*
     * 块大小：读比写宽松得多（写受 DSP 内部时序限制，读没有）。
     * 之前只读 24 字节/次，一次 14480 字节的整块读要 600 多次事务、约 0.5 秒，
     * 容易让 SigmaStudio 等超时。这里内存区放宽到 96 字节（仍是 4 的倍数），
     * 事务数减少 4 倍，显著降低超时风险。
     */
    const size_t max_chunk = is_reg ? 24 : 96;

    lock_bus();
    esp_err_t result = ESP_OK;
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        uint16_t sub;

        if (chunk > max_chunk) {
            chunk = max_chunk;
        }
        if (is_reg) {
            sub = (uint16_t)(addr + off);
        } else {
            /* 内存按字寻址：块大小必须是 4 的倍数；尾部不足一字时按字节读完 */
            if (chunk >= 4) {
                chunk = (chunk / 4) * 4;
            }
            sub = (uint16_t)(addr + off / 4);
        }

        esp_err_t ret = adau1452_raw_read(sub, data + off, chunk);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "block read 失败 @0x%04X: %s",
                     (unsigned)sub, esp_err_to_name(ret));
            result = ret;
            break;
        }
        off += chunk;
    }

    unlock_bus();
    return result;
}

esp_err_t adau1452_write_reg16(uint16_t addr, uint16_t value)
{
    uint8_t d[2] = { (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    lock_bus();
    esp_err_t ret = adau1452_raw_write(addr, d, sizeof(d));
    unlock_bus();
    return ret;
}

esp_err_t adau1452_soft_reset(void)
{
    /* 复刻 SigmaStudio 的复位序列：先写 0 再写 1 触发复位脉冲 */
    esp_err_t ret = adau1452_write_reg16(ADAU1452_REG_SOFT_RESET, 0x0000);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = adau1452_write_reg16(ADAU1452_REG_SOFT_RESET, 0x0001);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(255));   /* 与导出文件的 R2_RESET_DELAY 一致 */
    }
    return ret;
}

esp_err_t adau1452_set_volume_db(uint16_t addr, float db)
{
    /* 8.24 定点：gain = 10^(db/20)，raw = round(gain * 2^24) */
    float gain = powf(10.0f, db / 20.0f);
    int32_t raw = (int32_t)lroundf(gain * 16777216.0f);
    return adau1452_write_param(addr, (uint32_t)raw);
}

float adau1452_read_volume_db(uint16_t addr)
{
    uint32_t raw = 0;
    if (adau1452_read_param(addr, &raw) != ESP_OK) {
        return -999.0f;
    }

    int32_t s = (int32_t)raw;
    float gain = (float)s / 16777216.0f;
    if (gain <= 0.0f) {
        return -120.0f;
    }
    return 20.0f * log10f(gain);
}

float adau1452_read_level_db(uint16_t addr)
{
    uint32_t raw = 0;
    if (adau1452_read_param(addr, &raw) != ESP_OK) {
        return -999.0f;
    }

    /* Single Band Level Detector 的 X 输出实际为 Q15 格式
     * （满刻度 1.0 = 32768，存放在 32 位参数的低 16 位）。
     * 实测：满刻度信号经 -6dB 总音量后 X≈0x4000≈0.5，对应约 -7.5dB。 */
    uint16_t q15 = (uint16_t)(raw & 0xFFFF);
    float x = (float)q15 / 32768.0f;
    if (x <= 1e-7f) {
        return -96.0f;
    }
    float db = 20.0f * log10f(x);
    if (db < -96.0f) {
        db = -96.0f;
    }
    if (db > 0.0f) {
        db = 0.0f;
    }
    return db;
}

/* ================= I2C 调优（频率 / 内部上拉） ================= */

uint32_t adau1452_i2c_speed(void)
{
    return s_i2c_hz;
}

bool adau1452_i2c_internal_pullup(void)
{
    return s_i2c_pullup;
}

esp_err_t adau1452_i2c_set_speed(uint32_t hz)
{
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hz < 10000 || hz > 1000000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hz == s_i2c_hz && s_dev != NULL) {
        return ESP_OK;
    }

    /*
     * ESP-IDF v5 的 i2c_master 把 SCL 频率放在**设备**配置里（不是总线配置），
     * 所以换频率只能移除设备句柄、再按新频率重新挂上。
     */
    lock_bus();
    const uint32_t old = s_i2c_hz;
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    esp_err_t ret = dev_attach(hz);
    if (ret != ESP_OK) {
        /* 回滚：绝不能让总线上没有设备句柄（否则后续所有读写都会失败） */
        ESP_LOGE(TAG, "改 I2C 时钟到 %lu Hz 失败：%s，回滚到 %lu Hz",
                 (unsigned long)hz, esp_err_to_name(ret), (unsigned long)old);
        if (dev_attach(old) == ESP_OK) {
            s_i2c_hz = old;
        }
    } else {
        s_i2c_hz = hz;
        ESP_LOGW(TAG, "I2C 时钟已设为 %lu Hz", (unsigned long)hz);
    }
    unlock_bus();
    return ret;
}

esp_err_t adau1452_i2c_set_internal_pullup(bool on)
{
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const gpio_pull_mode_t mode = on ? GPIO_PULLUP_ONLY : GPIO_FLOATING;
    const esp_err_t e1 = gpio_set_pull_mode(I2C_SDA_PIN, mode);
    const esp_err_t e2 = gpio_set_pull_mode(I2C_SCL_PIN, mode);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        return (e1 != ESP_OK) ? e1 : e2;
    }
    s_i2c_pullup = on;
    ESP_LOGW(TAG, "I2C 内部弱上拉：%s", on ? "开（约 45kΩ，400kHz 下偏弱）" : "关（依赖外部上拉）");
    return ESP_OK;
}
