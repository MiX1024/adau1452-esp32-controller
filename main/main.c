#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "adau1452.h"
#include "st7789.h"
#include "ec11.h"
#include "ui.h"
#include "net.h"
#include "sigma_tcp.h"
#include "usbi.h"
#include "dsp_image.h"
#include "logbuf.h"
#include "eq_store.h"

static const char *TAG = "main";

/* 每格（detent）音量步进：0.5 dB；正交解码每格产生 4 个计数 */
#define VOLUME_STEP_DB          0.5f
#define COUNTS_PER_DETENT       4

/* 主音量按百分比步进：每格 0.5% */
#define MASTER_PERCENT_STEP     0.5f

/* 若编码器旋转方向与直觉相反，改为 0 */
#define ENCODER_DIR_INVERT      1

static volume_channel_t s_channels[4];

/*
 * ---------- 绑定：首页控件 ↔ SigmaStudio 模块 ----------
 *
 * 为什么要它：DSP 程序一改，音量/电平的地址就整体平移，硬编码地址必须重编固件。
 * 这里把每个控件保存成一个「名字 + 地址」的绑定项，名字部分（key = "cell|parameter"）
 * 用来在网页重新导入工程文件后**自动重新解析出新的地址**，地址部分供本固件直接用。
 *
 * 槽位：0..3 = 音量通道，4 = 左电平，5 = 右电平。
 * addr == 0 表示未绑定，退回下面的默认地址。
 */
static const uint16_t BIND_DEF_ADDR[NET_BIND_SLOTS] = {
    ADAU_ADDR_MASTER_TARGET, ADAU_ADDR_BASS_TARGET, ADAU_ADDR_TREBLE_TARGET,
    ADAU_ADDR_MID_TARGET,    ADAU_ADDR_LEVEL_L,     ADAU_ADDR_LEVEL_R,
};
static const char *const BIND_DEF_NAME[NET_BIND_SLOTS] = {
    "MASTER", "LOW", "HIGH", "MID", "LEVEL L", "LEVEL R",
};
/*
 * 默认绑定的来源 key（"cell|parameter"）。
 *
 * 为什么要有它：换 DSP 程序后地址会整体平移，网页端 bindAutoResolve()
 * 靠这个 key 在新导出的 .params 里按**名字**重新查到地址。如果 key 为空，
 * 默认绑定就永远退回下面硬编码的旧地址，换固件后首页音量/电平全部失配。
 *
 * 注意：这里的 cell 名必须和 SigmaStudio 导出的 *.params 里「Cell Name」
 * 严格一致（如 "Single 1"），参数名和「Parameter Name」一致。
 */
static const char *const BIND_DEF_KEY[NET_BIND_SLOTS] = {
    "Single 1|HWGainADAU145XAlg1target",
    "Single 1_7|HWGainADAU145XAlg7target",
    "Single 1_5|HWGainADAU145XAlg5target",
    "Single 1_2|HWGainADAU145XAlg2target",
    "Level1|SingleBandLevelDetS3001X",
    "Level1_2|SingleBandLevelDetS3002X",
};

/* 音量通道的固定属性（名、地址、范围在 bind_apply 里再覆盖） */
static const volume_channel_t CH_DEF[4] = {
    /* 主音量：0% = -96 dB，100% = 0 dB，120% = +10 dB */
    {"MASTER", ADAU_ADDR_MASTER_TARGET,  -6.0f, -96.0f, 10.0f, true},
    {"LOW",    ADAU_ADDR_BASS_TARGET,     0.0f, -40.0f, 20.0f, false},
    {"HIGH",   ADAU_ADDR_TREBLE_TARGET,   0.0f, -40.0f, 20.0f, false},
    {"MID",    ADAU_ADDR_MID_TARGET,      2.0f, -40.0f, 20.0f, false},
};

static net_bind_slot_t s_bind[NET_BIND_SLOTS];
static bool s_bind_loaded = false;

static uint16_t bind_addr(int i)
{
    if (i < 0 || i >= NET_BIND_SLOTS) {
        return 0;
    }
    return s_bind[i].addr ? s_bind[i].addr : BIND_DEF_ADDR[i];
}

static void bind_defaults(void)
{
    memset(s_bind, 0, sizeof(s_bind));
    for (int i = 0; i < NET_BIND_SLOTS; i++) {
        snprintf(s_bind[i].name, sizeof(s_bind[i].name), "%s", BIND_DEF_NAME[i]);
        snprintf(s_bind[i].key, sizeof(s_bind[i].key), "%s", BIND_DEF_KEY[i]);
    }
}

