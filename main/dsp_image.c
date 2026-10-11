#include "dsp_image.h"
#include "adau1452.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/*
 * DSP 程序映像：录制 / 存 flash / 回放 / 上电自动推送
 *
 * 本文件较长（约 1800 行），章节顺序如下，方便定位：
 *   ① 常量                       —— 槽位布局、时序参数、自动加载的各种间隔
 *   ② RAM 录制状态 / 录制         —— 桥接转发时"顺手"记下 SigmaStudio 写了什么
 *   ③ flash 槽位状态 / 管理       —— 4 个 256KB 槽位，各自带 CRC32 与用户命名
 *   ④ 配置持久化（NVS "dspimg"） —— 名字 / 上电自动加载开关 / 开机推送槽位 / 就绪等待
 *   ⑤ 内存写入校验               —— verify_mem_written()：判"推送真的写进 RAM 了吗"
 *   ⑥ 回放（⚠️ 顺序即正确性）     —— dimg_replay_slot()：复刻 default_download
 *   ⑦ 上电自动加载：持续监控 1452  —— 等上线 → 推送 → 健康巡检/自动重推
 *
 * 两个最容易看错的地方，先写在前面：
 *   - **回放的寄存器顺序绝对不能改**（详见"回放顺序"一节）：冷启动时若在 PLL
 *     锁定前就写音频串口寄存器，它们不生效 → I2C 一切正常但完全不出声。
 *   - **"推送成功"必须靠内存校验 + PLL/CORE 状态**，不能只回读一个刚写过的
 *     寄存器（那种值当然对得上；实测有些寄存器还会静默丢弃非法写入）。
 */

static const char *TAG = "dsp_image";

/* ================= 常量 ================= */
#define DIMG_REG_BASE      0xF000
#define DIMG_REG_COUNT     0x1000                 /* 0xF000..0xFFFF 共 4096 个 16 位寄存器 */
#define DIMG_MAX_EXT       24
#define DIMG_MAX_BYTES     (512 * 1024)           /* 录制上限，防止堆被吃光 */

#define DIMG_PART_LABEL    "dspimg"
#define DIMG_SLOT_SIZE     0x40000u               /* 256 KB，4 槽 = 1 MB */
#define DIMG_SLOT_COUNT    DSP_IMAGE_SLOT_COUNT
#define DIMG_HDR_SIZE      64
#define DIMG_MAGIC         0x31495344u            /* "DSI1" */

#define DIMG_FLASH_CHUNK   256                    /* 回放时从 flash 读的块大小 */

/* ---- 配置持久化（NVS namespace "dspimg"）：名称 / 上电自动加载 / 开机推送槽位 ---- */
#define DIMG_NS            "dspimg"
#define DIMG_NAME_MAX      DSP_IMAGE_NAME_MAX

/* 等待 1452 就绪（探测到上线后到开始推送之间的间隔）：
 *
 * ⚠️ adau1452_probe() 只是读一个参数，I2C 能 ACK 就返回成功。历史上需要等一段时间
 * 是因为 1452 会在**自己从 EEPROM 自举**的过程中就 ACK，推早了会被随后的自举覆盖。
 *
 * 但自从「selfboot 关闭」后（我们的推荐接法）1452 不再自举，这个理由就不存在了；
 * 而且即使推早了，现在也有**内存校验 + PLL_LOCK/CORE_STATUS 检查 + 自动重推**兜底
 * （`dsp_image_boot_tick` 的健康巡检），能自动发现并重新推送。
 *
 * 所以默认值取得很小（500ms），并且**可以在网页上调**（0 ~ DIMG_SETTLE_LIMIT_MS）。
 * 推送失败时会在当前值基础上自动加长重试，逐步逼近这台硬件真正需要的时长。
 *
 * ⚠️ 注意：不等于"完全没有等待"。1452 刚上电时供电与外部时钟需要一点稳定时间，
 *    留一个小的余量可以避免"第一次推送必然失败"的浪费。
 */
#define DIMG_SETTLE_DEFAULT_MS 500
#define DIMG_SETTLE_LIMIT_MS   5000      /* 网页可设上限，也是自适应增长的封顶 */
#define DIMG_SETTLE_STEP_MS    500       /* 推送失败后每次加长多少 */
/* 等待上线的探测间隔（只读探测，代价极低；250ms 让"1452 一上电就尽快发现"） */
#define DIMG_PROBE_MS      250
/* 已在线时的巡检间隔（长一些，减少 I2C 占用） */
#define DIMG_ONLINE_MS     1500
/* 连续几次探测失败才判定"1452 掉线"（防抖动） */
#define DIMG_OFF_COUNT     3
/* 连续几次发现程序被改写才重新推送（防误判） */
#define DIMG_BAD_COUNT     2
/* 每次开机最多自动重推几次（防止“推→程序挂→再推”的死循环刷屏/扰民） */
#define DIMG_REPUSH_MAX    30
/* 内存校验每个可读区段取多少个字比对（64 字 = 256 字节，足够区分“整段没写进去”） */
#define DIMG_VERIFY_WORDS  64

/* ================= RAM 录制状态 ================= */
typedef struct {
    uint16_t base;         /* 字地址 */
    uint32_t words;        /* 字数（每字 4 字节） */
    uint8_t *d;            /* words*4 字节，字内保持大端（与 I2C 线上一致） */
} dimg_ext_t;

static uint16_t s_reg[DIMG_REG_COUNT];
static uint8_t  s_reg_valid[DIMG_REG_COUNT / 8];
static dimg_ext_t s_ext[DIMG_MAX_EXT];
static int      s_ext_count;
static uint32_t s_reg_count;
static uint32_t s_ram_bytes;

static SemaphoreHandle_t s_mtx;
static const esp_partition_t *s_part;
static volatile bool s_recording;    /* 是否正在录制（网页「开始录制」后为真） */

void dsp_image_record_start(void)
{
    dsp_image_ram_clear();
    s_recording = true;
    ESP_LOGI(TAG, "开始录制（等待 SigmaStudio 下载程序）");
}

void dsp_image_record_stop(void)
{
    s_recording = false;
    ESP_LOGI(TAG, "停止录制：寄存器 %lu 个，区段 %d 个，%lu 字节",
             (unsigned long)s_reg_count, s_ext_count, (unsigned long)s_ram_bytes);
}

bool dsp_image_recording(void)
{
    return s_recording;
}

/* ================= flash 槽位状态 =================
 * 每个槽位都是一份**独立**的命名映像。哪些槽位有内容、序号、字节数都各自独立；
 * 「上电自动加载」在这 4 个里选一个（或选「不推送」）。 */
typedef struct {
    bool     valid;
    uint32_t seq;
    uint32_t bytes;
} dimg_slot_t;

static dimg_slot_t s_slots[DIMG_SLOT_COUNT];
static int      s_newest_slot = -1;      /* 序号最大的有效槽位（导出 / 回退用） */
static uint32_t s_newest_seq;
static uint32_t s_newest_bytes;

/* 配置（NVS namespace "dspimg"） */
static char s_names[DIMG_SLOT_COUNT][DIMG_NAME_MAX];
static bool s_auto_load;                 /* 上电自动加载总开关 */
static int  s_boot_slot = -1;            /* 开机推送目标槽位，-1 = 不推送 */
static bool s_boot_slot_unset = true;    /* NVS 里还没写过 boot 键（迁移用） */

/* 上电自动加载的执行状态（持续监控 1452） */
static dsp_image_boot_result_t s_boot_res;
static uint32_t s_push_count;            /* 成功推送次数（用于日志与网页） */
static TickType_t s_boot_last_tick;      /* 上次巡检时刻 */
static bool     s_watch;                 /* 是否启用监控（开 + 选中了有效槽位） */
static bool     s_dsp_online;            /* 上次判断的 1452 在线状态 */
static int      s_off_cnt;               /* 连续探测失败次数 */
static int      s_bad_cnt;               /* 连续发现程序被改写的次数 */
static TickType_t s_settle_tick;         /* 1452 本次上线的时刻（0 = 还没开始计时） */
/* 探测到上线后等多久再推：s_settle_cfg_ms 是网页上配置的值，
 * s_settle_ms 是当前生效值（失败后自动加长，成功后回到配置值）。 */
static uint32_t s_settle_cfg_ms = DIMG_SETTLE_DEFAULT_MS;
static uint32_t s_settle_ms = DIMG_SETTLE_DEFAULT_MS;
static bool     s_paused;                /* 上次 tick 处于暂停（桥接中） */
static bool     s_skip_push_once;        /* 从暂停恢复后本次不重复推（见 boot_tick） */
static uint32_t s_repush_cnt;            /* 本次开机已自动重推次数 */
static bool     s_repush_warned;

/* 推送成功后记下一个"见证寄存器"（我们写过、后续不会改的一个普通寄存器），
 * 与 PLL_LOCK / CORE_STATUS 一起用于健康巡检，判断 1452 里的程序还在不在运行。
 * 见 dsp_image_boot_tick() 里的健康巡检段。 */
static bool     s_sig_valid;
static uint16_t s_sig_addr;
static uint16_t s_sig_val;

/* 由「上电自动加载开关 + 选中槽位」重新装配监控（定义在文件后半部分） */
static void boot_arm(void);
static void boot_force_repush(void);

static volatile bool s_replay_busy;

bool dsp_image_busy(void)
{
    return s_replay_busy;
}

/* ================= 工具 ================= */
static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 大缓冲分配：优先 PSRAM（若将来启用），失败退回内部堆。
 * 注意：映像的保存/导出已改为**全程流式**，正常路径不再需要大块连续内存 ——
 * ESP32-S3 内部堆只剩 ~260KB 且碎片严重（free=44KB 时最大连续块仅 ~12KB），
 * 而板上 8MB PSRAM 是 Octal 模式，启用会占用 GPIO33~37，与 EC11 引脚冲突，
 * 因此不依赖 PSRAM。此函数仅用于几百字节的小缓冲与将来的余量。 */
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = malloc(n);
    }
    return p;
}

static void big_free(void *p)
{
    heap_caps_free(p);
}

static inline bool img_lock(void)
{
    return s_mtx != NULL && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(3000)) == pdTRUE;
}

static inline void img_unlock(void)
{
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
}

/* ================= 内存区段管理 ================= */
static inline bool ext_covers(const dimg_ext_t *e, uint16_t addr, uint32_t words)
{
    return addr >= e->base && (uint32_t)(addr - e->base) + words <= e->words;
}

static inline bool ext_touches(const dimg_ext_t *e, uint16_t addr, uint32_t words)
{
    uint32_t a0 = addr, a1 = (uint32_t)addr + words;
    uint32_t b0 = e->base, b1 = (uint32_t)e->base + e->words;
    return a0 <= b1 && b0 <= a1;
}

/* 把 [addr, addr+words) 并入区段（必要时扩容），保留其它字节 */
static bool ext_put(dimg_ext_t *e, uint16_t addr, uint32_t words, const uint8_t *data)
{
    uint32_t nb = e->base < addr ? e->base : (uint32_t)addr;
    uint32_t b1 = (uint32_t)e->base + e->words;
    uint32_t a1 = (uint32_t)addr + words;
    uint32_t ne = b1 > a1 ? b1 : a1;

    if (nb != e->base || ne != b1) {
        uint32_t nwords = ne - nb;
        if ((uint64_t)nwords * 4 > DIMG_MAX_BYTES) {
            return false;
        }
        uint8_t *nd = big_alloc(nwords * 4);
        if (nd == NULL) {
            return false;
        }
        memcpy(nd + (uint32_t)(e->base - nb) * 4, e->d, e->words * 4);
        s_ram_bytes += (nwords - e->words) * 4;
        big_free(e->d);
        e->d = nd;
        e->base = (uint16_t)nb;
        e->words = nwords;
    }
    memcpy(e->d + (uint32_t)(addr - e->base) * 4, data, (size_t)words * 4);
    return true;
}

/* ================= 录制 ================= */
void dsp_image_record(uint16_t addr, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || !s_recording || !img_lock()) {
        return;
    }

    /* --- 寄存器：按 16 位末次生效 --- */
    if (addr >= DIMG_REG_BASE) {
        for (size_t i = 0; i + 2 <= len; i += 2) {
            uint16_t a = (uint16_t)(addr + i);
            if (a < DIMG_REG_BASE) {
                break;
            }
            uint32_t idx = a - DIMG_REG_BASE;
            uint8_t bit = (uint8_t)(1u << (idx & 7));
            if (!(s_reg_valid[idx >> 3] & bit)) {
                s_reg_valid[idx >> 3] |= bit;
                s_reg_count++;
            }
            s_reg[idx] = (uint16_t)(((uint16_t)data[i] << 8) | data[i + 1]);
        }
        img_unlock();
        return;
    }

    /* --- DSP 内存：按 32 位字 --- */
    if ((len % 4) != 0) {
        img_unlock();
        return;
    }
    uint32_t words = (uint32_t)(len / 4);
    if ((uint64_t)words * 4 > DIMG_MAX_BYTES) {
        img_unlock();
        return;
    }

    /* 1) 已完全覆盖 -> 原地更新 */
    for (int i = 0; i < s_ext_count; i++) {
        dimg_ext_t *e = &s_ext[i];
        if (ext_covers(e, addr, words)) {
            memcpy(e->d + (uint32_t)(addr - e->base) * 4, data, (size_t)words * 4);
            img_unlock();
            return;
        }
    }
    /* 2) 相邻/重叠 -> 合并扩容 */
    for (int i = 0; i < s_ext_count; i++) {
        if (ext_touches(&s_ext[i], addr, words)) {
            if (ext_put(&s_ext[i], addr, words, data)) {
                img_unlock();
                return;
            }
            break;
        }
    }
    /* 3) 新建区段 */
    if (s_ext_count >= DIMG_MAX_EXT) {
        ESP_LOGW(TAG, "录制区段已满(%d)，忽略 0x%04X +%u 字", DIMG_MAX_EXT, addr, (unsigned)words);
        img_unlock();
        return;
    }
    dimg_ext_t *e = &s_ext[s_ext_count];
    e->d = big_alloc((size_t)words * 4);
    if (e->d == NULL) {
        ESP_LOGW(TAG, "分配 %u 字节失败", (unsigned)(words * 4));
        img_unlock();
        return;
    }
    e->base = addr;
    e->words = words;
    memcpy(e->d, data, (size_t)words * 4);
    s_ext_count++;
    s_ram_bytes += words * 4;
    img_unlock();
}

/* ================= RAM 状态查询 ================= */
bool dsp_image_ram_valid(void)
{
    return s_reg_count > 0 || s_ext_count > 0;
}

/*
 * 是否录到了程序 RAM（>= 0xC000）。
 * SigmaStudio 的 Link Compile Download 一定会写程序 RAM；只有它存在，
 * 这份映像才是"可独立启动 DSP"的完整程序。否则回放会做软复位却没有时钟/串口
 * 配置和程序，DSP 会起不来（失声）。所以保存与回放前都要检查。
 */
bool dsp_image_ram_has_program(void)
{
    for (int i = 0; i < s_ext_count; i++) {
        if (s_ext[i].base >= ADAU1452_ADDR_PROG_BASE && s_ext[i].words > 0) {
            return true;
        }
    }
    return false;
}

uint32_t dsp_image_ram_regs(void)  { return s_reg_count; }
uint32_t dsp_image_ram_exts(void)  { return (uint32_t)s_ext_count; }
uint32_t dsp_image_ram_bytes(void) { return s_ram_bytes; }

void dsp_image_ram_clear(void)
{
    if (!img_lock()) {
        return;
    }
    for (int i = 0; i < s_ext_count; i++) {
        big_free(s_ext[i].d);
        s_ext[i].d = NULL;
    }
    s_ext_count = 0;
    memset(s_reg_valid, 0, sizeof(s_reg_valid));
    s_reg_count = 0;
    s_ram_bytes = 0;
    img_unlock();
    ESP_LOGI(TAG, "RAM 录制已清空");
}

/* ================= 序列化（流式） =================
 * payload 布局：
 *   u32 reg_count | u32 ext_count
 *   regs: reg_count × { u16 addr, u16 value }        （按地址升序，回放稳定）
 *   exts: ext_count × { u16 base, u16 rsvd, u32 words }
 *   data: 各区段内容依次拼接（字内大端）
 *
 * 之所以全程流式（而不是拼成一个 buffer）：
 * ESP32-S3 内部堆只有 ~260KB，WiFi+HTTP 用掉后 free≈44KB 但最大连续块仅 ~12KB，
 * 而映像需要 ~19KB 连续内存 —— 必然失败。且板上 8MB PSRAM 是 Octal 模式，
 * 启用会占用 GPIO33~37，正是 EC11 的 A/B/PUSH 引脚，因此不能靠 PSRAM。
 * 流式处理后只用到几百字节的栈缓冲。
 */
static size_t payload_size(void)
{
    size_t n = 8 + (size_t)s_reg_count * 4 + (size_t)s_ext_count * 8;
    for (int i = 0; i < s_ext_count; i++) {
        n += (size_t)s_ext[i].words * 4;
    }
    return n;
}

static esp_err_t walk_payload(dsp_image_sink_t sink, void *ctx)
{
    uint8_t hdr[8];
    wr32(hdr, s_reg_count);
    wr32(hdr + 4, (uint32_t)s_ext_count);
    esp_err_t e = sink(hdr, sizeof(hdr), ctx);
    if (e != ESP_OK) {
        return e;
    }

    /* 寄存器：按地址升序，攒成 256 字节一批 */
    uint8_t rb[256];
    size_t rn = 0;
    for (uint32_t idx = 0; idx < DIMG_REG_COUNT; idx++) {
        if (!(s_reg_valid[idx >> 3] & (1u << (idx & 7)))) {
            continue;
        }
        uint16_t a = (uint16_t)(DIMG_REG_BASE + idx);
        rb[rn++] = (uint8_t)(a >> 8);
        rb[rn++] = (uint8_t)(a & 0xFF);
        rb[rn++] = (uint8_t)(s_reg[idx] >> 8);
        rb[rn++] = (uint8_t)(s_reg[idx] & 0xFF);
        if (rn == sizeof(rb)) {
            e = sink(rb, rn, ctx);
            if (e != ESP_OK) {
                return e;
            }
            rn = 0;
        }
    }
    if (rn > 0) {
        e = sink(rb, rn, ctx);
        if (e != ESP_OK) {
            return e;
        }
    }

    /* 区段表（最多 24 项 = 192 字节，栈上足够） */
    if (s_ext_count > 0) {
        uint8_t eb[DIMG_MAX_EXT * 8];
        uint8_t *p = eb;
        for (int i = 0; i < s_ext_count; i++) {
            *p++ = (uint8_t)(s_ext[i].base >> 8);
            *p++ = (uint8_t)(s_ext[i].base & 0xFF);
            *p++ = 0;
            *p++ = 0;
            wr32(p, s_ext[i].words);
            p += 4;
        }
        e = sink(eb, (size_t)s_ext_count * 8, ctx);
        if (e != ESP_OK) {
            return e;
        }
    }

    /* 区段数据 */
    for (int i = 0; i < s_ext_count; i++) {
        e = sink(s_ext[i].d, (size_t)s_ext[i].words * 4, ctx);
        if (e != ESP_OK) {
            return e;
        }
    }
    return ESP_OK;
}

/* ================= flash 槽位管理 ================= */
static inline uint32_t slot_off(int slot)
{
    return (uint32_t)slot * DIMG_SLOT_SIZE;
}

static esp_err_t read_hdr(int slot, uint32_t *seq, uint32_t *plen, uint32_t *crc)
{
    uint8_t h[DIMG_HDR_SIZE];
    esp_err_t e = esp_partition_read(s_part, slot_off(slot), h, sizeof(h));
    if (e != ESP_OK) {
        return e;
    }
    if (rd32(h) != DIMG_MAGIC) {
        return ESP_ERR_NOT_FOUND;
    }
    if (seq)  *seq  = rd32(h + 4);
    if (plen) *plen = rd32(h + 8);
    if (crc)  *crc  = rd32(h + 12);
    return ESP_OK;
}