/* 把绑定落到 s_channels（名字 + 地址） */
static void bind_apply(void)
{
    for (int i = 0; i < 4; i++) {
        s_channels[i] = CH_DEF[i];
        s_channels[i].addr = bind_addr(i);
        /* name 指向绑定项里的缓冲，所以网页改名后屏幕也会跟着变 */
        s_channels[i].name = s_bind[i].name;
    }
}

/* bind_load / bind_save 依赖 NVS 状态，定义在下面 NVS 小节里 */
static void bind_load(void);
static void bind_save(void);


/*
 * ---------- 安全模式 ----------
 *
 * 场景：用户用 SigmaStudio 重新编译下载了 1452 程序后，ESP32 里还存着
 * 旧固件的参数地址。此时若 ESP32 上电按「上次保存的音量」写 DSP，就会写到
 * 旧地址上 —— 那已经不是音量参数了，轻则爆音、重则让 1452 崩溃。
 *
 * 安全模式开启后：上电时 ESP32 **绝不主动写** DSP 参数（音量、EQ 都只读回），
 * 只把 DSP 当前值读回来当显示基线。用户就可以放心用 SigmaStudio 调程序。
 *
 * 关闭安全模式时，会置位 s_safe_fresh：下一次上电写「固件里的默认值」
 * （而不是 NVS 保存值），因为新固件已经烧进 ESP32、地址是新的，默认值也安全。
 * 再之后的上电恢复正常：写 NVS 保存值。
 */
static bool s_safe_mode = false;   /* 安全模式开关（NVS "safe"/on） */
static bool s_safe_fresh = false;  /* 关闭安全模式后置位：下次上电写固件默认值 */

/* bind_load / bind_save 依赖 NVS 状态，定义在下面 NVS 小节里 */


static bool s_dsp_ok = false;

/* 主循环与网络任务共享的状态（float 32 位读写原子；I2C 驱动内部已串行化） */
static int s_sel = 0;
static float s_lv_l = -999.0f;
static float s_lv_r = -999.0f;
static bool s_nvs_dirty = false;
static TickType_t s_last_adj_tick = 0;

/* ---------- NVS 断电保存 ---------- */
static nvs_handle_t s_nvs = 0;
static bool s_nvs_ok = false;

/* 从 NVS 加载上次保存的音量；返回是否有保存过数据 */
static bool load_volumes_from_nvs(void)
{
    esp_err_t err = nvs_open("volume", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    s_nvs_ok = true;

    bool any = false;
    for (int i = 0; i < 4; i++) {
        char key[8];
        snprintf(key, sizeof(key), "vol%d", i);
        int32_t db10 = 0;
        if (nvs_get_i32(s_nvs, key, &db10) == ESP_OK) {
            s_channels[i].db = (float)db10 / 10.0f;
            any = true;
            ESP_LOGI(TAG, "NVS load %s = %.1f dB", s_channels[i].name, s_channels[i].db);
        }
    }
    return any;
}

/* 保存当前音量到 NVS（写入 flash，仅在停止调节后调用以延长寿命） */
static void save_volumes_to_nvs(void)
{
    if (!s_nvs_ok) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        char key[8];
        snprintf(key, sizeof(key), "vol%d", i);
        nvs_set_i32(s_nvs, key, (int32_t)lroundf(s_channels[i].db * 10.0f));
    }
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "volumes saved to NVS");
}

/* ---------- 绑定：NVS 读写 ---------- */
static void bind_load(void)
{
    bind_defaults();
    nvs_handle_t h;
    if (nvs_open("bind", NVS_READONLY, &h) != ESP_OK) {
        bind_apply();
        return;
    }
    size_t sz = sizeof(s_bind);
    /* ⚠️ 用 blob 一整块存取：6 个槽位一起生效，不会出现"改了一半"的中间状态 */
    if (nvs_get_blob(h, "map", s_bind, &sz) == ESP_OK && sz == sizeof(s_bind)) {
        s_bind_loaded = true;
        /*
         * 迁移：旧版本保存的绑定里 key 是空的（只有名字+地址）。
         * 补上默认 key，否则换 DSP 程序后网页端 bindAutoResolve()
         * 找不到名字可查，首页会一直用旧地址。
         */
        bool key_migrated = false;
        for (int i = 0; i < NET_BIND_SLOTS; i++) {
            if (s_bind[i].key[0] == '\0') {
                snprintf(s_bind[i].key, sizeof(s_bind[i].key), "%s", BIND_DEF_KEY[i]);
                key_migrated = true;
            }
        }
        ESP_LOGI(TAG, "绑定已加载：%s(%u) %s(%u) %s(%u) %s(%u) / 电平 %u,%u",
                 s_bind[0].name, s_bind[0].addr, s_bind[1].name, s_bind[1].addr,
                 s_bind[2].name, s_bind[2].addr, s_bind[3].name, s_bind[3].addr,
                 s_bind[4].addr, s_bind[5].addr);
        nvs_close(h);
        if (key_migrated) {
            /* 把补上的 key 写回，避免下次启动再迁移一次 */
            bind_save();
            ESP_LOGI(TAG, "已为绑定补上默认 key（cell|parameter）");
        }
    } else {
        nvs_close(h);
    }
    bind_apply();
}