static esp_err_t payload_crc(int slot, uint32_t plen, uint32_t *out_crc)
{
    uint8_t buf[DIMG_FLASH_CHUNK];
    uint32_t crc = 0, off = 0;
    while (off < plen) {
        uint32_t n = plen - off;
        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        esp_err_t e = esp_partition_read(s_part, slot_off(slot) + DIMG_HDR_SIZE + off, buf, n);
        if (e != ESP_OK) {
            return e;
        }
        crc = esp_rom_crc32_le(crc, buf, n);
        off += n;
    }
    *out_crc = crc;
    return ESP_OK;
}

/* ================= 配置持久化（NVS namespace "dspimg"） =================
 * 名称、上电自动加载开关、开机推送槽位都存这里。以前「上电自动加载」存在
 * bridge 命名空间（键 autoload），下面会做一次性迁移。 */
static void cfg_nvs_save_boot(void)
{
    nvs_handle_t h;
    if (nvs_open(DIMG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    const uint8_t v = (s_boot_slot < 0 || s_boot_slot >= DIMG_SLOT_COUNT)
                      ? 0xFF : (uint8_t)s_boot_slot;
    nvs_set_u8(h, "boot", v);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_nvs_save_auto(void)
{
    nvs_handle_t h;
    if (nvs_open(DIMG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "auto", s_auto_load ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_nvs_save_settle(void)
{
    nvs_handle_t h;
    if (nvs_open(DIMG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, "settle", s_settle_cfg_ms);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_nvs_save_name(int idx)
{
    static const char *keys[DIMG_SLOT_COUNT] = { "n0", "n1", "n2", "n3" };
    if (idx < 0 || idx >= DIMG_SLOT_COUNT) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(DIMG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, keys[idx], s_names[idx]);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_nvs_load(void)
{
    static const char *keys[DIMG_SLOT_COUNT] = { "n0", "n1", "n2", "n3" };
    nvs_handle_t h;
    if (nvs_open(DIMG_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS(\"%s\") 失败，使用默认配置", DIMG_NS);
        return;
    }

    uint8_t u8 = 0;
    uint32_t u32v = 0;
    bool have_auto = (nvs_get_u8(h, "auto", &u8) == ESP_OK);
    if (have_auto) {
        s_auto_load = (u8 != 0);
    }
    if (nvs_get_u32(h, "settle", &u32v) == ESP_OK && u32v <= DIMG_SETTLE_LIMIT_MS) {
        s_settle_cfg_ms = u32v;
        s_settle_ms = u32v;
    }
    if (nvs_get_u8(h, "boot", &u8) == ESP_OK) {
        s_boot_slot_unset = false;
        s_boot_slot = (u8 >= DIMG_SLOT_COUNT) ? -1 : (int)u8;
    }
    for (int i = 0; i < DIMG_SLOT_COUNT; i++) {
        size_t n = sizeof(s_names[i]);
        s_names[i][0] = '\0';
        nvs_get_str(h, keys[i], s_names[i], &n);
    }

    if (!have_auto) {
        /* 迁移：旧版本把「上电自动加载」开关存在 bridge 命名空间的 autoload 键 */
        nvs_handle_t ob;
        if (nvs_open("bridge", NVS_READONLY, &ob) == ESP_OK) {
            uint8_t old = 0;
            if (nvs_get_u8(ob, "autoload", &old) == ESP_OK && old) {
                s_auto_load = true;
                nvs_set_u8(h, "auto", 1);
                nvs_commit(h);
                ESP_LOGI(TAG, "已迁移旧设置：「上电自动加载」= 开");
            }
            nvs_close(ob);
        }
    }
    nvs_close(h);
}

static void scan_flash(void)
{
    int valid_count = 0;

    s_newest_slot = -1;
    s_newest_seq = 0;
    s_newest_bytes = 0;

    for (int i = 0; i < DIMG_SLOT_COUNT; i++) {
        uint32_t seq = 0, plen = 0, crc = 0, got = 0;

        s_slots[i].valid = false;
        s_slots[i].seq = 0;
        s_slots[i].bytes = 0;

        if (read_hdr(i, &seq, &plen, &crc) != ESP_OK) {
            ESP_LOGI(TAG, "槽 %d：空", i);
            continue;
        }
        if (plen == 0 || plen > DIMG_SLOT_SIZE - DIMG_HDR_SIZE) {
            ESP_LOGW(TAG, "槽 %d：长度非法 %lu", i, (unsigned long)plen);
            continue;
        }
        if (payload_crc(i, plen, &got) != ESP_OK || got != crc) {
            ESP_LOGW(TAG, "槽 %d SEQ=%lu CRC 校验失败（存储 0x%08lX / 实算 0x%08lX）",
                     i, (unsigned long)seq, (unsigned long)crc, (unsigned long)got);
            continue;
        }

        s_slots[i].valid = true;
        s_slots[i].seq = seq;
        s_slots[i].bytes = plen;
        valid_count++;
        ESP_LOGI(TAG, "槽 %d：有效 seq=%lu %lu 字节 名称=\"%s\"",
                 i, (unsigned long)seq, (unsigned long)plen,
                 s_names[i][0] ? s_names[i] : "(未命名)");

        if (s_newest_slot < 0 || seq > s_newest_seq) {
            s_newest_slot = i;
            s_newest_seq = seq;
            s_newest_bytes = plen;
        }
    }

    if (s_newest_slot >= 0) {
        ESP_LOGI(TAG, "flash 映像：%d 个有效槽位，最新一份在槽 %d（seq=%lu，%lu 字节）",
                 valid_count, s_newest_slot,
                 (unsigned long)s_newest_seq, (unsigned long)s_newest_bytes);
    } else {
        ESP_LOGI(TAG, "flash 映像：没有有效槽位");
    }
}

bool dsp_image_flash_valid(void) { return s_newest_slot >= 0; }
uint32_t dsp_image_flash_seq(void) { return s_newest_slot >= 0 ? s_newest_seq : 0; }
uint32_t dsp_image_flash_bytes(void) { return s_newest_slot >= 0 ? s_newest_bytes : 0; }

int dsp_image_slot_count(void)
{
    return DIMG_SLOT_COUNT;
}

esp_err_t dsp_image_slot_get(int idx, dsp_image_slot_t *out)
{
    if (out == NULL || idx < 0 || idx >= DIMG_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    out->index = idx;
    out->valid = s_slots[idx].valid;
    out->seq = s_slots[idx].seq;
    out->bytes = s_slots[idx].bytes;
    memcpy(out->name, s_names[idx], DIMG_NAME_MAX);
    out->name[DIMG_NAME_MAX - 1] = '\0';
    return ESP_OK;
}

esp_err_t dsp_image_slot_rename(int idx, const char *name)
{
    if (idx < 0 || idx >= DIMG_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (name == NULL) {
        name = "";
    }
    /* 控制字符（\n \r \t …）在网页里会搞乱显示，统一换成空格；
     * 0x80 以上是 UTF-8 续接字节，必须原样保留（中文名字）。 */
    char tmp[DIMG_NAME_MAX];
    size_t n = 0;
    for (const char *p = name; *p != '\0' && n < DIMG_NAME_MAX - 1; p++) {
        unsigned char c = (unsigned char)*p;
        tmp[n++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    /*
     * ⚠️ 长度截断可能把一个汉字切成两半。不合法的 UTF-8 会让整个 WebSocket
     * 文本帧被浏览器拒绝（页面直接变成"已断开"），所以这里把结尾不完整的
     * 序列整体丢掉。
     */
    for (size_t back = 0; back < n && back < 4; ) {
        unsigned char c = (unsigned char)tmp[n - 1 - back];
        if ((c & 0xC0) != 0x80) {                 /* 找到起始字节（或 ASCII） */
            const int need = (c < 0x80) ? 1
                           : (((c & 0xE0) == 0xC0) ? 2 : (((c & 0xF0) == 0xE0) ? 3 : 4));
            if (back + 1 < (size_t)need) {
                n -= back + 1;                    /* 序列不完整 → 整体丢弃 */
            }
            break;
        }
        back++;
    }
    tmp[n] = '\0';
    /* 去掉首尾空格 */
    size_t s = 0;
    while (tmp[s] == ' ') {
        s++;
    }
    size_t e = strlen(tmp);
    while (e > s && tmp[e - 1] == ' ') {
        tmp[--e] = '\0';
    }
    if (s > 0) {
        memmove(tmp, tmp + s, e - s + 1);
    }

    if (strcmp(tmp, s_names[idx]) == 0) {
        return ESP_OK;
    }
    memcpy(s_names[idx], tmp, strlen(tmp) + 1);
    cfg_nvs_save_name(idx);
    ESP_LOGI(TAG, "槽 %d 重命名为 \"%s\"", idx, s_names[idx][0] ? s_names[idx] : "(未命名)");
    return ESP_OK;
}

esp_err_t dsp_image_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_boot_res.state = DSP_BOOT_IDLE;
    s_boot_res.slot = -1;
    s_boot_res.uptime_ms = 0;
    s_boot_res.attempts = 0;
    snprintf(s_boot_res.msg, sizeof(s_boot_res.msg), "未启用上电自动加载");

    /* 先读配置：scan_flash 的日志里要带上用户起的名字 */
    cfg_nvs_load();

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                      DIMG_PART_LABEL);
    if (s_part == NULL) {
        ESP_LOGW(TAG, "未找到分区 \"%s\"，映像存储功能不可用", DIMG_PART_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "分区 \"%s\" @0x%08lx 大小 %lu 字节（%d 个槽位）",
             DIMG_PART_LABEL, (unsigned long)s_part->address,
             (unsigned long)s_part->size, DIMG_SLOT_COUNT);

    scan_flash();

    /* 兼容旧行为：以前没有"选哪一份"的概念，固定用最新的一份。
     * 首次升级上来时把「开机推送」指到当前最新的有效槽位。 */
    if (s_boot_slot_unset) {
        s_boot_slot = s_auto_load ? s_newest_slot : -1;
        cfg_nvs_save_boot();
        ESP_LOGI(TAG, "「开机推送」未设置过，按最新映像初始化为槽 %d", s_boot_slot);
    }
    ESP_LOGI(TAG, "上电自动加载：%s；开机推送：%s；就绪等待：%lu ms",
             s_auto_load ? "开" : "关",
             s_boot_slot < 0 ? "不推送"
                             : (s_slots[s_boot_slot].valid ? s_names[s_boot_slot][0] ? s_names[s_boot_slot] : "槽位（未命名）" : "槽位（空）"),
             (unsigned long)s_settle_cfg_ms);
    return ESP_OK;
}

/* ================= 上电自动加载：配置读写 ================= */
bool dsp_image_auto_load(void)
{
    return s_auto_load;
}

uint32_t dsp_image_settle_ms(void)
{
    return s_settle_cfg_ms;
}

esp_err_t dsp_image_set_settle_ms(uint32_t ms)
{
    if (ms > DIMG_SETTLE_LIMIT_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_settle_cfg_ms = ms;
    s_settle_ms = ms;              /* 配置值生效，丢弃自适应的加长 */
    cfg_nvs_save_settle();
    ESP_LOGI(TAG, "就绪等待时间设为 %lu ms", (unsigned long)ms);
    return ESP_OK;
}

esp_err_t dsp_image_set_auto_load(bool on)
{
    if (s_auto_load == on) {
        return ESP_OK;
    }
    s_auto_load = on;
    cfg_nvs_save_auto();
    ESP_LOGI(TAG, "上电自动加载：%s", on ? "开" : "关");
    if (on) {
        boot_force_repush();   /* 刚打开：立刻等 1452 就绪后推一次 */
    }
    boot_arm();                /* 立即生效，不用重启 */
    return ESP_OK;
}

int dsp_image_boot_slot(void)
{
    return s_boot_slot;
}

esp_err_t dsp_image_set_boot_slot(int idx)
{
    if (idx < -1 || idx >= DIMG_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (idx == s_boot_slot) {
        return ESP_OK;
    }
    s_boot_slot = idx;
    s_boot_slot_unset = false;
    cfg_nvs_save_boot();
    if (idx < 0) {
        ESP_LOGI(TAG, "开机推送改为：不推送");
    } else {
        ESP_LOGI(TAG, "开机推送改为：槽 %d（%s）", idx,
                 s_slots[idx].valid ? (s_names[idx][0] ? s_names[idx] : "未命名") : "空槽");
        if (!s_slots[idx].valid) {
            ESP_LOGW(TAG, "  注意：该槽位当前没有有效映像，上电时不会推送");
        }
    }
    boot_force_repush();   /* 换了目标：重新走一轮推送 */
    boot_arm();
    return ESP_OK;
}


/* 保存时把 walk_payload 的输出直接写进 flash，同时累计 CRC（无需大块内存） */
typedef struct {
    uint32_t off;      /* payload 内偏移 */
    uint32_t crc;
    size_t   total;
    uint8_t *buf;      /* 小缓冲，凑满扇区对齐再写 */
    size_t   n;
    esp_err_t err;
} flash_writer_t;

static esp_err_t flash_sink(const uint8_t *data, size_t len, void *ctx)
{
    flash_writer_t *w = ctx;
    if (w->err != ESP_OK) {
        return w->err;
    }
    w->crc = esp_rom_crc32_le(w->crc, data, (uint32_t)len);

    size_t done = 0;
    while (done < len) {
        size_t room = DIMG_FLASH_CHUNK - w->n;
        size_t n = len - done;
        if (n > room) {
            n = room;
        }
        memcpy(w->buf + w->n, data + done, n);
        w->n += n;
        done += n;
        if (w->n == DIMG_FLASH_CHUNK) {
            w->err = esp_partition_write(s_part, w->off, w->buf, w->n);
            if (w->err != ESP_OK) {
                return w->err;
            }
            w->off += (uint32_t)w->n;
            w->total += w->n;
            w->n = 0;
        }
    }
    return ESP_OK;
}

esp_err_t dsp_image_save_to_flash(int *slot_out)
{
    if (slot_out) {
        *slot_out = -1;
    }
    if (s_part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_reg_count && !s_ext_count) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dsp_image_ram_has_program()) {
        ESP_LOGW(TAG, "录制中缺少程序 RAM，拒绝保存（请先在 SigmaStudio 里做一次 Link Compile Download）");
        return ESP_ERR_INVALID_STATE;
    }

    /* 选目标槽：已用序号最小者（未用过的槽 seq=0 天然优先，实现轮换） */
    uint32_t max_seq = 0, min_seq = 0xFFFFFFFFu;
    int target = 0;
    for (int i = 0; i < DIMG_SLOT_COUNT; i++) {
        const uint32_t seq = s_slots[i].valid ? s_slots[i].seq : 0;
        if (seq > max_seq) {
            max_seq = seq;
        }
        if (seq < min_seq) {
            min_seq = seq;
            target = i;
        }
    }
    const uint32_t new_seq = max_seq + 1;

    if (!img_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    const size_t plen = payload_size();
    ESP_LOGI(TAG, "保存映像到槽 %d（seq=%lu，%lu 字节）",
             target, (unsigned long)new_seq, (unsigned long)plen);

    esp_err_t e = esp_partition_erase_range(s_part, slot_off(target), DIMG_SLOT_SIZE);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "擦除槽 %d 失败：%s", target, esp_err_to_name(e));
        goto out;
    }

    {
        flash_writer_t w = {
            .off = slot_off(target) + DIMG_HDR_SIZE,
            .crc = 0,
            .total = 0,
            .buf = big_alloc(DIMG_FLASH_CHUNK),
            .n = 0,
            .err = ESP_OK,
        };
        if (w.buf == NULL) {
            e = ESP_ERR_NO_MEM;
            goto out;
        }
        e = walk_payload(flash_sink, &w);
        /* 收尾：写完尾块 */
        if (e == ESP_OK && w.n > 0) {
            e = esp_partition_write(s_part, w.off, w.buf, w.n);
            if (e == ESP_OK) {
                w.off += (uint32_t)w.n;
                w.total += w.n;
            }
        }
        big_free(w.buf);

        if (e != ESP_OK) {
            ESP_LOGE(TAG, "写入 payload 失败：%s", esp_err_to_name(e));
            goto out;
        }
        if (w.total != plen) {
            ESP_LOGE(TAG, "写入长度不符：%lu != %lu",
                     (unsigned long)w.total, (unsigned long)plen);
            e = ESP_FAIL;
            goto out;
        }

        /* 写后校验 */
        uint32_t got = 0;
        if (payload_crc(target, (uint32_t)plen, &got) != ESP_OK || got != w.crc) {
            ESP_LOGE(TAG, "写后校验失败");
            e = ESP_ERR_INVALID_CRC;
            goto out;
        }

        /* 最后写槽头（此时映像已完整，掉电只会丢掉未提交的槽） */
        uint8_t h[DIMG_HDR_SIZE];
        memset(h, 0, sizeof(h));
        wr32(h, DIMG_MAGIC);
        wr32(h + 4, new_seq);
        wr32(h + 8, (uint32_t)plen);
        wr32(h + 12, w.crc);
        e = esp_partition_write(s_part, slot_off(target), h, sizeof(h));
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "写入槽头失败：%s", esp_err_to_name(e));
            goto out;
        }
    }

    s_slots[target].valid = true;
    s_slots[target].seq = new_seq;
    s_slots[target].bytes = (uint32_t)plen;
    if (s_newest_slot < 0 || new_seq > s_newest_seq) {
        s_newest_slot = target;
        s_newest_seq = new_seq;
        s_newest_bytes = (uint32_t)plen;
    }
    /* 名字属于「那一份固件」而不是槽位：槽位被新固件覆盖后旧名字作废 */
    if (s_names[target][0] != '\0') {
        s_names[target][0] = '\0';
        cfg_nvs_save_name(target);
    }
    /* 开了上电自动加载却没有可推送的目标时（比如第一次保存），
     * 自动指向刚存下来的这份，避免「开了开关却没反应」。 */
    if (s_auto_load && (s_boot_slot < 0 || !s_slots[s_boot_slot].valid)) {
        s_boot_slot = target;
        s_boot_slot_unset = false;
        cfg_nvs_save_boot();
        ESP_LOGI(TAG, "开机推送自动指向刚保存的槽 %d", target);
    }
    if (slot_out) {
        *slot_out = target;
    }
    ESP_LOGI(TAG, "映像已保存（槽 %d，seq=%lu，CRC=0x%08lX）",
             target, (unsigned long)new_seq, (unsigned long)s_newest_bytes);
    /* 之前选中的槽位为空/被本份覆盖时，上面已把开机推送指到 target，
     * 这里重新装配一下，让监控立刻生效 */
    boot_arm();

out:
    img_unlock();
    return e;
}

/* 擦除单个槽位（名字一并清掉，因为名字属于那份固件） */
esp_err_t dsp_image_slot_erase(int idx)
{
    if (s_part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (idx < 0 || idx >= DIMG_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 只擦第 1 个扇区（含槽头）即可让映像失效，无需擦满 256KB */
    esp_err_t e = esp_partition_erase_range(s_part, slot_off(idx), 4096);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "擦除槽 %d 失败：%s", idx, esp_err_to_name(e));
        return e;
    }
    s_slots[idx].valid = false;
    s_slots[idx].seq = 0;
    s_slots[idx].bytes = 0;
    if (s_names[idx][0] != '\0') {
        s_names[idx][0] = '\0';
        cfg_nvs_save_name(idx);
    }
    if (s_newest_slot == idx) {
        s_newest_slot = -1;
        s_newest_seq = 0;
        s_newest_bytes = 0;
        for (int i = 0; i < DIMG_SLOT_COUNT; i++) {
            if (s_slots[i].valid && (s_newest_slot < 0 || s_slots[i].seq > s_newest_seq)) {
                s_newest_slot = i;
                s_newest_seq = s_slots[i].seq;
                s_newest_bytes = s_slots[i].bytes;
            }
        }
    }
    /* 删掉的正是开机推送目标：改指到还剩的最新一份，没有就改为「不推送」，
     * 免得网页上「开机推送」指着一个空槽位。 */
    if (s_boot_slot == idx) {
        s_boot_slot = s_newest_slot;
        cfg_nvs_save_boot();
        ESP_LOGI(TAG, "开机推送目标被删除，改为槽 %d", s_boot_slot);
    }
    boot_arm();
    ESP_LOGI(TAG, "槽 %d 的映像已清除", idx);
    return ESP_OK;
}

esp_err_t dsp_image_flash_erase(void)
{
    if (s_part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < DIMG_SLOT_COUNT; i++) {
        esp_err_t e = dsp_image_slot_erase(i);
        if (e != ESP_OK) {
            return e;
        }
    }
    ESP_LOGI(TAG, "flash 映像已全部清除");
    return ESP_OK;
}

/* 在已录寄存器表里查某个核心控制寄存器的值，没有则用默认值 */
static uint16_t lookup_reg(const uint8_t *regs, uint32_t count, uint16_t addr, uint16_t def)
{
    for (uint32_t i = 0; i < count; i++) {
        if (be16(regs + i * 4) == addr) {
            return be16(regs + i * 4 + 2);
        }
    }
    return def;
}

/*
 * 内存写入校验：**这才是"推送成功"的真凭据**。
 *
 * 背景（用户实测）：1452 断电重启后，我们"推送成功"却不出声 —— 只回读了一个刚
 * 写过的寄存器（当然对）就宣布成功，而**程序内存根本没落实**。
 * 最可能的原因：推送时序与 1452 自身状态冲突时，内存接口只完成了一半，
 * 于是 I2C 有 ACK 但数据没进 RAM。
 *
 * 为什么不能直接读 PROGRAM 区：ADAU1452 的 **PM 读回恒为 0**（实测 0xC000 全 0），
 * 但 **PARAM / DM 是可读的**，而且实测与映像逐字一致 —— 所以拿可读区段做比对。
 *
 * ⚠️ 判定不能用"有一字不符就算失败"：区段里有程序**运行时自己会更新**的参数
 *    （例如 Level Detector 的输出就落在 PARAM 前 64 字里），个别字随时可能变。
 *    真正要区分的是"整段都没写进去"，所以按**一致字数 vs 不符字数**判定。
 *
 * 返回 ESP_OK 表示"校验通过或无法校验"，ESP_ERR_INVALID_CRC 表示**确认写失败**。
 */
static esp_err_t verify_mem_written(const uint8_t *exts, uint32_t ext_count,
                                    uint32_t base, uint32_t data_off, uint32_t plen)
{
    uint8_t img[DIMG_VERIFY_WORDS * 4];
    uint8_t got[DIMG_VERIFY_WORDS * 4];
    uint32_t same = 0, diff = 0;

    for (uint32_t j = 0; j < ext_count; j++) {
        const uint16_t ebase = be16(exts + j * 8);
        const uint32_t words = rd32(exts + j * 8 + 4);
        const uint32_t seg_bytes = words * 4;
        if (data_off + seg_bytes > plen) {      /* 越界：正常不会发生 */
            break;
        }
        if (ebase < ADAU1452_ADDR_PROG_BASE && words > 0) {
            const uint32_t n = (words > DIMG_VERIFY_WORDS) ? DIMG_VERIFY_WORDS : words;
            const uint32_t bytes = n * 4;
            if (esp_partition_read(s_part, base + data_off, img, bytes) == ESP_OK &&
                adau1452_read_block(ebase, got, bytes) == ESP_OK) {
                for (uint32_t i = 0; i < n; i++) {
                    if (memcmp(img + i * 4, got + i * 4, 4) == 0) {
                        same++;
                    } else {
                        diff++;
                        if (diff <= 2) {   /* 只打印前两条，避免刷屏 */
                            ESP_LOGW(TAG, "  校验不符：内存 0x%04X 字 %lu："
                                          "映像 0x%08lX，实读 0x%08lX",
                                     ebase, (unsigned long)i,
                                     (unsigned long)be32(img + i * 4),
                                     (unsigned long)be32(got + i * 4));
                        }
                    }
                }
            }
        }
        data_off += seg_bytes;
    }

    const uint32_t total = same + diff;
    if (total == 0) {
        ESP_LOGW(TAG, "  可读区段为空，本次跳过内存校验");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "  内存校验：%lu/%lu 字与映像一致", (unsigned long)same,
             (unsigned long)total);
    if (diff > same) {
        ESP_LOGE(TAG, "  不一致的字占多数，判定为没有写进去");
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

/* ================= 回放顺序：复刻 SigmaStudio default_download =================
 *
 * ⚠️⚠️ **顺序至关重要，绝不能按地址升序写**。
 *
 * 官方顺序见导出的 `default_download_IC_1()`（dsp_project 目录下的 IC_1.h，61 步），要点：
 *   ① 软复位 → `HIBERNATE` 0（唤醒）→ 1（进休眠）→ `KILL_CORE` 0 → 1；
 *   ② **PLL 先关（R8=0）→ 写 CTRL0/CTRL1/CLK_SRC/MCLK_OUT → 再开（R13=1）
 *      → 等 255ms 锁定**；
 *   ③ 只有 PLL 锁定之后才写 `POWER_ENABLE0/1`、`CLK_GEN3_M`、`MP7/MP11_MODE`、
 *      `SOUT_SOURCE`×24、`SERIAL_BYTE`×7（也就是**音频串口配置**）。
 *
 * 为什么这是"冷启动不出声"的根因：
 *   冷启动时 PLL 要从零开始锁。若在锁定前写时钟/串口寄存器，**它们不生效**
 *   → 音频串口从未被配置 → I2C 一切正常（寄存器值存得住、读得回，校验也通过）
 *   **但完全不出声**。而已初始化过的芯片 PLL 一直锁着，顺序错了照样能响 ——
 *   这正好解释了"ESP32 重启那次能出声、真正冷启动不出声"。
 */

/* 已经在上面的固定阶段里写过的地址（不再参与"兜底"遍历） */
static bool replay_addr_handled(uint16_t a)
{
    if (a >= ADAU1452_REG_PLL_CTRL0 && a <= ADAU1452_REG_MCLK_OUT) {
        return true;                                /* PLL 块 0xF000..0xF005 */
    }
    if (a == ADAU1452_REG_CLK_GEN3_M ||
        a == ADAU1452_REG_POWER_ENABLE0 || a == ADAU1452_REG_POWER_ENABLE1) {
        return true;
    }
    if (a == ADAU1452_REG_MP7_MODE || a == ADAU1452_REG_MP11_MODE) {
        return true;
    }
    if (a >= ADAU1452_REG_AUDIO_BASE && a <= ADAU1452_REG_AUDIO_END) {
        return true;                                /* SOUT_SOURCE×24 + SERIAL_BYTE×7 */
    }
    if (a >= ADAU1452_REG_HIBERNATE && a <= ADAU1452_REG_START_ADDRESS) {
        return true;                                /* 核心控制（最后单独写） */
    }
    if (a == ADAU1452_REG_SOFT_RESET) {
        return true;                                /* 软复位（已单独写） */
    }
    return false;
}

/* 映像里是否有这个地址（**有才写**，否则会把 0 写进本该保持默认的寄存器） */
static bool reg_present(const uint8_t *regs, uint32_t count, uint16_t addr)
{
    for (uint32_t i = 0; i < count; i++) {
        if (be16(regs + i * 4) == addr) {
            return true;
        }
    }
    return false;
}

/* 回放上下文：统计写失败 + 记一个"见证寄存器"（供巡检判断程序是否还在） */
typedef struct {
    const uint8_t *regs;
    uint32_t count;
    uint32_t fail;
    bool     sig_set;
    uint16_t sig_addr, sig_val;
} rp_t;

static void rp_put(rp_t *c, uint16_t addr, uint16_t val)
{
    if (adau1452_write_reg16(addr, val) != ESP_OK) {
        if (c->fail == 0) {
            ESP_LOGW(TAG, "  寄存器 0x%04X 写入失败（后续不再逐条打印）", addr);
        }
        c->fail++;
        return;
    }
    if (!c->sig_set) {
        c->sig_set = true;
        c->sig_addr = addr;
        c->sig_val = val;
    }
}

static void rp_put_img(rp_t *c, uint16_t addr, uint16_t def)
{
    rp_put(c, addr, lookup_reg(c->regs, c->count, addr, def));
}

/*
 * 推送结束后回读**硬件状态寄存器**，这是"1452 到底起来没有"的客观证据
 * （不用听声音就能判断，排查冷启动问题时尤其有用）：
 *   PLL_LOCK        1 = PLL 已锁定
 *   CLK_GEN3_LOCK   1 = 音频时钟发生器已锁定
 *   CORE_STATUS     0 = 核心已停；非 0 = 在运行
 *
 * ⚠️ CORE_STATUS 是**动态**的 —— 实测正常运行时在 1 与 3 之间跳动，
 *    所以只能判"是否等于 0"，**不能拿去和固定值比较**。
 */
static void log_hw_status(void)
{
    uint8_t b[2];
    int pll = -1, g3 = -1, core = -1;
    if (adau1452_read_block(ADAU1452_REG_PLL_LOCK, b, 2) == ESP_OK) {
        pll = be16(b);
    }
    if (adau1452_read_block(ADAU1452_REG_CLK_GEN3_LOCK, b, 2) == ESP_OK) {
        g3 = be16(b);
    }
    if (adau1452_read_block(ADAU1452_REG_CORE_STATUS, b, 2) == ESP_OK) {
        core = be16(b);
    }
    ESP_LOGI(TAG, "  1452 硬件状态：PLL_LOCK=%d CLK_GEN3_LOCK=%d CORE_STATUS=%d"
                  "（期望 1/1/非0，为 0 表示没起来）", pll, g3, core);
}

/*
 * 把某个槽位的映像回放到 DSP（会重置 DSP，耗时约 0.8~1s）。
 *
 * ⚠️ 与早期版本最重要的区别：**每一步 I2C 写入都检查返回值**。
 * 早期版本忽略所有写入错误，于是 DSP 还没上电时（1452 常常后于 ESP32 上电）
 * 会"静默成功"——日志打印"回放完成"，DSP 里却什么都没写进去。
 * 现在改为返回真实错误，让上层（上电自动加载）重试。
 */
static esp_err_t dimg_replay_slot(int idx)
{
    if (s_part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (idx < 0 || idx >= DIMG_SLOT_COUNT || !s_slots[idx].valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t base = slot_off(idx) + DIMG_HDR_SIZE;
    const uint32_t plen = s_slots[idx].bytes;

    uint8_t head[8];
    if (esp_partition_read(s_part, base, head, sizeof(head)) != ESP_OK) {
        return ESP_FAIL;
    }
    uint32_t reg_count = rd32(head);
    uint32_t ext_count = rd32(head + 4);
    if (reg_count > DIMG_REG_COUNT || ext_count > DIMG_MAX_EXT) {
        ESP_LOGE(TAG, "映像表头异常：regs=%lu exts=%lu",
                 (unsigned long)reg_count, (unsigned long)ext_count);
        return ESP_ERR_INVALID_STATE;
    }

    size_t regs_b = (size_t)reg_count * 4;
    size_t exts_b = (size_t)ext_count * 8;
    if (8 + regs_b + exts_b > plen) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *regs = big_alloc(regs_b ? regs_b : 1);
    uint8_t *exts = big_alloc(exts_b ? exts_b : 1);
    if (regs == NULL || exts == NULL) {
        big_free(regs);
        big_free(exts);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t e = ESP_OK;
    if (regs_b && esp_partition_read(s_part, base + 8, regs, regs_b) != ESP_OK) {
        e = ESP_FAIL; goto out;
    }
    if (exts_b && esp_partition_read(s_part, base + 8 + regs_b, exts, exts_b) != ESP_OK) {
        e = ESP_FAIL; goto out;
    }

    /* 完整性校验：没有程序 RAM 就回放会让 DSP 起不来，直接拒绝（在任何 I2C 操作之前） */
    {
        bool has_prog = false;
        for (uint32_t j = 0; j < ext_count; j++) {
            if (be16(exts + j * 8) >= ADAU1452_ADDR_PROG_BASE && rd32(exts + j * 8 + 4) > 0) {
                has_prog = true;
                break;
            }
        }
        if (!has_prog) {
            ESP_LOGE(TAG, "映像不含程序 RAM，拒绝回放（避免 DSP 停摆）");
            e = ESP_ERR_INVALID_STATE;
            goto out;
        }
    }

    ESP_LOGI(TAG, "开始回放槽 %d 的 DSP 程序：寄存器 %lu 个，内存区段 %lu 个，共 %lu 字节",
             idx, (unsigned long)reg_count, (unsigned long)ext_count, (unsigned long)plen);
    s_replay_busy = true;

    /* 记下第一个写过的普通寄存器，回放结束后回读校验。
     * ⚠️ 不要拿程序 RAM（≥0xC000）做校验：ADAU1452 的 PM **读回恒为 0**
     *    （实测 0xC000 读回全 0，而寄存器读回与映像逐字节一致），
     *    曾经误用导致每次开机都误报"校验不符"。 */
    rp_t rp = { .regs = regs, .count = reg_count };

    /* ---- 1. 软复位（R0/R1） ---- */
    /* 这两笔写失败说明 DSP 根本没在线（未上电 / 总线异常）。
     * 必须立即中止并返回错误，让上层（上电自动加载）稍后重试。 */
    if (adau1452_write_reg16(ADAU1452_REG_SOFT_RESET, 0x0000) != ESP_OK ||
        adau1452_write_reg16(ADAU1452_REG_SOFT_RESET, 0x0001) != ESP_OK) {
        ESP_LOGW(TAG, "回放中止：软复位写入失败（DSP 未在线？）");
        e = ESP_FAIL;
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(255));            /* R2_RESET_DELAY = 255ms */

    /* ---- 2. 唤醒 → 进休眠 → 释放/停核（R3..R7） ---- */
    adau1452_write_reg16(ADAU1452_REG_HIBERNATE, 0x0000);   /* R3: 先唤醒 */
    adau1452_write_reg16(ADAU1452_REG_HIBERNATE, 0x0001);   /* R4: 再进休眠 */
    vTaskDelay(pdMS_TO_TICKS(255));                         /* R5 = 255ms */
    adau1452_write_reg16(ADAU1452_REG_KILL_CORE, 0x0000);   /* R6 */
    adau1452_write_reg16(ADAU1452_REG_KILL_CORE, 0x0001);   /* R7: 停核 */

    /* ---- 3. PLL：先关 → 配置 → 再开 → 等锁定（R8..R14） ---- */
    adau1452_write_reg16(ADAU1452_REG_PLL_ENABLE, 0x0000);  /* R8: 关 PLL */
    if (!reg_present(regs, reg_count, ADAU1452_REG_PLL_CTRL0)) {
        ESP_LOGW(TAG, "  映像里没有 PLL 配置（0x%04X），冷启动可能起不来",
                 ADAU1452_REG_PLL_CTRL0);
    }
    rp_put_img(&rp, ADAU1452_REG_PLL_CTRL0,    0x0000);
    rp_put_img(&rp, ADAU1452_REG_PLL_CTRL1,    0x0000);
    rp_put_img(&rp, ADAU1452_REG_PLL_CLK_SRC,  0x0000);
    rp_put_img(&rp, ADAU1452_REG_MCLK_OUT,     0x0000);
    adau1452_write_reg16(ADAU1452_REG_PLL_ENABLE,                       /* R13 */
                         lookup_reg(regs, reg_count, ADAU1452_REG_PLL_ENABLE, 0x0001));
    vTaskDelay(pdMS_TO_TICKS(255));                         /* R14: 等 PLL 锁定 */

    /* ---- 4. PLL 锁定后才写：电源 / 时钟 / 音频串口（R15..R50，顺序照抄） ----
     * 电源与时钟先行，然后才是串口源与串口字节配置。 */
    {
        static const uint16_t head[] = {
            ADAU1452_REG_POWER_ENABLE0,
            ADAU1452_REG_POWER_ENABLE1,
            ADAU1452_REG_CLK_GEN3_M,
            ADAU1452_REG_MP7_MODE,
            ADAU1452_REG_MP11_MODE,
        };
        for (size_t i = 0; i < sizeof(head) / sizeof(head[0]); i++) {
            if (reg_present(regs, reg_count, head[i])) {
                rp_put_img(&rp, head[i], 0x0000);
            }
        }
        /* SOUT_SOURCE0..23 与 SERIAL_BYTE_1..7：地址升序恰好就是官方顺序 */
        for (uint16_t a = ADAU1452_REG_AUDIO_BASE; a <= ADAU1452_REG_AUDIO_END; a++) {
            if (reg_present(regs, reg_count, a)) {
                rp_put_img(&rp, a, 0x0000);
            }
        }
        /* 兜底：映像里其余寄存器按地址升序补写 */
        for (uint32_t i = 0; i < reg_count; i++) {
            const uint16_t a = be16(regs + i * 4);
            if (!replay_addr_handled(a)) {
                rp_put(&rp, a, be16(regs + i * 4 + 2));
            }
        }
    }
    if (rp.fail) {
        ESP_LOGW(TAG, "回放：共 %lu 个寄存器写入失败", (unsigned long)rp.fail);
    }
    vTaskDelay(pdMS_TO_TICKS(300));   /* 等时钟稳定后再灌内存 */

    /* ---- 4. 内存：PROGRAM / PARAM / DM ---- */
    const uint32_t data_base = (uint32_t)(8 + regs_b + exts_b);
    uint32_t data_off = data_base;
    uint8_t *buf = big_alloc(DIMG_FLASH_CHUNK);
    if (buf == NULL) {
        e = ESP_ERR_NO_MEM; goto out;
    }

    for (uint32_t j = 0; j < ext_count; j++) {
        /* ⚠️ 区段表字节序：base 是大端（便于人读 0xC000），words 是小端（wr32/rd32）
         * —— 二者必须与 walk_payload() 的写入方式严格一致，否则 words 会读成天文数字。 */
        uint16_t ebase = be16(exts + j * 8);
        uint32_t words = rd32(exts + j * 8 + 4);
        uint32_t bytes = words * 4;
        if (data_off + bytes > plen) {
            ESP_LOGE(TAG, "区段 %lu 越界", (unsigned long)j);
            e = ESP_ERR_INVALID_STATE;
            break;
        }
        ESP_LOGI(TAG, "  内存 0x%04X + %lu 字", ebase, (unsigned long)words);

        uint32_t done = 0;
        while (done < bytes) {
            uint32_t n = bytes - done;
            if (n > DIMG_FLASH_CHUNK) {
                n = DIMG_FLASH_CHUNK;
            }
            if (esp_partition_read(s_part, base + data_off + done, buf, n) != ESP_OK) {
                e = ESP_FAIL;
                break;
            }
            e = adau1452_write_block((uint16_t)(ebase + done / 4), buf, n);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "回放：内存 0x%04X 写入失败 @+%lu 字节",
                         ebase, (unsigned long)done);
                break;
            }
            done += n;
        }
        data_off += bytes;
        if (e != ESP_OK) {
            break;
        }
    }
    big_free(buf);

    if (e != ESP_OK) {
        ESP_LOGE(TAG, "回放失败：内存写入未完成");
        goto out;
    }

    /* ---- 4b. 内存写入校验：I2C ACK 不等于数据真的进了 RAM ---- */
    const esp_err_t mem_ver = verify_mem_written(exts, ext_count, base, data_base, plen);
    /* ---- 5. 启动核心（顺序与导出文件的 R54..R60 一致） ---- */
    {
        esp_err_t s1 = adau1452_write_reg16(ADAU1452_REG_KILL_CORE,
                         lookup_reg(regs, reg_count, ADAU1452_REG_KILL_CORE, 0x0000));
        esp_err_t s2 = adau1452_write_reg16(ADAU1452_REG_START_ADDRESS,
                         lookup_reg(regs, reg_count, ADAU1452_REG_START_ADDRESS, 0x0000));
        esp_err_t s3 = adau1452_write_reg16(ADAU1452_REG_START_PULSE,
                         lookup_reg(regs, reg_count, ADAU1452_REG_START_PULSE, 0x0003));
        esp_err_t s4 = adau1452_write_reg16(ADAU1452_REG_START_CORE, 0x0000);
        esp_err_t s5 = adau1452_write_reg16(ADAU1452_REG_START_CORE,
                         lookup_reg(regs, reg_count, ADAU1452_REG_START_CORE, 0x0001));
        if (s1 != ESP_OK || s2 != ESP_OK || s3 != ESP_OK || s4 != ESP_OK || s5 != ESP_OK) {
            ESP_LOGE(TAG, "回放失败：启动核心寄存器写入失败");
            e = ESP_FAIL;
            goto out;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    adau1452_write_reg16(ADAU1452_REG_HIBERNATE,
                         lookup_reg(regs, reg_count, ADAU1452_REG_HIBERNATE, 0x0000));

    /* 硬件状态（PLL/时钟是否锁定、核心是否运行）—— 判断"起来没有"的客观依据 */
    log_hw_status();

    /* ---- 6. 寄存器回读 + 记下见证寄存器 ----
     * 寄存器是可读的（程序 RAM 读回恒为 0，不能用来校验）。
     * 记下的 (addr, val) 之后会被健康巡检用来发现"程序被改写"。
     * ⚠️ 它只是**辅助**手段：实测有些寄存器会静默丢弃非法写入
     *    （0xF000 写 0/0x61 都读回 0x60），所以真正的依据是
     *    PLL_LOCK 与 CORE_STATUS（见 dsp_image_boot_tick 的巡检段）。 */
    if (rp.sig_set) {
        uint8_t rb[2] = { 0 };
        if (adau1452_read_block(rp.sig_addr, rb, sizeof(rb)) == ESP_OK) {
            const uint16_t got = be16(rb);
            if (got == rp.sig_val) {
                ESP_LOGI(TAG, "回读校验通过（寄存器 0x%04X = 0x%04X）", rp.sig_addr, got);
            } else {
                ESP_LOGW(TAG, "寄存器回读不符：0x%04X 期望 0x%04X，实读 0x%04X",
                         rp.sig_addr, rp.sig_val, got);
            }
        } else {
            ESP_LOGW(TAG, "寄存器回读失败，跳过校验");
        }
    }

    /* ---- 7. 判定 ----
     * ⚠️ 必须有**内存**校验通过才算成功。只回读一个刚写过的寄存器是不够的：
     * 1452 被抢占时可能寄存器写进去了、程序内存却没有，
     * 表现出来就是"推送成功但不出声"。
     */
    if (mem_ver != ESP_OK) {
        ESP_LOGE(TAG, "回放失败：内存未真正写入（I2C 有 ACK 但数据没进 RAM）");
        e = mem_ver;
        goto out;
    }

    /* 校验通过才算"这次推送有效"：刷新监控状态与见证寄存器 */
    if (rp.sig_set) {
        s_sig_addr = rp.sig_addr;
        s_sig_val = rp.sig_val;
        s_sig_valid = true;
    }
    s_dsp_online = true;
    s_off_cnt = 0;
    s_bad_cnt = 0;
    s_settle_tick = 0;

    ESP_LOGI(TAG, "DSP 程序回放完成（槽 %d）", idx);

out:
    s_replay_busy = false;
    big_free(regs);
    big_free(exts);
    return e;
}

/* 回放指定的一个槽位 */
esp_err_t dsp_image_slot_replay(int idx)
{
    if (idx < 0 || idx >= DIMG_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return dimg_replay_slot(idx);
}

/*
 * 默认回放入口：用「开机推送」选中的槽位；没选择（或选中的槽位已失效）时
 * 退回最新的一份 —— 这样网页上的「加载到 DSP」在没设置过时也能直接用。
 */
esp_err_t dsp_image_replay(void)
{
    int idx = s_boot_slot;
    if (idx < 0 || !s_slots[idx].valid) {
        idx = s_newest_slot;
    }
    if (idx < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return dimg_replay_slot(idx);
}

esp_err_t dsp_image_stream(dsp_image_sink_t sink, void *ctx)
{
    if (sink == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 优先导出当前 RAM 录制（可能还没保存到 flash） */
    if (img_lock()) {
        if (s_reg_count > 0 || s_ext_count > 0) {
            esp_err_t r = walk_payload(sink, ctx);
            img_unlock();
            return r;
        }
        img_unlock();
    }

    /* 否则从 flash 映像流式读出（用最新的一份） */
    if (s_part == NULL || s_newest_slot < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *buf = big_alloc(DIMG_FLASH_CHUNK);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = ESP_OK;
    uint32_t off = 0;
    while (off < s_newest_bytes) {
        uint32_t n = s_newest_bytes - off;
        if (n > DIMG_FLASH_CHUNK) {
            n = DIMG_FLASH_CHUNK;
        }
        e = esp_partition_read(s_part, slot_off(s_newest_slot) + DIMG_HDR_SIZE + off, buf, n);
        if (e != ESP_OK) {
            break;
        }
        e = sink(buf, n, ctx);
        if (e != ESP_OK) {
            break;
        }
        off += n;
    }
    big_free(buf);
    return e;
}

/* ================= 上电自动加载：持续监控 1452 =================
 *
 * 这**不是**一次性的开机动作，而是**一直**巡检 1452：
 *   ① 1452 常常**后于** ESP32 上电 → 必须等它上线再推
 *      （旧版本只在 app_main 里试一次，所以"没生效"）
 *   ② 1452 **下电再上电**后要自动重推（它自己不会自举，没固件就起不来）
 *      → 掉线 → 等待 → 再上线 → 推送
 *   ③ 如果 1452 里我们的程序被覆盖/挂掉（PLL 没锁、核心停了），也要自动重推
 *
 * 探测到上线后等 `settle_ms`（默认 500ms，网页可调）再推：
 *   以前这个等待是为了躲开 1452 自己的 EEPROM 自举；关掉 selfboot 后这个理由
 *   不存在了，而且推早了也有校验 + 自动重推兜底，所以默认值取很小。
 *   推送失败会在当前值上递增（DIMG_SETTLE_STEP_MS）重试，成功后回到配置值。
 */
static uint32_t boot_uptime_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void boot_set_state(dsp_boot_state_t st, const char *msg)
{
    s_boot_res.state = st;
    s_boot_res.uptime_ms = boot_uptime_ms();
    if (msg != NULL) {
        snprintf(s_boot_res.msg, sizeof(s_boot_res.msg), "%s", msg);
    }
}

/* 按当前的「开关 + 选中槽位」重新决定要不要监控。改配置后可立即生效，不必重启 */
static void boot_arm(void)
{
    if (!s_auto_load) {
        s_watch = false;
        boot_set_state(DSP_BOOT_IDLE, "未启用");
        return;
    }
    if (s_boot_slot < 0) {
        s_watch = false;
        s_boot_res.slot = -1;
        boot_set_state(DSP_BOOT_IDLE, "未选择要推送的固件（选「不推送」）");
        return;
    }
    s_boot_res.slot = s_boot_slot;
    if (!s_slots[s_boot_slot].valid) {
        s_watch = false;
        boot_set_state(DSP_BOOT_FAIL, "选定的槽位没有有效映像");
        ESP_LOGE(TAG, "上电自动加载：槽 %d 没有有效映像，不推送", s_boot_slot);
        return;
    }
    s_watch = true;
    if (s_dsp_online) {
        /* 1452 已经在线且里面就是我们推的程序：不动它，等下次它上线再说 */
        if (s_boot_res.state == DSP_BOOT_IDLE || s_boot_res.state == DSP_BOOT_FAIL) {
            boot_set_state(DSP_BOOT_OK, "1452 已在线");
        }
    } else {
        boot_set_state(DSP_BOOT_WAIT, "等待 1452 上线…");
    }
}

/* 用户显式改了"开哪一份"这类设置：把"已推送"标记作废，重新走一轮推送 */
static void boot_force_repush(void)
{
    s_dsp_online = false;
    s_sig_valid = false;
    s_settle_tick = 0;
    s_off_cnt = 0;
    s_bad_cnt = 0;
    s_boot_last_tick = 0;
}

void dsp_image_boot_begin(void)
{
    s_boot_last_tick = 0;
    s_dsp_online = false;
    s_off_cnt = 0;
    s_bad_cnt = 0;
    s_settle_tick = 0;
    s_paused = false;
    s_skip_push_once = false;
    s_sig_valid = false;
    s_push_count = 0;
    s_repush_cnt = 0;
    s_repush_warned = false;
    s_boot_res.attempts = 0;
    s_boot_res.slot = -1;
    boot_arm();

    if (s_watch) {
        ESP_LOGI(TAG, "上电自动加载：开，目标槽 %d（%lu 字节，%s）；"
                      "之后持续监控，1452 每次上线都会自动推送",
                 s_boot_slot, (unsigned long)s_slots[s_boot_slot].bytes,
                 s_names[s_boot_slot][0] ? s_names[s_boot_slot] : "未命名");
        dsp_image_boot_tick(true);   /* 先试一轮（在线的话这里就会起"稳定计时"） */
    }
}

void dsp_image_boot_tick(bool allow)
{
    if (!s_watch) {
        return;
    }
    if (dsp_image_busy()) {
        return;                      /* 正在回放（可能是网页点的手动加载），别插队 */
    }

    if (!allow) {
        s_paused = true;
        return;
    }
    if (s_paused) {
        /*
         * 从暂停（SigmaStudio 会话中）恢复。
         *
         * ⚠️ 这里**不能**顺手推一次：SigmaStudio 刚往 DSP 里下载了程序，
         * 我们一推就把它覆盖了。所以只把"在线"状态接过来，同时作废见证值
         * （见证值是旧程序的，接着比对会误判成"被改写"→ 又把 SigmaStudio
         * 的程序推掉）。1452 真的掉线重上时，下面照样会自动推。
         */
        s_paused = false;
        s_skip_push_once = true;
        s_sig_valid = false;
        s_boot_last_tick = 0;
        s_settle_tick = 0;
        ESP_LOGI(TAG, "桥接结束：本次不重复推送（避免覆盖 SigmaStudio 刚下载的程序）");
    }

    const TickType_t now = xTaskGetTickCount();
    const uint32_t period = s_dsp_online ? DIMG_ONLINE_MS : DIMG_PROBE_MS;
    if (s_boot_last_tick != 0 && (now - s_boot_last_tick) < pdMS_TO_TICKS(period)) {
        return;
    }
    s_boot_last_tick = now;

    const bool online = (adau1452_probe() == ESP_OK);

    /* ---------------- 还没认定它在线：等上线 → 稳定 → 推送 ---------------- */
    if (!s_dsp_online) {
        if (!online) {
            s_settle_tick = 0;
            s_skip_push_once = false;   /* 真的离线过，下次上线就该推 */
            if (s_boot_res.state != DSP_BOOT_WAIT) {
                boot_set_state(DSP_BOOT_WAIT, "等待 1452 上线…");
                ESP_LOGI(TAG, "上电自动加载：等待 1452 上线");
            }
            return;
        }

        /* --- 刚探测到：先让它稳定一下，别在它刚上电的瞬间就写 --- */
        if (s_settle_tick == 0) {
            s_settle_tick = now;
            boot_set_state(DSP_BOOT_SETTLE, "1452 已上线，等它就绪…");
            ESP_LOGI(TAG, "上电自动加载：探测到 1452 上线，等 %lu ms 后推送"
                          "（可在网页「DSP 程序映像」里调整）", (unsigned long)s_settle_ms);
            return;
        }
        const uint32_t waited_ms =
            (uint32_t)(now - s_settle_tick) * portTICK_PERIOD_MS;
        if (waited_ms < s_settle_ms) {
            return;
        }

        /*
         * ⚠️ 这里**不要**用"读回的主音量是否合理"来判断是否就绪。
         * 关掉 selfboot 后冷启动时 PARAM RAM 是未初始化的，读回必然是垃圾值
         * （实测见过 0x2B0C21E7 ≈ +43），那**不代表"还没准备好"，只代表
         * "还没有程序被装载" —— 而这正是我们该推送的时刻。
         * 所以只按固定稳定时长走（推送前的值会打日志，供排查用）。
         */

        if (s_skip_push_once) {
            /* 桥接刚结束、1452 一直没掉线：认定里面的程序是 SigmaStudio 的 */
            s_skip_push_once = false;
            s_dsp_online = true;
            s_sig_valid = false;
            boot_set_state(DSP_BOOT_OK, "1452 已在线（桥接刚结束，未重复推送）");
            return;
        }

        /* --- 推送 --- */
        s_boot_res.attempts++;
        s_boot_res.slot = s_boot_slot;
        {
            uint32_t v = 0;
            if (adau1452_read_param(ADAU_ADDR_MASTER_TARGET, &v) == ESP_OK) {
                ESP_LOGI(TAG, "上电自动加载：开始推送槽 %d 给 1452"
                              "（探测到上线后 %lu ms，推送前 master param = 0x%08lX）…",
                         s_boot_slot, (unsigned long)waited_ms, (unsigned long)v);
            } else {
                ESP_LOGI(TAG, "上电自动加载：开始推送槽 %d 给 1452"
                              "（探测到上线后 %lu ms）…",
                         s_boot_slot, (unsigned long)waited_ms);
            }
        }
        const esp_err_t e = dimg_replay_slot(s_boot_slot);
        s_boot_res.uptime_ms = boot_uptime_ms();
        if (e == ESP_OK) {
            s_push_count++;
            s_settle_ms = s_settle_cfg_ms;   /* 成功：回到配置值 */
            char m[80];
            snprintf(m, sizeof(m), "已推送槽 %d（%lu 字节）", s_boot_slot + 1,
                     (unsigned long)s_slots[s_boot_slot].bytes);
            boot_set_state(DSP_BOOT_OK, m);
            ESP_LOGI(TAG, "上电自动加载成功（本次开机第 %lu 次，耗时 %lu ms）：%s",
                     (unsigned long)s_push_count, (unsigned long)waited_ms, m);
        } else {
            char m[80];
            snprintf(m, sizeof(m), "推送失败（%s），稍后重试", esp_err_to_name(e));
            boot_set_state(DSP_BOOT_FAIL, m);
            ESP_LOGW(TAG, "上电自动加载：%s", m);
            /* 推早了（例如 1452 供电/时钟还没稳）→ 下次多等一点再试 */
            if (s_settle_ms < DIMG_SETTLE_LIMIT_MS) {
                s_settle_ms += DIMG_SETTLE_STEP_MS;
                if (s_settle_ms > DIMG_SETTLE_LIMIT_MS) {
                    s_settle_ms = DIMG_SETTLE_LIMIT_MS;
                }
                ESP_LOGW(TAG, "  下次改为等 %lu ms 再试", (unsigned long)s_settle_ms);
            }
            s_settle_tick = 0;          /* 重新走"稳定 → 重试" */
        }
        return;
    }

    /* ---------------- 已认定在线：巡检掉线 / 程序被改写 ---------------- */
    if (!online) {
        if (++s_off_cnt >= DIMG_OFF_COUNT) {
            s_dsp_online = false;
            s_off_cnt = 0;
            s_bad_cnt = 0;
            s_settle_tick = 0;
            s_sig_valid = false;
            boot_set_state(DSP_BOOT_WAIT, "1452 已离线，等它重新上线…");
            ESP_LOGW(TAG, "上电自动加载：1452 已离线，重新上线后会自动推送");
        }
        return;
    }
    s_off_cnt = 0;

    /* ---- 健康巡检：判断 1452 里我们的程序是否还在正常运行 ---- */
    bool bad = false;

    /* ① 见证寄存器：我们写进去的某个值被改掉，说明程序被覆盖过 */
    if (s_sig_valid) {
        uint8_t rb[2] = { 0 };
        if (adau1452_read_block(s_sig_addr, rb, sizeof(rb)) == ESP_OK &&
            be16(rb) != s_sig_val) {
            ESP_LOGW(TAG, "上电自动加载：见证寄存器 0x%04X 被改写"
                          "（实读 0x%04X，期望 0x%04X）",
                     s_sig_addr, (unsigned)be16(rb), (unsigned)s_sig_val);
            bad = true;
        }
    }

    /*
     * ② 更重要的是**直接看硬件状态**，而不是比对"我们写过的某个值"：
     *    实测有些寄存器会**静默丢弃**非法写入（例如 0xF000 写 0 或 0x61 都读回 0x60），
     *    所以"值对得上"并不等于程序在跑。而下面这两个反映的是真实状态：
     *      PLL_LOCK    = 0 → PLL 没锁 → 程序肯定没在运行
     *      CORE_STATUS = 0 → 核心已停 → 同上
     *    （实测：断开/软复位后为 0；正常运行时 PLL_LOCK=1、CORE_STATUS 在 1/3 间跳动，
     *      ——所以只判"是否等于 0"，不能和固定值比较。）
     */
    if (!bad) {
        uint8_t st[2];
        if (adau1452_read_block(ADAU1452_REG_PLL_LOCK, st, sizeof(st)) == ESP_OK &&
            be16(st) == 0) {
            ESP_LOGW(TAG, "上电自动加载：1452 的 PLL 未锁定（程序没在跑）");
            bad = true;
        } else if (adau1452_read_block(ADAU1452_REG_CORE_STATUS, st, sizeof(st)) == ESP_OK &&
                   be16(st) == 0) {
            ESP_LOGW(TAG, "上电自动加载：1452 的核心已停止（程序没在跑）");
            bad = true;
        }
    }

    if (!bad) {
        s_bad_cnt = 0;
        return;
    }
    if (++s_bad_cnt < DIMG_BAD_COUNT) {
        return;                          /* 连着两次才动手，防误判 */
    }
    s_bad_cnt = 0;

    if (s_repush_cnt >= DIMG_REPUSH_MAX) {
        if (!s_repush_warned) {
            s_repush_warned = true;
            ESP_LOGE(TAG, "上电自动加载：自动重推已达上限 %d 次，停止重推"
                          "（请检查 1452 供电 / 时钟 / 复位）", DIMG_REPUSH_MAX);
        }
        return;
    }
    s_repush_cnt++;
    ESP_LOGW(TAG, "上电自动加载：1452 里的程序没在运行，重新推送"
                  "（第 %lu 次自动重推）", (unsigned long)s_repush_cnt);
    s_dsp_online = false;
    s_settle_tick = 0;
    s_sig_valid = false;
    boot_set_state(DSP_BOOT_WAIT, "程序未运行，准备重新推送…");
}

bool dsp_image_autoload_pending(void)
{
    /* 监控开着但还没把固件推上去（等待中 / 稳定中 / 失败重试）→ 先别同步音量，
     * 否则窗口期里跑的还是 1452 自己的程序，音量会被写到错误的地址上。 */
    return s_watch && !s_dsp_online;
}

void dsp_image_boot_result(dsp_image_boot_result_t *out)
{
    if (out != NULL) {
        *out = s_boot_res;
    }
}

uint32_t dsp_image_push_count(void)
{
    return s_push_count;
}