static void bind_save(void)
{
    nvs_handle_t h;
    if (nvs_open("bind", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "map", s_bind, sizeof(s_bind)) == ESP_OK) {
        nvs_commit(h);
        s_bind_loaded = true;
    }
    nvs_close(h);
}

/* ---------- 安全模式：NVS 读写 ---------- */
static void safe_mode_load(void)
{
    nvs_handle_t h;
    if (nvs_open("safe", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8 = 0;
    if (nvs_get_u8(h, "on", &u8) == ESP_OK) {
        s_safe_mode = (u8 != 0);
    }
    if (nvs_get_u8(h, "fresh", &u8) == ESP_OK) {
        s_safe_fresh = (u8 != 0);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "安全模式：%s%s", s_safe_mode ? "开" : "关",
             s_safe_fresh ? "（下次上电写默认值）" : "");
}

static void safe_mode_save(void)
{
    nvs_handle_t h;
    if (nvs_open("safe", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "on", s_safe_mode ? 1 : 0);
    nvs_set_u8(h, "fresh", s_safe_fresh ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* 把 4 个音量通道重置为固件默认值（CH_DEF 里的初始 dB） */
static void reset_volumes_to_defaults(void)
{
    for (int i = 0; i < 4; i++) {
        s_channels[i].db = CH_DEF[i].db;
    }
}

/* 音量与 DSP 同步：
 * - 有 NVS 保存值：把保存值写回 DSP（覆盖 DSP 当前值）
 * - 无保存值（首次上电）：从 DSP 读取当前值作为初始值 */
static void sync_volumes_with_dsp(bool has_saved)
{
    if (has_saved) {
        for (int i = 0; i < 4; i++) {
            adau1452_set_volume_db(s_channels[i].addr, s_channels[i].db);
        }
        ESP_LOGI(TAG, "restored volumes from NVS to DSP");
    } else {
        for (int i = 0; i < 4; i++) {
            float db = adau1452_read_volume_db(s_channels[i].addr);
            if (db > -200.0f) {
                s_channels[i].db = db;
            }
        }
        ESP_LOGI(TAG, "read initial volumes from DSP");
    }
}

/*
 * 把本地保存的参数整体推回 DSP：音量 + EQ。
 *
 * 为什么必须一起做：EQ 系数存在设备 NVS 里，但 DSP 里的值会在两种情况下被冲掉 ——
 *   ① 1452 断电重启（除非固件映像自动重推，否则它就是空的）
 *   ② 固件映像被自动重推（映像里带的系数是**录制那一刻**的值）
 * 所以每次"DSP 刚刚就绪"或"刚推送完映像"都要重放一遍，否则用户会发现
 * "调好的 EQ 没了"。
 *
 * ⚠️ 调用方必须保证此刻允许访问 I2C（本地控制开着、桥接没开、映像不忙）。
 */
static void sync_dsp_params(bool has_saved_vol)
{
    if (s_safe_mode) {
        /*
         * 安全模式：上电绝不写 DSP。只把当前值读回来当显示基线。
         * 音量、EQ 都不写 —— 它们写的都是旧固件地址，写进去会爆音/崩溃。
         */
        sync_volumes_with_dsp(false);
        ESP_LOGI(TAG, "安全模式开启：跳过音量/EQ 写入，仅读回 DSP 基线");
        return;
    }

    if (s_safe_fresh) {
        /*
         * 刚关闭安全模式的第一次上电：写「固件默认值」而非 NVS 保存值。
         * 此时 ESP32 里已是新固件、地址是新的，默认值安全；写完后这组默认值
         * 成为新的 NVS 基线，之后的上电恢复正常（写保存值）。
         */
        reset_volumes_to_defaults();
        for (int i = 0; i < 4; i++) {
            adau1452_set_volume_db(s_channels[i].addr, s_channels[i].db);
        }
        s_safe_fresh = false;
        safe_mode_save();
        s_nvs_dirty = true;
        s_last_adj_tick = xTaskGetTickCount();   /* 2 秒后把默认值存进 NVS */
        ESP_LOGI(TAG, "安全模式已关闭，首次上电写入固件默认音量");
        return;
    }

    sync_volumes_with_dsp(has_saved_vol);
    eq_store_apply();
}

/* 统一音量应用入口：钳位 + 写 DSP + 标记 NVS 待保存。编码器与网络共用。 */
static bool apply_volume(int idx, float db)
{
    /*
     * 安全模式：本地一律不写 DSP。转编码器 / 拖网页滑块都会走到这里，
     * 若放行就会写到旧固件地址上爆音，所以直接拒绝。
     */
    if (s_safe_mode) {
        return false;
    }
    /*
     * 刚关闭安全模式时 s_safe_fresh 还挂着（等"下次上电写默认值"）。
     * 若用户在这之前就主动调了音量，说明他已经手动接管 —— 取消"写默认值"，
     * 之后的上电写这个保存值即可。
     */
    if (s_safe_fresh) {
        s_safe_fresh = false;
        safe_mode_save();
    }
    volume_channel_t *ch = &s_channels[idx];
    if (db > ch->max_db) {
        db = ch->max_db;
    }
    if (db < ch->min_db) {
        db = ch->min_db;
    }
    ch->db = db;
    bool ok = (adau1452_set_volume_db(ch->addr, ch->db) == ESP_OK);
    s_dsp_ok = ok;
    s_nvs_dirty = true;
    s_last_adj_tick = xTaskGetTickCount();
    return ok;
}

/* ---------- 网络回调（net.c 通过函数指针调用，运行于 httpd 任务） ---------- */
static bool app_set_volume_db(int idx, float db)
{
    if (idx < 0 || idx >= 4) {
        return false;
    }
    /*
     * ⚠️ 桥接/USBi 开启时**绝不能再写 DSP**。
     *
     * 原因不只是"让位"：SigmaStudio 一旦重编过程序，本地硬编码的地址
     * （29/226/164/161）就可能指向完全不同的参数，写进去会改错东西甚至
     * 破坏正在调试的程序。所以这里直接拒绝，不只靠主循环不调用。
     */
    if (sigma_tcp_get_config().enabled || usbi_enabled()) {
        return false;
    }
    return apply_volume(idx, db);
}

static void app_get_state(float db[4], float min_db[4], float max_db[4],
                          int *sel, bool *dsp_ok, float *lv_l, float *lv_r)
{
    for (int i = 0; i < 4; i++) {
        db[i] = s_channels[i].db;
        min_db[i] = s_channels[i].min_db;
        max_db[i] = s_channels[i].max_db;
    }
    *sel = s_sel;
    /*
     * 桥接期间我们**完全不访问 I2C**（本地控制让位），本地读不到电平，
     * 所以 s_dsp_ok 不能反映真实情况。若不修正，网页上的 DSP 状态会在
     * "SigmaStudio 调试中" 与 "DSP 离线" 之间反复跳。
     * SigmaStudio 正连着，就说明 DSP 显然在线。
     */
    *dsp_ok = s_dsp_ok || sigma_tcp_busy() || usbi_busy();
    *lv_l = s_lv_l;
    *lv_r = s_lv_r;
}

static const char *app_channel_name(int idx)
{
    if (idx < 0 || idx >= 4) {
        return "";
    }
    return s_channels[idx].name;
}

static bool app_channel_is_percent(int idx)
{
    if (idx < 0 || idx >= 4) {
        return false;
    }
    return s_channels[idx].show_percent;
}

/* ---------- 绑定：网络回调 ---------- */
static void app_bind_get(net_bind_slot_t out[NET_BIND_SLOTS])
{
    for (int i = 0; i < NET_BIND_SLOTS; i++) {
        out[i] = s_bind[i];
        /* 把"实际生效的地址"一并给出去，网页上就能看到默认绑定是什么 */
        if (out[i].addr == 0) {
            out[i].addr = BIND_DEF_ADDR[i];
        }
    }
}

static bool app_bind_set(const net_bind_slot_t in[NET_BIND_SLOTS])
{
    /* 桥接/USBi 开着时 DSP 由 SigmaStudio 管（地址随时可能变），不允许改绑定 */
    if (sigma_tcp_get_config().enabled || usbi_enabled()) {
        ESP_LOGW(TAG, "桥接/USBi 开启，拒绝修改绑定");
        return false;
    }
    for (int i = 0; i < NET_BIND_SLOTS; i++) {
        s_bind[i].addr = in[i].addr;
        strncpy(s_bind[i].name, in[i].name, sizeof(s_bind[i].name) - 1);
        s_bind[i].name[sizeof(s_bind[i].name) - 1] = '\0';
        strncpy(s_bind[i].key, in[i].key, sizeof(s_bind[i].key) - 1);
        s_bind[i].key[sizeof(s_bind[i].key) - 1] = '\0';
        if (s_bind[i].name[0] == '\0') {
            snprintf(s_bind[i].name, sizeof(s_bind[i].name), "%s", BIND_DEF_NAME[i]);
        }
    }
    bind_apply();
    bind_save();

    /*
     * 改了地址之后，通道里的 dB 值本来属于「上一个地址」，对新地址没有意义。
     * 所以从新地址读一次作为新基线 —— 网页滑块随后显示的才是 DSP 的真实状态。
     */
    if (adau1452_probe() == ESP_OK) {
        for (int i = 0; i < 4; i++) {
            float db = adau1452_read_volume_db(s_channels[i].addr);
            if (db > -200.0f) {
                if (db > s_channels[i].max_db) db = s_channels[i].max_db;
                if (db < s_channels[i].min_db) db = s_channels[i].min_db;
                s_channels[i].db = db;
            }
        }
        s_nvs_dirty = true;
        s_last_adj_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "绑定已更新，已从新地址回读基线值");
    }
    return true;
}

/* ---------- 安全模式：网络回调 ---------- */
static bool app_safe_mode_get(void)
{
    return s_safe_mode;
}

static bool app_safe_mode_set(bool on)
{
    if (on) {
        /* 开启：上电不再写任何 DSP 参数（下次重启生效） */
        s_safe_mode = true;
        s_safe_fresh = false;
        ESP_LOGI(TAG, "安全模式已开启：下次上电不再写 DSP 参数");
    } else {
        /* 关闭：置位 fresh，让下次上电写「固件默认值」而非保存值 */
        s_safe_mode = false;
        s_safe_fresh = true;
        ESP_LOGI(TAG, "安全模式已关闭：下次上电写入固件默认音量");
    }
    safe_mode_save();
    return true;
}

void app_main(void)
{
    /* 最先安装日志钩子：这样网页能看到的日志尽可能完整 */
    logbuf_init();

    ESP_LOGI(TAG, "ADAU1452 2.1 controller starting");

    /* 初始化 NVS 并加载上次保存的音量 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupt, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
    /*
     * 安全模式开关先读出来：sync_dsp_params() 要按它决定写不写 DSP。
     * 它只读 NVS、不碰 I2C，放在这里（音量加载前）最合适。
     */
    safe_mode_load();
    /*
     * 绑定必须先于音量：bind_apply() 会把 s_channels 重置成默认范围，
     * 之后再让 load_volumes_from_nvs() 把 NVS 里的 dB 值填进来。
     * 顺序反了会把刚读出来的音量冲掉。
     */
    bind_load();
    bool has_saved = load_volumes_from_nvs();
    /* 恢复上次保存的 EQ 系数（真正写回 DSP 在下面 sync_dsp_params 里） */
    eq_store_init();

    /* 初始化显示（先初始化，便于后续报错可见） */
    ret = st7789_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "st7789_init failed: %s", esp_err_to_name(ret));
    }

    /* 初始化 DSP */
    ret = adau1452_init();
    s_dsp_ok = (ret == ESP_OK);
    if (!s_dsp_ok) {
        ESP_LOGW(TAG, "ADAU1452 init failed, will keep retrying");
    }

    /*
     * USBi（USB 直连 SigmaStudio）：从 NVS 读开关；上次开着的话直接把
     * ESP32-S3 原生 USB 安装成 USBi 设备。
     * ⚠️ 必须在 nvs_flash_init() 之后调用；USB 与 UART 互不影响。
     */
    if (usbi_init() != ESP_OK) {
        ESP_LOGW(TAG, "USBi 初始化失败（不影响本地控制）");
    }

    /* 初始化编码器 */
    ec11_init();

    /* ---------- 程序映像 ---------- */
    /* 只读 NVS 配置（不建 socket：此时 lwIP 还没初始化，建 socket 会 assert） */
    sigma_tcp_config_load();
    dsp_image_init();

    /*
     * 上电自动加载 DSP 程序：开关、选哪一份、回放、重试、健康巡检全部封装在
     * dsp_image 模块里，这里只声明一下。
     *
     * ⚠️ 它是**长期在跑**的（不是开机那一下）：1452 常常后于 ESP32 上电，
     *    所以靠主循环里的 dsp_image_boot_tick() 持续监控——
     *    1452 每次上线（含断电重启）都会自动推送。
     */
    dsp_image_boot_begin();

    /* 音量初始化策略：
     * - 有 NVS 保存值：上电后将保存值写入 DSP（覆盖 DSP 当前值）
     * - 无保存值（首次上电）：从 DSP 读取当前值作为初始值
     * DSP 可能比 ESP32 上电慢（或后上电）：若启动时未就绪，交由主循环
     * 周期重试探测，就绪后再执行同步（见 volumes_synced 标志）。
     * ⚠️ 本地控制关闭时（程序由 SigmaStudio 下载，地址可能已变），连这次同步也要跳过，
     *    否则会把 NVS 里的音量写到错误地址。 */
    const bool boot_local_ok = sigma_tcp_get_config().local_control;
    bool volumes_synced = false;
    if (usbi_enabled()) {
        /* USBi 模式：DSP 由 PC 上的 SigmaStudio 管，本地绝不能抢先写 */
        volumes_synced = true;
        ESP_LOGW(TAG, "USBi 已开启，跳过上电音量同步（DSP 由 SigmaStudio 管理）");
    } else if (s_dsp_ok && boot_local_ok && !dsp_image_autoload_pending()) {
        sync_dsp_params(has_saved);
        volumes_synced = true;
    } else if (!boot_local_ok) {
        volumes_synced = true;   /* 让位模式：不做任何音量同步 */
        ESP_LOGW(TAG, "本地控制已关闭，跳过音量同步（DSP 程序由 SigmaStudio 管理）");
    }

    /* DSP 就绪重试：DSP 后上电时，周期性探测直到在线再同步音量 */
    TickType_t last_dsp_retry_tick = 0;
    /* EQ 补推节流 */
    TickType_t last_eq_retry_tick = 0;
    /* 已处理过的推送次数（见主循环里的重放逻辑） */
    uint32_t push_seen = dsp_image_push_count();

    /* 初始化 UI（内部维护电平平滑、峰值保持、刷新快照状态） */
    ui_init();

    /* 初始化网络（WiFi + HTTP + WebSocket + OTA），注册控制回调 */
    net_callbacks_t cb = {
        .set_volume_db = app_set_volume_db,
        .get_state = app_get_state,
        .channel_name = app_channel_name,
        .channel_is_percent = app_channel_is_percent,
        .bind_get = app_bind_get,
        .bind_set = app_bind_set,
        .safe_get = app_safe_mode_get,
        .safe_set = app_safe_mode_set,
    };
    net_init(&cb);

    /* 网络栈就绪后再启动 SigmaStudio 桥接监听
     * （socket 依赖 lwIP 已初始化，必须放在 net_init 之后） */
    sigma_tcp_init();

    /*
     * 兜底：USBi 与 TCP 桥接互斥。若 NVS 里两个开关都还开着（例如上次异常掉电
     * 时的中间状态），启动时以 USBi 为准，把 TCP 桥接关掉，避免两边同时抢 I2C。
     */
    if (usbi_enabled() && sigma_tcp_get_config().enabled) {
        sigma_tcp_config_t cfg0 = sigma_tcp_get_config();
        sigma_tcp_set_config(false, cfg0.port, cfg0.local_control, cfg0.resp_gap_us);
        ESP_LOGW(TAG, "USBi 已开启：启动时自动关闭 TCP 桥接（两者互斥）");
    }

    while (1) {
        /*
         * 上电自动加载的监控心跳：等 1452 上线 → 推送 → 巡检重建。
         * SigmaStudio 正连着时先让位（allow=false），等它断开再继续。
         */
        dsp_image_boot_tick(!sigma_tcp_busy() && !usbi_enabled());

        /*
         * ⚠️ 固件刚被（重新）推送时，DSP 里的参数是**录制时的初值**
         * （映像里带的就是那些值），必须把本地保存的音量重放一遍，
         * 否则音量会"跳回"录制时的值 —— 1452 断电重启后自动重推时尤其明显。
         * 这一步**不能**用 dsp_access 判断：桥接服务开着时 dsp_access 是 false，
         * 但推送仍然会发生（推送只要求当前没有 SigmaStudio 客户端连接）。
         */
        if (sigma_tcp_get_config().local_control && !usbi_enabled()) {
            const uint32_t pc = dsp_image_push_count();
            if (pc != push_seen) {
                push_seen = pc;
                vTaskDelay(pdMS_TO_TICKS(300));   /* 等 DSP 时钟/PLL 稳定 */
                sync_dsp_params(has_saved);
                volumes_synced = true;
                ESP_LOGI(TAG, "固件推送后已重放本地音量与 EQ（第 %lu 次推送）",
                         (unsigned long)pc);
            }
        }

        /*
         * ⚠️ 本地/桥接的模式切换依据是「启用桥接服务」这个**开关**，
         * 而不是"当前有没有客户端连接"。
         *
         * 这样语义清晰且可预期：开关一开 = 把 DSP 交给 SigmaStudio 管，
         * 本地电平与音量立即让位；开关一关 = 本地控制接管。
         * 若按"有没有连接"来切，状态会随 SigmaStudio 连/断反复跳，
         * 而且网页与屏幕容易显示不一致（两者判断口径不同就会出现）。
         */
        const bool bridge_on = sigma_tcp_get_config().enabled;
        const bool usbi_on = usbi_enabled();
        const bool local_ctl = sigma_tcp_get_config().local_control;
        /* 任一 SigmaStudio 通道（TCP 桥接 / USB USBi）开启，本地就让位 */
        const bool sig_on = bridge_on || usbi_on;
        const bool dsp_access = local_ctl && !sig_on && !dsp_image_busy();

        if (dsp_access) {
            /* DSP 上电可能晚于 ESP32：未完成音量同步时每 500ms 重试探测一次。
             * ⚠️ 若上电自动加载还没完成，必须等它先把程序灌进去 ——
             *    否则本地音量会被写到「上一份程序」的地址上。 */
            if (!volumes_synced && !dsp_image_autoload_pending() &&
                (xTaskGetTickCount() - last_dsp_retry_tick) >= pdMS_TO_TICKS(500)) {
                last_dsp_retry_tick = xTaskGetTickCount();
                if (adau1452_probe() == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    sync_dsp_params(has_saved);
                    volumes_synced = true;
                    s_dsp_ok = true;
                    ESP_LOGI(TAG, "DSP ready after retry, volumes+EQ synced");
                }
            }

            /*
             * EQ 的自愈：如果保存表里有 N 段但只有 M<N 段真正写进 DSP
             * （刚上电时 DSP 还没稳、或某一段恰好写失败），每 3 秒补推一次。
             * 不做这件事的话，那一段会一直"看着已保存、实际没生效"。
             */
            if (!s_safe_mode && !dsp_image_autoload_pending() &&
                (xTaskGetTickCount() - last_eq_retry_tick) >= pdMS_TO_TICKS(3000)) {
                last_eq_retry_tick = xTaskGetTickCount();
                if (eq_store_count() > 0 && eq_store_applied() < eq_store_count()) {
                    ESP_LOGW(TAG, "EQ 未完全写回（%d/%d），重试",
                             eq_store_applied(), eq_store_count());
                    eq_store_apply();
                }
            }

            /* 处理编码器旋转 */
            int32_t count = ec11_read_and_clear();
#if ENCODER_DIR_INVERT
            count = -count;
#endif
            if (count != 0) {
                volume_channel_t *ch = &s_channels[s_sel];
                float target;
                if (ch->show_percent) {
                    /* 主音量：按百分比步进（每格 0.5%），再反算回 dB */
                    float pct = ui_db_to_percent_float(ch->db);
                    pct += (float)count * (MASTER_PERCENT_STEP / (float)COUNTS_PER_DETENT);
                    if (pct > 120.0f) {
                        pct = 120.0f;
                    }
                    if (pct < 0.0f) {
                        pct = 0.0f;
                    }
                    target = ui_percent_to_db(pct);
                } else {
                    /* 其余通道：按 dB 步进 */
                    target = ch->db + (float)count * (VOLUME_STEP_DB / (float)COUNTS_PER_DETENT);
                }
                apply_volume(s_sel, target);
            }

            /* 处理编码器按键：切换选中通道 */
            if (ec11_button_pressed()) {
                s_sel = (s_sel + 1) % 4;
                ESP_LOGI(TAG, "selected: %s (%.1f dB)", s_channels[s_sel].name, s_channels[s_sel].db);
            }

            /* 读取左右声道电平（地址来自绑定表，改程序后由网页重新绑定） */
            s_lv_l = adau1452_read_level_db(bind_addr(4));
            s_lv_r = adau1452_read_level_db(bind_addr(5));
            if (s_lv_l > -200.0f && s_lv_r > -200.0f) {
                s_dsp_ok = true;
            } else {
                s_dsp_ok = false;
            }
        } else {
            /* 让位模式：丢弃编码器输入，电平按静音处理，完全不访问 I2C */
            ec11_read_and_clear();
            ec11_button_pressed();
            s_lv_l = -999.0f;
            s_lv_r = -999.0f;
        }

        /*
         * 渲染 UI。
         * 任一 SigmaStudio 通道（TCP 桥接 / USB USBi）开着就整屏切成"调试页"：
         * 显示链路与转发状态 + 本机网络信息；关掉后自动切回电平表页。
         */
        ui_dsp_state_t ui_state;
        ui_bridge_info_t brinfo;
        const ui_bridge_info_t *brp = NULL;
        if (sig_on) {
            ui_state = UI_DSP_BRIDGE;

            memset(&brinfo, 0, sizeof(brinfo));
            if (usbi_on) {
                /* USB（USBi）模式：没有 IP/端口，改为显示 USB 枚举/驱动状态 */
                usbi_status_t us;
                usbi_get_status(&us);
                brinfo.is_usb        = true;
                brinfo.usb_mounted   = us.mounted;
                brinfo.usb_configured = us.configured;
                brinfo.usb_format    = us.last_format;
                brinfo.clients       = us.configured ? 1 : 0;
                brinfo.pkt_write     = us.pkt_write;
                brinfo.pkt_read      = us.pkt_read;
                brinfo.last_addr     = us.last_addr;
                brinfo.last_len      = us.last_len;
                brinfo.last_is_read  = us.last_is_read;
                brinfo.lat_avg_us    = us.last_us;
                brinfo.errors        = us.errors;
            } else {
                sigma_tcp_status_t st;
                sigma_tcp_get_status(&st);
                strncpy(brinfo.client_ip, st.client_ip, sizeof(brinfo.client_ip) - 1);
                brinfo.port        = st.port;
                brinfo.clients     = (uint8_t)st.active;
                brinfo.pkt_write   = st.pkt_write;
                brinfo.pkt_read    = st.pkt_read;
                brinfo.last_addr   = st.last_addr;
                brinfo.last_len    = st.last_len;
                brinfo.last_is_read = st.last_is_read;
                brinfo.lat_avg_us  = st.lat_avg_us;
                brinfo.errors      = st.errors;
            }

            net_link_info_t li;
            net_get_link_info(&li);
            strncpy(brinfo.local_ip, li.ip, sizeof(brinfo.local_ip) - 1);
            brinfo.rssi    = li.rssi;
            brinfo.channel = li.channel;

            brp = &brinfo;
        } else if (!local_ctl) {
            /*
             * 本地控制被关掉了。这时我们不访问 I2C，电平表没有数据源，
             * 但 DSP 本身是好的 —— 必须与"真的掉线"区分开，
             * 否则屏幕显示 "1452 OFFLINE" 会让人以为 DSP 掉了。
             */
            ui_state = UI_DSP_LOCAL_OFF;
        } else if (s_dsp_ok) {
            ui_state = UI_DSP_ONLINE;
        } else {
            ui_state = UI_DSP_OFFLINE;
        }
        ui_render(s_channels, s_sel, ui_state, net_wifi_connected(), s_lv_l, s_lv_r, brp);

        /* 停止调节超过 2 秒后保存到 NVS */
        if (s_nvs_dirty && (xTaskGetTickCount() - s_last_adj_tick) > pdMS_TO_TICKS(2000)) {
            save_volumes_to_nvs();
            s_nvs_dirty = false;
        }

        /* 向 Web 客户端推送状态（订阅了日志时日志会附在这一帧里，限频约 100ms） */
        net_push_state();

        vTaskDelay(pdMS_TO_TICKS(dsp_access ? 15 : 80));
    }
}
