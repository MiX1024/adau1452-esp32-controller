#include "net.h"
#include "ui.h"   /* ui_db_to_percent_float / ui_percent_to_db */
#include "adau1452.h"
#include "sigma_tcp.h"
#include "usbi.h"
#include "dsp_image.h"
#include "logbuf.h"
#include "dsp_graph.h"
#include "eq_store.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net";

/* ---------- WiFi 配置 ----------
 * 配置默认值 + NVS 持久化（namespace "wifi"）。
 * 首次启动用下面的默认值；用户通过 Web 页修改后写入 NVS，重启后仍生效。
 *
 * 默认值来源：`wifi_secrets.h`（本地私密文件，**已被 .gitignore 排除**）。
 * 克隆仓库后请复制 `wifi_secrets.example.h` 为 `wifi_secrets.h` 并填入自己的
 * WiFi 信息；该文件不存在则回退到下面的通用值（STA 留空 = 默认只开热点，
 * 设备仍可通过网页「WiFi 设置」卡片配置）。 */
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif

#ifndef WIFI_DEFAULT_AP_SSID
#  define WIFI_DEFAULT_AP_SSID   "DSP-2.1"
#endif
#ifndef WIFI_DEFAULT_AP_PASS
#  define WIFI_DEFAULT_AP_PASS   "12345678"
#endif
#ifndef WIFI_DEFAULT_STA_SSID
#  define WIFI_DEFAULT_STA_SSID  ""
#endif
#ifndef WIFI_DEFAULT_STA_PASS
#  define WIFI_DEFAULT_STA_PASS  ""
#endif

static const char DEFAULT_AP_SSID[]  = WIFI_DEFAULT_AP_SSID;
static const char DEFAULT_AP_PASS[]  = WIFI_DEFAULT_AP_PASS;
static const char DEFAULT_STA_SSID[] = WIFI_DEFAULT_STA_SSID;
static const char DEFAULT_STA_PASS[] = WIFI_DEFAULT_STA_PASS;

#define WIFI_MAX_SSID_LEN   32
#define WIFI_MAX_PASS_LEN   64

static char s_ap_ssid[WIFI_MAX_SSID_LEN + 1];
static char s_ap_pass[WIFI_MAX_PASS_LEN + 1];
static char s_sta_ssid[WIFI_MAX_SSID_LEN + 1];
static char s_sta_pass[WIFI_MAX_PASS_LEN + 1];

/*
 * AP / STA 独立开关。
 * 为什么要能关 AP：APSTA 模式下 AP 会持续发送信标（即使没人连），
 * 与 STA 争抢射频时间片。确定只用路由器时关掉 AP，STA 的收包时序更干净。
 * 反之只用直连时关掉 STA，可避免 ESP32 持续扫描/重连。
 */
static bool s_ap_enabled  = true;
static bool s_sta_enabled = true;

static httpd_handle_t s_server = NULL;
static int s_ws_fd = -1;
static const net_callbacks_t *s_cb = NULL;
static bool s_sta_configured = false;
static bool s_sta_connected = false;
static TickType_t s_last_push = 0;
/* 是否有网页客户端订阅了实时日志（日志随状态帧一起推送，见 net_push_state） */
static bool s_log_stream;
/* 刚订阅，下一帧要先补一段历史日志 */
static bool s_log_need_history;

/* 嵌入的网页（由 CMake EMBED_FILES 注入，符号基于文件名 index.html） */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ---------- 状态 JSON 构建与推送 ---------- */
/* 单帧附带的日志上限（太大会让状态帧变重、挤占带宽） */
#define LOG_CHUNK   1000
/* SigmaStudio 调试中：单帧日志更小，减少占用无线链路 */
#define LOG_CHUNK_BRIDGE 500
/* 新订阅时先补一段历史（只发一次，之后只推新增） */
#define LOG_HIST    2600

/*
 * 连线图数据（NetList.xml + .params）的存在状态。
 * 放进状态帧里，网页就不必额外轮询 /api/graph 才知道设备上有没有数据。
 */
static void add_graph_json(cJSON *root)
{
    dsp_graph_file_info_t info[DSP_GRAPH_FILE_COUNT];
    dsp_graph_get_info(info);

    cJSON *nl = cJSON_CreateObject();
    cJSON_AddBoolToObject(nl, "valid", info[DSP_GRAPH_FILE_NETLIST].valid);
    cJSON_AddNumberToObject(nl, "len", (double)info[DSP_GRAPH_FILE_NETLIST].len);
    cJSON_AddNumberToObject(nl, "crc", (double)info[DSP_GRAPH_FILE_NETLIST].crc);
    cJSON_AddItemToObject(root, "netlist", nl);

    cJSON *pa = cJSON_CreateObject();
    cJSON_AddBoolToObject(pa, "valid", info[DSP_GRAPH_FILE_PARAMS].valid);
    cJSON_AddNumberToObject(pa, "len", (double)info[DSP_GRAPH_FILE_PARAMS].len);
    cJSON_AddNumberToObject(pa, "crc", (double)info[DSP_GRAPH_FILE_PARAMS].crc);
    cJSON_AddItemToObject(root, "params", pa);
}

/*
 * 首页控件的绑定（哪个模块管音量、哪个管电平）。
 * 状态帧里只给「名字 + 地址」（够网页显示与滑块用），
 * 完整的 key（用于重新导入后自动解析）走 GET /api/bind 单独取。
 */
static void add_bind_json(cJSON *root)
{
    net_bind_slot_t s[NET_BIND_SLOTS];
    memset(s, 0, sizeof(s));
    if (s_cb && s_cb->bind_get) {
        s_cb->bind_get(s);
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < NET_BIND_SLOTS; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "n", s[i].name);
        cJSON_AddNumberToObject(o, "a", s[i].addr);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "bind", arr);
}

static void add_bridge_json(cJSON *root)
{
    sigma_tcp_config_t cfg = sigma_tcp_get_config();
    sigma_tcp_status_t st;
    sigma_tcp_get_status(&st);

    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddBoolToObject(root, "local_control", cfg.local_control);
    cJSON_AddBoolToObject(root, "verbose", cfg.verbose);
    cJSON_AddNumberToObject(root, "resp_gap_us", cfg.resp_gap_us);
    cJSON_AddBoolToObject(root, "running", st.running);
    cJSON_AddBoolToObject(root, "client", st.client);
    cJSON_AddStringToObject(root, "ip", st.client_ip);
    cJSON_AddNumberToObject(root, "port", st.port);
    cJSON_AddNumberToObject(root, "pkt_write", st.pkt_write);
    cJSON_AddNumberToObject(root, "pkt_read", st.pkt_read);
    cJSON_AddNumberToObject(root, "errors", st.errors);
    cJSON_AddNumberToObject(root, "bytes", st.bytes);
    /* ⚠️ last_addr 是地址，必须用 Number —— 曾经误用 AddBool，
     * 网页上会显示成 "0xTRUE"（地址 0x0001 也永远变成 true/false）。 */
    cJSON_AddNumberToObject(root, "last_addr", st.last_addr);
    cJSON_AddNumberToObject(root, "last_len", st.last_len);
    cJSON_AddBoolToObject(root, "last_is_read", st.last_is_read);
    /* 连接次数持续增长 = SigmaStudio 在反复重连（崩溃的前兆） */
    cJSON_AddNumberToObject(root, "conn_count", st.conn_count);
    cJSON_AddNumberToObject(root, "active", st.active);
    cJSON_AddNumberToObject(root, "disconnects", st.disconnects);
    cJSON_AddNumberToObject(root, "resync_bytes", st.resync_bytes);

    /* 读响应耗时（微秒）：用于判断瓶颈在 I2C 还是网络/调度 */
    cJSON_AddNumberToObject(root, "lat_last", st.lat_last_us);
    cJSON_AddNumberToObject(root, "lat_max", st.lat_max_us);
    cJSON_AddNumberToObject(root, "lat_avg", st.lat_avg_us);
    cJSON_AddNumberToObject(root, "i2c_last", st.i2c_last_us);
    cJSON_AddNumberToObject(root, "i2c_max", st.i2c_max_us);
    cJSON_AddNumberToObject(root, "jitter10", st.lat_over_10ms);
}

/*
 * USBi（USB 直连 SigmaStudio）状态。
 * 与 bridge 分开一个对象：网页据此显示开关、USB 枚举状态和转发统计。
 */
static void add_usbi_json(cJSON *root)
{
    usbi_status_t st;
    usbi_get_status(&st);

    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "installed", st.installed);
    cJSON_AddBoolToObject(root, "mounted", st.mounted);
    cJSON_AddBoolToObject(root, "configured", st.configured);
    cJSON_AddNumberToObject(root, "attach_count", st.attach_count);
    cJSON_AddNumberToObject(root, "pkt_write", st.pkt_write);
    cJSON_AddNumberToObject(root, "pkt_read", st.pkt_read);
    cJSON_AddNumberToObject(root, "cmd_other", st.cmd_other);
    cJSON_AddNumberToObject(root, "errors", st.errors);
    cJSON_AddNumberToObject(root, "last_addr", st.last_addr);
    cJSON_AddNumberToObject(root, "last_len", st.last_len);
    cJSON_AddBoolToObject(root, "last_is_read", st.last_is_read);
    cJSON_AddNumberToObject(root, "last_format", st.last_format);
    cJSON_AddNumberToObject(root, "last_value", st.last_value);
    cJSON_AddNumberToObject(root, "last_index", st.last_index);
    cJSON_AddNumberToObject(root, "last_us", st.last_us);
    cJSON_AddNumberToObject(root, "max_us", st.max_us);
    cJSON_AddNumberToObject(root, "write_status", st.write_status);
    cJSON_AddNumberToObject(root, "read_status", st.read_status);
}

/*
 * WiFi 链路质量。这是排查"接路由器偶尔崩、直连却稳定"的第一现场数据：
 * RSSI 差（< -75dBm）或信道拥塞会直接导致 TCP 丢包重传，
 * 而重传要等 RTO（~1.5s），早就超过 SigmaStudio 的等待极限了。
 */
static void add_wifi_link_json(cJSON *root)
{
    cJSON_AddBoolToObject(root, "connected", s_sta_connected);
    cJSON_AddBoolToObject(root, "ap_enabled", s_ap_enabled);
    cJSON_AddBoolToObject(root, "sta_enabled", s_sta_enabled);
    cJSON_AddStringToObject(root, "ap_ssid", s_ap_ssid);

    wifi_ap_record_t ap;
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddNumberToObject(root, "ch", ap.primary);
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        cJSON_AddStringToObject(root, "bssid", bssid);
    }

    esp_netif_ip_info_t ip;
    esp_netif_t *st = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (st != NULL && esp_netif_get_ip_info(st, &ip) == ESP_OK) {
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(root, "ip", buf);
    }
}

static void add_dspimg_json(cJSON *root)
{
    cJSON *ram = cJSON_CreateObject();
    cJSON_AddBoolToObject(ram, "valid", dsp_image_ram_valid());
    cJSON_AddBoolToObject(ram, "has_program", dsp_image_ram_has_program());
    cJSON_AddBoolToObject(ram, "recording", dsp_image_recording());
    cJSON_AddNumberToObject(ram, "regs", dsp_image_ram_regs());
    cJSON_AddNumberToObject(ram, "exts", dsp_image_ram_exts());
    cJSON_AddNumberToObject(ram, "bytes", dsp_image_ram_bytes());
    cJSON_AddItemToObject(root, "ram", ram);

    /* 上电自动加载配置：开关 + 推送到 1452 的那一份（-1 = 不推送）+ 就绪等待时间 */
    cJSON_AddBoolToObject(root, "auto", dsp_image_auto_load());
    cJSON_AddNumberToObject(root, "boot", dsp_image_boot_slot());
    cJSON_AddNumberToObject(root, "settle_ms", dsp_image_settle_ms());

    /* 每个槽位一份独立映像（带用户起的名字） */
    const int boot_slot = dsp_image_boot_slot();
    cJSON *slots = cJSON_CreateArray();
    for (int i = 0; i < dsp_image_slot_count(); i++) {
        dsp_image_slot_t sl;
        if (dsp_image_slot_get(i, &sl) != ESP_OK) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", sl.index);
        cJSON_AddBoolToObject(o, "valid", sl.valid);
        cJSON_AddNumberToObject(o, "seq", sl.seq);
        cJSON_AddNumberToObject(o, "bytes", sl.bytes);
        cJSON_AddStringToObject(o, "name", sl.name);
        cJSON_AddBoolToObject(o, "boot", sl.index == boot_slot);
        cJSON_AddItemToArray(slots, o);
    }
    cJSON_AddItemToObject(root, "slots", slots);

    /* 兼容字段：最新（序号最大）那一份的摘要 */
    cJSON *fl = cJSON_CreateObject();
    cJSON_AddBoolToObject(fl, "valid", dsp_image_flash_valid());
    cJSON_AddNumberToObject(fl, "seq", dsp_image_flash_seq());
    cJSON_AddNumberToObject(fl, "bytes", dsp_image_flash_bytes());
    cJSON_AddItemToObject(root, "flash", fl);

    /* 本次开机的自动加载结果（失败时能看到原因，不用猜）。
     * 注意这是**持续监控**的状态：1452 掉线会回到 wait，重新上线会再推一次。 */
    dsp_image_boot_result_t br;
    dsp_image_boot_result(&br);
    static const char *boot_state[] = { "idle", "wait", "settle", "ok", "fail" };
    cJSON *bres = cJSON_CreateObject();
    cJSON_AddStringToObject(bres, "state",
                            boot_state[(br.state <= DSP_BOOT_FAIL) ? br.state : 0]);
    cJSON_AddNumberToObject(bres, "slot", br.slot);
    cJSON_AddNumberToObject(bres, "uptime_ms", br.uptime_ms);
    cJSON_AddNumberToObject(bres, "attempts", br.attempts);
    cJSON_AddStringToObject(bres, "msg", br.msg);
    cJSON_AddItemToObject(root, "boot_result", bres);
}

static char *build_state_json(bool with_log, size_t log_cap)
{
    float db[4], mn[4], mx[4];
    int sel;
    bool dsp_ok;
    float l, r;
    s_cb->get_state(db, mn, mx, &sel, &dsp_ok, &l, &r);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "dsp", dsp_ok);
    cJSON_AddNumberToObject(root, "sel", sel);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        bool pct = s_cb->channel_is_percent(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s_cb->channel_name(i));
        if (pct) {
            cJSON_AddStringToObject(o, "unit", "%");
            cJSON_AddNumberToObject(o, "value", ui_db_to_percent_float(db[i]));
            cJSON_AddNumberToObject(o, "min", 0);
            cJSON_AddNumberToObject(o, "max", 120);
        } else {
            cJSON_AddStringToObject(o, "unit", "dB");
            cJSON_AddNumberToObject(o, "value", db[i]);
            cJSON_AddNumberToObject(o, "min", mn[i]);
            cJSON_AddNumberToObject(o, "max", mx[i]);
        }
        cJSON_AddNumberToObject(o, "step", 0.5);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "ch", arr);

    cJSON *lv = cJSON_CreateObject();
    cJSON_AddNumberToObject(lv, "l", l);
    cJSON_AddNumberToObject(lv, "r", r);
    cJSON_AddItemToObject(root, "level", lv);

    cJSON *br = cJSON_CreateObject();
    add_bridge_json(br);
    cJSON_AddItemToObject(root, "bridge", br);

    /* USBi（USB 直连 SigmaStudio）状态 */
    cJSON *usb = cJSON_CreateObject();
    add_usbi_json(usb);
    cJSON_AddItemToObject(root, "usbi", usb);

    cJSON *img = cJSON_CreateObject();
    add_dspimg_json(img);
    cJSON_AddItemToObject(root, "dspimg", img);

    cJSON *wf = cJSON_CreateObject();
    add_wifi_link_json(wf);
    cJSON_AddItemToObject(root, "wifi", wf);

    cJSON *gp = cJSON_CreateObject();
    add_graph_json(gp);
    cJSON_AddItemToObject(root, "graph", gp);

    add_bind_json(root);

    /* 安全模式：前端据此禁用滑块并显示提示 */
    cJSON_AddBoolToObject(root, "safe", s_cb->safe_get ? s_cb->safe_get() : false);

    cJSON *eq = cJSON_CreateObject();
    cJSON_AddNumberToObject(eq, "count", eq_store_count());
    cJSON_AddNumberToObject(eq, "applied", eq_store_applied());
    cJSON_AddItemToObject(root, "eq", eq);

    /* 是否正在 SigmaStudio 调试（网页/PC 端据此判断该不该让位） */
    cJSON_AddBoolToObject(root, "bridging", sigma_tcp_busy() || usbi_busy());

    /* 日志随状态帧一起发（同一 fd 上 httpd_ws_send_frame_async 不能并发发送，
     * 分成两帧会相互打断导致推送悄悄失效）。只在订阅（网页勾了「显示日志」）时才附带。 */
    if (with_log) {
        const size_t cap = s_log_need_history ? LOG_HIST : log_cap;
        char *lb = malloc(cap);
        if (lb != NULL) {
            size_t n;
            if (s_log_need_history) {
                /* 先把"最近一段历史"整块补上，并把读指针推到末尾，
                 * 这样之后只推新增内容、不会重复 */
                s_log_need_history = false;
                logbuf_sync_read();
                n = logbuf_snapshot(lb, cap);
            } else {
                n = logbuf_take(lb, cap);
            }
            if (n > 0) {
                cJSON_AddStringToObject(root, "log", lb);
            }
            free(lb);
        }
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

void net_push_state(void)
{
    if (s_server == NULL || s_ws_fd < 0 || s_cb == NULL) {
        return;
    }

    /*
     * ⚠️ 关于桥接期间推日志：
     *
     * 曾经在桥接时**暂停日志推送**，理由是一帧 2~3KB 的日志可能把 18 字节的
     * 读响应压在 lwIP 发送队列里。但后来实锤了崩溃的真正根因是多条应答被 TCP
     * 合并（用 30ms 应答间隔解决），与日志无关；而暂停的代价是用户在线调试时
     * **完全看不到任何日志**（这就是"开日志但没输出"的原因）。
     *
     * 现在恢复推送，只是桥接时把单帧日志上限调小、周期放宽到 200ms，
     * 降低链路负担。
     */
    const bool bridging = sigma_tcp_busy();

    TickType_t now = xTaskGetTickCount();
    if ((now - s_last_push) < pdMS_TO_TICKS(bridging ? 200 : 100)) {
        return;
    }
    s_last_push = now;

    /* 确认该 fd 仍是 WebSocket 客户端，否则清空重连标记 */
    if (httpd_ws_get_fd_info(s_server, s_ws_fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        s_ws_fd = -1;
        return;
    }

    /* 只有网页勾了「显示日志」才附带日志字段，否则白占带宽 */
    char *json = build_state_json(s_log_stream, bridging ? LOG_CHUNK_BRIDGE : LOG_CHUNK);
    if (json == NULL) {
        return;
    }
    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)json;
    frame.len = strlen(json);
    /* 失败（例如上一帧仍在发送）不做处理，下个周期自然会重试 */
    httpd_ws_send_frame_async(s_server, s_ws_fd, &frame);
    cJSON_free(json);
}

/* 网页「清空日志」用 */
void net_log_clear(void)
{
    logbuf_clear();
}

/* ---------- WebSocket 命令处理 ---------- */
static void handle_ws_command(const char *payload, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        return;
    }
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    cJSON *idx = cJSON_GetObjectItem(root, "idx");
    cJSON *val = cJSON_GetObjectItem(root, "value");

    if (cmd && cJSON_IsString(cmd)) {
        /* 日志订阅开关（网页「显示日志」勾选状态） */
        if (strcmp(cmd->valuestring, "log") == 0) {
            cJSON *on = cJSON_GetObjectItem(root, "on");
            if (on && cJSON_IsBool(on)) {
                s_log_stream = cJSON_IsTrue(on);
                if (s_log_stream) {
                    s_log_need_history = true;   /* 下一帧先补历史，而不是清空缓冲 */
                }
                ESP_LOGI(TAG, "日志推送：%s", s_log_stream ? "开" : "关");
            }
            cJSON_Delete(root);
            return;
        }
    }

    if (cmd && cJSON_IsString(cmd) && strcmp(cmd->valuestring, "set") == 0 &&
        idx && cJSON_IsNumber(idx) && val && cJSON_IsNumber(val)) {
        int i = idx->valueint;
        if (i >= 0 && i < 4) {
            float v = (float)val->valuedouble;
            float db = s_cb->channel_is_percent(i) ? ui_percent_to_db(v) : v;
            s_cb->set_volume_db(i, db);
        }
    }
    cJSON_Delete(root);
}

/* ---------- URI handlers ---------- */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* 禁止缓存：否则刷完固件浏览器仍用旧页面 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t state_handler(httpd_req_t *req);
static esp_err_t wifi_get_handler(httpd_req_t *req);
static esp_err_t wifi_set_handler(httpd_req_t *req);
static esp_err_t i2c_handler(httpd_req_t *req);
static esp_err_t save_wifi_config_to_nvs(const char *ap_ssid, const char *ap_pass,
                                         const char *sta_ssid, const char *sta_pass);
static void add_wifi_link_json(cJSON *root);

static esp_err_t state_handler(httpd_req_t *req)
{
    char *json = build_state_json(false, LOG_CHUNK);   /* HTTP 查询不消费日志，避免抢走 WS 的日志 */
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "state error");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *ap = cJSON_CreateObject();
    cJSON_AddStringToObject(ap, "ssid", s_ap_ssid);
    cJSON_AddStringToObject(ap, "pass", s_ap_pass);
    cJSON_AddBoolToObject(ap, "enabled", s_ap_enabled);
    cJSON_AddItemToObject(root, "ap", ap);
    cJSON *sta = cJSON_CreateObject();
    cJSON_AddStringToObject(sta, "ssid", s_sta_ssid);
    cJSON_AddStringToObject(sta, "pass", s_sta_pass);
    cJSON_AddBoolToObject(sta, "enabled", s_sta_enabled);
    cJSON_AddBoolToObject(sta, "connected", s_sta_connected);
    cJSON_AddItemToObject(root, "sta", sta);
    /* 链接质量直接附上，前端展开菜单时一次就拿全 */
    add_wifi_link_json(root);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* 握手完成，记录客户端 fd。
         * 同时清掉日志订阅标志：否则旧的（已断开的）客户端会把日志缓冲一直抽干，
         * 新打开的页面就看不到任何历史日志。页面连上后会自己重新订阅。 */
        s_ws_fd = httpd_req_to_sockfd(req);
        s_log_stream = false;
        ESP_LOGI(TAG, "websocket client connected (fd=%d)", s_ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {0};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        s_ws_fd = -1;
        return ret;
    }
    if (pkt.len == 0) {
        return ESP_OK;
    }

    uint8_t *buf = malloc(pkt.len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK && pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf[pkt.len] = '\0';
        handle_ws_command((char *)buf, pkt.len);
    }
    free(buf);
    return ret;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA begin, writing to %s (%d bytes)", update->label, (int)req->content_len);

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, sizeof(buf));
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(handle, buf, got) != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_FAIL;
        }
        remaining -= got;
    }

    if (esp_ota_end(handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(update) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OTA 成功，3 秒后重启…");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t wifi_set_handler(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, total);
    if (got != total) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    const char *ap_ssid = NULL, *ap_pass = NULL, *sta_ssid = NULL, *sta_pass = NULL;
    bool ap_en = s_ap_enabled, sta_en = s_sta_enabled;
    cJSON *ap = cJSON_GetObjectItem(root, "ap");
    if (ap) {
        cJSON *s = cJSON_GetObjectItem(ap, "ssid");
        cJSON *p = cJSON_GetObjectItem(ap, "pass");
        cJSON *e = cJSON_GetObjectItem(ap, "enabled");
        if (s && cJSON_IsString(s)) ap_ssid = s->valuestring;
        if (p && cJSON_IsString(p)) ap_pass = p->valuestring;
        if (e && cJSON_IsBool(e))   ap_en = cJSON_IsTrue(e);
    }
    cJSON *sta = cJSON_GetObjectItem(root, "sta");
    if (sta) {
        cJSON *s = cJSON_GetObjectItem(sta, "ssid");
        cJSON *p = cJSON_GetObjectItem(sta, "pass");
        cJSON *e = cJSON_GetObjectItem(sta, "enabled");
        if (s && cJSON_IsString(s)) sta_ssid = s->valuestring;
        if (p && cJSON_IsString(p)) sta_pass = p->valuestring;
        if (e && cJSON_IsBool(e))   sta_en = cJSON_IsTrue(e);
    }

    /* 两个都关会彻底失联，直接拒绝 */
    if (!ap_en && !sta_en) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "AP 与 STA 不能同时关闭，否则将无法访问设备");
        return ESP_FAIL;
    }

    /*
     * 只改了开关（账号密码没变）→ 在线切换模式，**不重启**。
     * 这样调 AP/STA 时不会把 SigmaStudio 的会话和网页一起打断。
     */
    const bool cred_changed =
        (ap_ssid  != NULL && strcmp(ap_ssid,  s_ap_ssid)  != 0) ||
        (ap_pass  != NULL && strcmp(ap_pass,  s_ap_pass)  != 0) ||
        (sta_ssid != NULL && strcmp(sta_ssid, s_sta_ssid) != 0) ||
        (sta_pass != NULL && strcmp(sta_pass, s_sta_pass) != 0);

    if (!cred_changed) {
        esp_err_t e = net_wifi_set_enabled(ap_en, sta_en);
        cJSON_Delete(root);
        if (e != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "切换失败");
            return ESP_FAIL;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "restart", false);
        cJSON_AddBoolToObject(r, "ap_enabled", s_ap_enabled);
        cJSON_AddBoolToObject(r, "sta_enabled", s_sta_enabled);
        char *j = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, j ? j : "{\"ok\":true}");
        if (j) cJSON_free(j);
        return ESP_OK;
    }

    /* AP SSID 不能为空 —— **仅在用户确实传了新 SSID** 时校验。
     * （只切换开关时前端只发 enabled，不传 SSID，不应报错。） */
    if (ap_en && ap_ssid != NULL && ap_ssid[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP SSID empty");
        return ESP_FAIL;
    }
    /* 开了 AP 但从未配置过 SSID（也不应该） */
    if (ap_en && ap_ssid == NULL && s_ap_ssid[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP SSID empty");
        return ESP_FAIL;
    }

    s_ap_enabled = ap_en;
    s_sta_enabled = sta_en;
    esp_err_t err = save_wifi_config_to_nvs(
        ap_ssid ? ap_ssid : s_ap_ssid, ap_pass ? ap_pass : s_ap_pass,
        sta_ssid ? sta_ssid : s_sta_ssid, sta_pass ? sta_pass : s_sta_pass);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "配置已保存，设备即将重启…");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* ---------- SigmaStudio 桥接 / DSP 程序映像 / 地址读写 ---------- */

/* 读取 POST body 到 buf（自动补 '\0'）；失败返回 -1 */
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= cap) {
        return -1;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return -1;
        }
        got += n;
    }
    buf[total] = '\0';
    return total;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

/* ---- /api/bridge ：查询 / 修改桥接配置 ---- */
static esp_err_t bridge_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_bridge_json(root);
    return send_json(req, root);
}

static esp_err_t bridge_set_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    sigma_tcp_config_t cfg = sigma_tcp_get_config();
    bool enabled = cfg.enabled;
    bool local = cfg.local_control;
    bool verbose = cfg.verbose;
    uint32_t resp_gap = cfg.resp_gap_us;
    uint16_t port = cfg.port;

    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(it)) {
        enabled = cJSON_IsTrue(it);
    }
    if ((it = cJSON_GetObjectItem(root, "local_control")) && cJSON_IsBool(it)) {
        local = cJSON_IsTrue(it);
    }
    if ((it = cJSON_GetObjectItem(root, "verbose")) && cJSON_IsBool(it)) {
        verbose = cJSON_IsTrue(it);
    }
    if ((it = cJSON_GetObjectItem(root, "resp_gap_us")) && cJSON_IsNumber(it)) {
        resp_gap = (uint32_t)it->valuedouble;
    }
    if ((it = cJSON_GetObjectItem(root, "port")) && cJSON_IsNumber(it)) {
        port = (uint16_t)it->valueint;
    }
    cJSON_Delete(root);

    /* 互斥：TCP 桥接与 USBi 都会独占 DSP，开启一个就先关掉另一个 */
    if (enabled && !cfg.enabled && usbi_enabled()) {
        usbi_set_enabled(false);
        ESP_LOGW(TAG, "开启桥接服务：已自动关闭 USBi（两者互斥）");
    }

    esp_err_t e = sigma_tcp_set_config(enabled, port, local, resp_gap);
    if (e == ESP_OK) {
        /* 逐报文日志是独立的开关，单独设置 */
        sigma_tcp_set_verbose(verbose);
    }
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "端口无效（需 >= 1024）或启动失败");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    add_bridge_json(resp);
    return send_json(req, resp);
}

/* ---- /api/usbi ：USB（USBi）模式开关与状态 ----
 * 打开后 ESP32 的原生 USB 口枚举成 Analog Devices USBi，
 * SigmaStudio 在 Hardware Configuration 里选 USBi 即可（无需 WiFi）。
 * 与 TCP 桥接互斥：打开 USBi 会自动关掉桥接服务。 */
static esp_err_t usbi_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_usbi_json(root);
    return send_json(req, root);
}

static esp_err_t usbi_set_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON *it = cJSON_GetObjectItem(root, "enabled");
    if (!it || !cJSON_IsBool(it)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enabled");
        return ESP_FAIL;
    }
    const bool on = cJSON_IsTrue(it);
    cJSON_Delete(root);

    if (on) {
        /* 互斥：先关掉 TCP 桥接（两者都独占 DSP） */
        sigma_tcp_config_t cfg = sigma_tcp_get_config();
        if (cfg.enabled) {
            sigma_tcp_set_config(false, cfg.port, cfg.local_control, cfg.resp_gap_us);
            ESP_LOGW(TAG, "开启 USBi：已自动关闭 TCP 桥接（两者互斥）");
        }
    }

    esp_err_t e = usbi_set_enabled(on);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "USBi 切换失败（USB 初始化失败，详见设备日志）");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    add_usbi_json(resp);
    return send_json(req, resp);
}

/* ---- I2C 配置持久化（复用 "bridge" 命名空间） ----
 * 频率/上拉是硬件级参数，必须掉电保留，否则重启就回默认了。
 */
static void i2c_config_save(void)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, "i2chz", adau1452_i2c_speed());
    nvs_set_u8(h, "i2cpu", adau1452_i2c_internal_pullup() ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void i2c_config_load(void)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t hz = 0;
    uint8_t  pu = 2;
    if (nvs_get_u32(h, "i2chz", &hz) == ESP_OK && hz >= 10000 && hz <= 1000000) {
        if (adau1452_i2c_set_speed(hz) == ESP_OK) {
            ESP_LOGW(TAG, "已恢复 I2C 时钟 %lu Hz（上次设置）", (unsigned long)hz);
        }
    }
    if (nvs_get_u8(h, "i2cpu", &pu) == ESP_OK && pu <= 1) {
        adau1452_i2c_set_internal_pullup(pu != 0);
    }
    nvs_close(h);
}

/*
 * 桥接服务开启期间，本地一律不得**改动** DSP 或 I2C 总线参数。
 *
 * 这不只是"让位"：SigmaStudio 一旦重编过程序，本地硬编码的地址
 * （29/226/164/161）就可能指向完全不同的参数 —— 写进去会改错东西，
 * 甚至破坏正在调试的程序。所以凡是有副作用的本地操作全部拒绝。
 * 只读操作（读地址、看日志）放行，它不改动 DSP 状态。
 *
 * 返回 true 表示已拒绝并回了错误响应，调用方直接 return。
 */
static bool bridge_blocks_local_write(httpd_req_t *req)
{
    const bool usb = usbi_enabled();
    if (!sigma_tcp_get_config().enabled && !sigma_tcp_busy() && !usb) {
        return false;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        usb
                        ? "USBi 已开启，DSP 由 SigmaStudio 管理；请先在「USBi（USB）」里关闭 USBi"
                        : "桥接服务已开启，DSP 由 SigmaStudio 管理；"
                          "请先在「SigmaStudio 桥接」里关闭桥接服务");
    return true;
}

/* ---- /api/i2c ：I2C 总线调优（频率 / 内部上拉） ---- */

static esp_err_t i2c_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "hz", adau1452_i2c_speed());
        cJSON_AddBoolToObject(root, "pullup", adau1452_i2c_internal_pullup());
        return send_json(req, root);
    }

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    /*
     * ⚠️ 改频率 / 上拉都会直接干扰总线上的 SigmaStudio 会话，
     * 所以桥接服务开启期间一律拒绝。
     */
    if (bridge_blocks_local_write(req)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *it;
    esp_err_t err = ESP_OK;

    if ((it = cJSON_GetObjectItem(root, "hz")) && cJSON_IsNumber(it)) {
        err = adau1452_i2c_set_speed((uint32_t)it->valuedouble);
    }
    if (err == ESP_OK && (it = cJSON_GetObjectItem(root, "pullup")) && cJSON_IsBool(it)) {
        err = adau1452_i2c_set_internal_pullup(cJSON_IsTrue(it));
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "I2C 操作失败（频率需 10000~1000000）");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    i2c_config_save();                  /* 频率/上拉改动掉电保留 */
    cJSON_AddNumberToObject(resp, "hz", adau1452_i2c_speed());
    cJSON_AddBoolToObject(resp, "pullup", adau1452_i2c_internal_pullup());
    return send_json(req, resp);
}

/* ---- /api/dspimg ：查询 / 保存 / 清空 / 擦除 / 重载 ---- */
static esp_err_t dspimg_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_dspimg_json(root);
    return send_json(req, root);
}

static esp_err_t dspimg_set_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *act = cJSON_GetObjectItem(root, "action");
    if (act == NULL || !cJSON_IsString(act)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing action");
        return ESP_FAIL;
    }

    const char *a = act->valuestring;
    const char *msg;
    esp_err_t e = ESP_OK;
    static char ebuf[80];
    cJSON *it;

    /* 取 slot 参数（-1 = 「不推送」，其余 0..N-1） */
    int slot = -2;
    if ((it = cJSON_GetObjectItem(root, "slot")) && cJSON_IsNumber(it)) {
        slot = it->valueint;
    }

    if (strcmp(a, "record_start") == 0) {
        dsp_image_record_start();
        msg = "已开始录制，请在 SigmaStudio 里执行 Link Compile Download";
    } else if (strcmp(a, "record_stop") == 0) {
        dsp_image_record_stop();
        msg = "已停止录制";
    } else if (strcmp(a, "clear") == 0) {
        dsp_image_record_stop();
        dsp_image_ram_clear();
        msg = "录制已清空";
    } else if (strcmp(a, "save") == 0) {
        if (!dsp_image_ram_valid()) {
            e = ESP_ERR_INVALID_STATE;
            msg = "还没有录制到任何内容";
        } else if (!dsp_image_ram_has_program()) {
            e = ESP_ERR_INVALID_STATE;
            msg = "录制里没有程序 RAM，请在 SigmaStudio 执行一次 Link Compile Download 再保存";
        } else if (sigma_tcp_busy()) {
            e = ESP_ERR_INVALID_STATE;
            msg = "SigmaStudio 正在连接，请先断开再保存";
        } else {
            int written = -1;
            e = dsp_image_save_to_flash(&written);
            if (e == ESP_OK) {
                snprintf(ebuf, sizeof(ebuf), "已保存到槽 %d（可在下面重命名）", written + 1);
                msg = ebuf;
            } else {
                snprintf(ebuf, sizeof(ebuf), "保存失败：%s", esp_err_to_name(e));
                msg = ebuf;
            }
        }
    } else if (strcmp(a, "erase") == 0) {
        e = dsp_image_flash_erase();
        msg = (e == ESP_OK) ? "全部 flash 映像已清除" : "清除失败";
    } else if (strcmp(a, "erase_slot") == 0) {
        if (slot < 0) {
            e = ESP_ERR_INVALID_ARG;
            msg = "缺少槽位号";
        } else {
            e = dsp_image_slot_erase(slot);
            if (e == ESP_OK) {
                snprintf(ebuf, sizeof(ebuf), "槽 %d 的映像已清除", slot + 1);
            } else {
                snprintf(ebuf, sizeof(ebuf), "清除失败：%s", esp_err_to_name(e));
            }
            msg = ebuf;
        }
    } else if (strcmp(a, "rename") == 0) {
        cJSON *nm = cJSON_GetObjectItem(root, "name");
        if (slot < 0 || nm == NULL || !cJSON_IsString(nm)) {
            e = ESP_ERR_INVALID_ARG;
            msg = "缺少槽位号或名称";
        } else {
            e = dsp_image_slot_rename(slot, nm->valuestring);
            msg = (e == ESP_OK) ? "已重命名" : "重命名失败";
        }
    } else if (strcmp(a, "set_boot") == 0) {
        if (slot == -2) {
            e = ESP_ERR_INVALID_ARG;
            msg = "缺少槽位号（-1 = 不推送）";
        } else {
            e = dsp_image_set_boot_slot(slot);
            if (e == ESP_OK) {
                msg = (slot < 0) ? "开机推送已设为：不推送" : "开机推送已设置";
            } else {
                msg = "设置失败";
            }
        }
    } else if (strcmp(a, "set_auto") == 0) {
        cJSON *on = cJSON_GetObjectItem(root, "on");
        if (on == NULL || !cJSON_IsBool(on)) {
            e = ESP_ERR_INVALID_ARG;
            msg = "缺少开关状态";
        } else {
            e = dsp_image_set_auto_load(cJSON_IsTrue(on));
            msg = cJSON_IsTrue(on) ? "已开启上电自动加载" : "已关闭上电自动加载";
        }
    } else if (strcmp(a, "set_settle") == 0) {
        cJSON *ms = cJSON_GetObjectItem(root, "ms");
        if (ms == NULL || !cJSON_IsNumber(ms)) {
            e = ESP_ERR_INVALID_ARG;
            msg = "缺少毫秒数";
        } else {
            e = dsp_image_set_settle_ms((uint32_t)ms->valuedouble);
            if (e == ESP_OK) {
                snprintf(ebuf, sizeof(ebuf), "就绪等待已设为 %d ms", ms->valueint);
                msg = ebuf;
            } else {
                snprintf(ebuf, sizeof(ebuf), "设置失败（范围 0~5000 ms）");
                msg = ebuf;
            }
        }
    } else if (strcmp(a, "replay_slot") == 0) {
        if (sigma_tcp_get_config().enabled || sigma_tcp_busy()) {
            e = ESP_ERR_INVALID_STATE;
            msg = "桥接服务已开启，DSP 由 SigmaStudio 管理；请先关闭桥接服务";
        } else if (slot < 0) {
            e = ESP_ERR_INVALID_ARG;
            msg = "缺少槽位号";
        } else {
            e = dsp_image_slot_replay(slot);
            msg = (e == ESP_OK) ? "已重新加载 DSP 程序" : "回放失败";
        }
    } else if (strcmp(a, "replay") == 0) {
        if (sigma_tcp_get_config().enabled || sigma_tcp_busy()) {
            e = ESP_ERR_INVALID_STATE;
            msg = "桥接服务已开启，DSP 由 SigmaStudio 管理；请先关闭桥接服务";
        } else if (!dsp_image_flash_valid()) {
            e = ESP_ERR_INVALID_STATE;
            msg = "flash 中没有映像";
        } else {
            e = dsp_image_replay();
            msg = (e == ESP_OK) ? "已重新加载 DSP 程序" : "回放失败";
        }
    } else {
        e = ESP_ERR_INVALID_ARG;
        msg = "未知操作";
    }
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    cJSON_AddStringToObject(resp, "msg", msg);
    add_dspimg_json(resp);
    return send_json(req, resp);
}

/* ---- /api/dspimg/download ：把映像导出成文件备份（流式发送，无需大块内存） ---- */
static esp_err_t img_download_sink(const uint8_t *data, size_t len, void *ctx)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, (const char *)data, len);
}

static esp_err_t dspimg_download_handler(httpd_req_t *req)
{
    if (!dsp_image_ram_valid() && !dsp_image_flash_valid()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                            "没有可导出的映像（先在桥接时录制）");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"dsp_image.bin\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t e = dsp_image_stream(img_download_sink, req);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "导出映像失败：%s", esp_err_to_name(e));
    }
    /* 结束分块响应 */
    httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

/* ---- /api/dsp ：直接读写任意 DSP 地址（不依赖 SigmaStudio 也能调试）
 * GET  /api/dsp?addr=0x0029&len=4      （地址可用 0x 前缀，内存区按字寻址）
 * POST /api/dsp  {"addr":41,"hex":"00804DCE"}
 * POST /api/dsp  {"addr":29,"value":8404942}   */
static esp_err_t dsp_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        char q[128];
        char vs[32];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
            httpd_query_key_value(q, "addr", vs, sizeof(vs)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr");
            return ESP_FAIL;
        }
        uint16_t addr = (uint16_t)strtoul(vs, NULL, 0);
        int len = 4;
        if (httpd_query_key_value(q, "len", vs, sizeof(vs)) == ESP_OK) {
            len = atoi(vs);
        }
        if (len < 1) {
            len = 1;
        }
        /*
         * ⚠️ DSP 内存（< 0xF000）是**按字寻址**的，内部按 `addr + off/4` 算子地址，
         * 所以长度必须是 4 的倍数，否则字节↔字的映射不再线性、读回来的数据会错位。
         * 这里向上取整到 4，让调用方即使传了奇怪的长度也不会拿到错位数据。
         */
        if (addr < ADAU1452_ADDR_REG_BASE && (len % 4) != 0) {
            len = ((len + 3) / 4) * 4;
        }
        if (len > 64) {
            len = 64;
        }

        uint8_t buf[64] = {0};
        esp_err_t e = adau1452_read_block(addr, buf, (size_t)len);

        char hex[64 * 2 + 1];
        for (int i = 0; i < len; i++) {
            sprintf(hex + i * 2, "%02X", buf[i]);
        }
        hex[len * 2] = '\0';

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "addr", addr);
        cJSON_AddNumberToObject(root, "len", len);
        cJSON_AddBoolToObject(root, "ok", e == ESP_OK);
        cJSON_AddStringToObject(root, "hex", hex);
        if (len == 4) {
            uint32_t raw = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                           ((uint32_t)buf[2] << 8) | buf[3];
            /* DSP 参数多为 8.24 定点：raw / 2^24 = 线性增益 */
            cJSON_AddNumberToObject(root, "raw", (double)raw);
            cJSON_AddNumberToObject(root, "f24", (double)((int32_t)raw) / 16777216.0);
        }
        return send_json(req, root);
    }

    /* POST：写入 */
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *addr_it = cJSON_GetObjectItem(root, "addr");
    cJSON *reset_it = cJSON_GetObjectItem(root, "reset");

    /*
     * 地址调试的**写入**与**软复位**都会改动 DSP，桥接开启时全部拒绝。
     * 注意 addr29 就是总音量 —— 不拦的话网页音量滑块能被绕过。
     */
    if (bridge_blocks_local_write(req)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (reset_it && cJSON_IsBool(reset_it) && cJSON_IsTrue(reset_it)) {
        cJSON_Delete(root);
        esp_err_t r = adau1452_soft_reset();
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", r == ESP_OK);
        cJSON_AddStringToObject(resp, "msg",
                                (r == ESP_OK) ? "已发送软复位（程序已丢失，请重载映像）"
                                              : "软复位失败");
        return send_json(req, resp);
    }
    if (addr_it == NULL || !cJSON_IsNumber(addr_it)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr");
        return ESP_FAIL;
    }
    uint16_t addr = (uint16_t)addr_it->valueint;

    uint8_t data[64];
    size_t len = 0;
    cJSON *hex_it = cJSON_GetObjectItem(root, "hex");
    cJSON *val_it = cJSON_GetObjectItem(root, "value");
    if (hex_it && cJSON_IsString(hex_it)) {
        /*
         * 归一化 hex：去掉 0x 前缀与分隔符（空格/逗号/连字符），只收集合法的
         * 十六进制数字，再成对解析。这样修正了旧逻辑的两个问题：
         *   ① 奇数长度时，最后一个字符被当成高半字节（"F" 被解析成 0xF0，
         *      实际应为 0x0F；"FFF" 被解析成 [FF,F0] 而非 [0F,FF]）；
         *   ② 分隔符紧跟的单个 nibble（如 "F F"）会多吞一字节、整体错位。
         */
        const char *s = hex_it->valuestring;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
        char digits[128];
        size_t dlen = 0;
        while (*s && dlen < sizeof(digits)) {
            const char c = *s;
            if (c >= '0' && c <= '9') {
                digits[dlen++] = c;
            } else if (c >= 'a' && c <= 'f') {
                digits[dlen++] = (char)(c - 'a' + 'A');
            } else if (c >= 'A' && c <= 'F') {
                digits[dlen++] = c;
            }
            /* 其它字符（分隔符、残留的 0x 等）直接跳过 */
            s++;
        }
        if (dlen & 1) {
            /* 奇数位：高位补 0 */
            memmove(digits + 1, digits, dlen);
            digits[0] = '0';
            dlen++;
        }
        for (size_t i = 0; i + 1 < dlen && len < sizeof(data); i += 2) {
            char tmp[3] = { digits[i], digits[i + 1], 0 };
            data[len++] = (uint8_t)strtoul(tmp, NULL, 16);
        }
    } else if (val_it && cJSON_IsNumber(val_it)) {
        uint32_t v = (uint32_t)val_it->valuedouble;
        data[0] = (uint8_t)(v >> 24);
        data[1] = (uint8_t)(v >> 16);
        data[2] = (uint8_t)(v >> 8);
        data[3] = (uint8_t)v;
        len = 4;
    }
    cJSON_Delete(root);

    if (len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "需要 hex 或 value");
        return ESP_FAIL;
    }
    if (addr < ADAU1452_ADDR_REG_BASE && (len % 4) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "DSP 内存写入长度必须是 4 的倍数");
        return ESP_FAIL;
    }

    esp_err_t e = adau1452_write_block(addr, data, len);

    char hex[64 * 2 + 1];
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + i * 2, "%02X", data[i]);
    }
    hex[len * 2] = '\0';

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    cJSON_AddNumberToObject(resp, "addr", addr);
    cJSON_AddNumberToObject(resp, "len", (int)len);
    cJSON_AddStringToObject(resp, "hex", hex);
    return send_json(req, resp);
}

/* ---- /api/graph ：连线图数据（NetList.xml + *.params）
 *
 * GET  /api/graph             → 两份文件的存在状态（长度 / CRC）
 * GET  /api/graph/netlist     → NetList.xml 原文（分块回传）
 * GET  /api/graph/params      → *.params 原文（分块回传）
 * POST /api/graph/netlist     → 流式上传原文
 * POST /api/graph/params      → 流式上传原文
 * POST /api/graph             {"action":"clear"}
 *
 * 这里只读写 flash、不碰 I2C，所以桥接期间**不需要拦**。
 * 上传走流式（1KB 栈缓冲），因为这两个文件可能有几百 KB，
 * 而 S3 上 WiFi+HTTP 之后最大连续堆块只有 ~12KB。
 */
static dsp_graph_file_t graph_file_from_uri(httpd_req_t *req)
{
    return (strstr(req->uri, "params") != NULL) ? DSP_GRAPH_FILE_PARAMS
                                                : DSP_GRAPH_FILE_NETLIST;
}

static esp_err_t graph_file_handler(httpd_req_t *req)
{
    const dsp_graph_file_t f = graph_file_from_uri(req);

    if (req->method == HTTP_GET) {
        dsp_graph_file_info_t info[DSP_GRAPH_FILE_COUNT];
        dsp_graph_get_info(info);
        if (!info[f].valid) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "设备上没有这份文件");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        char buf[1024];
        uint32_t off = 0;
        while (off < info[f].len) {
            size_t n = (size_t)info[f].len - off;
            if (n > sizeof(buf)) {
                n = sizeof(buf);
            }
            if (dsp_graph_read(f, off, buf, n) != ESP_OK) {
                break;
            }
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_FAIL;
            }
            off += (uint32_t)n;
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    if (dsp_graph_uploading()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "另一个上传正在进行");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > DSP_GRAPH_SLOT_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件为空或超过上限");
        return ESP_FAIL;
    }
    if (dsp_graph_upload_begin(f) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "上传初始化失败");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, sizeof(buf));
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            dsp_graph_upload_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_FAIL;
        }
        if (dsp_graph_upload_data(buf, (size_t)got) != ESP_OK) {
            dsp_graph_upload_abort();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "写入失败或超限");
            return ESP_FAIL;
        }
        remaining -= got;
    }

    if (dsp_graph_upload_finish() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return ESP_FAIL;
    }

    dsp_graph_file_info_t info[DSP_GRAPH_FILE_COUNT];
    dsp_graph_get_info(info);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "file", dsp_graph_file_name(f));
    cJSON_AddNumberToObject(root, "len", (double)info[f].len);
    cJSON_AddNumberToObject(root, "crc", (double)info[f].crc);
    return send_json(req, root);
}

static esp_err_t graph_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        dsp_graph_file_info_t info[DSP_GRAPH_FILE_COUNT];
        dsp_graph_get_info(info);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        add_graph_json(root);
        return send_json(req, root);
    }

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *act = cJSON_GetObjectItem(root, "action");
    if (act == NULL || !cJSON_IsString(act) || strcmp(act->valuestring, "clear") != 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown action");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    esp_err_t e = dsp_graph_clear();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    add_graph_json(resp);
    return send_json(req, resp);
}

/* ---- /api/bind ：首页控件 ↔ SigmaStudio 模块的绑定
 *
 * GET  → {slots:[{addr,name,key} × 6]}   （0..3 音量，4..5 电平）
 * POST → {"slots":[...]} 写回（桥接开启时 500 拒绝）
 *
 * key 存的是 "cell|parameter"，网页在重新导入工程文件后拿它重新解析出地址。
 */
static esp_err_t bind_handler(httpd_req_t *req)
{
    if (s_cb == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no callbacks");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        net_bind_slot_t s[NET_BIND_SLOTS];
        memset(s, 0, sizeof(s));
        if (s_cb->bind_get) {
            s_cb->bind_get(s);
        }
        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < NET_BIND_SLOTS; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "idx", i);
            cJSON_AddNumberToObject(o, "addr", s[i].addr);
            cJSON_AddStringToObject(o, "name", s[i].name);
            cJSON_AddStringToObject(o, "key", s[i].key);
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_AddItemToObject(root, "slots", arr);
        return send_json(req, root);
    }

    /* 同 /api/dsp、/api/i2c：桥接开着时一律拒绝改 DSP 相关配置 */
    if (bridge_blocks_local_write(req)) {
        return ESP_FAIL;
    }

    char body[1200];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(root, "slots");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slots");
        return ESP_FAIL;
    }

    net_bind_slot_t s[NET_BIND_SLOTS];
    memset(s, 0, sizeof(s));
    if (s_cb->bind_get) {
        s_cb->bind_get(s);   /* 未提供的槽位保持原值 */
    }
    /* 先把 addr 清成 0（未绑定）——GET 会把它填成默认值，写回时不能把默认值当成"已绑定" */
    for (int i = 0; i < NET_BIND_SLOTS; i++) {
        s[i].addr = 0;
    }

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *ji = cJSON_GetObjectItem(it, "idx");
        if (!cJSON_IsNumber(ji)) {
            continue;
        }
        int i = ji->valueint;
        if (i < 0 || i >= NET_BIND_SLOTS) {
            continue;
        }
        cJSON *ja = cJSON_GetObjectItem(it, "addr");
        cJSON *jn = cJSON_GetObjectItem(it, "name");
        cJSON *jk = cJSON_GetObjectItem(it, "key");
        if (cJSON_IsNumber(ja)) {
            s[i].addr = (uint16_t)ja->valueint;
        }
        if (cJSON_IsString(jn)) {
            strncpy(s[i].name, jn->valuestring, sizeof(s[i].name) - 1);
            s[i].name[sizeof(s[i].name) - 1] = '\0';
        }
        if (cJSON_IsString(jk)) {
            strncpy(s[i].key, jk->valuestring, sizeof(s[i].key) - 1);
            s[i].key[sizeof(s[i].key) - 1] = '\0';
        }
    }
    cJSON_Delete(root);

    bool ok = s_cb->bind_set ? s_cb->bind_set(s) : false;
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(resp, "msg", "绑定写入被拒绝或失败");
    }
    return send_json(req, resp);
}

/*
 * ---- /api/safe ：安全模式开关 ----
 *   GET  → {"safe":bool}
 *   POST → {"on":bool}
 *
 * 安全模式开启后，ESP32 上电**不会主动写** DSP 参数（音量/EQ 都只读回），
 * 防止用户用 SigmaStudio 改过 1452 程序后、ESP32 再把旧地址的音量写进去导致爆音。
 */
static esp_err_t safe_handler(httpd_req_t *req)
{
    if (s_cb == NULL || s_cb->safe_get == NULL || s_cb->safe_set == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no callbacks");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "safe", s_cb->safe_get());
        return send_json(req, root);
    }

    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *it = cJSON_GetObjectItem(root, "on");
    if (!cJSON_IsBool(it)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing on");
        return ESP_FAIL;
    }
    const bool on = cJSON_IsTrue(it);
    cJSON_Delete(root);

    s_cb->safe_set(on);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "safe", s_cb->safe_get());
    return send_json(req, resp);
}

/*
 * ---- /api/vol ：PC 端音量控制程序（win-volume-ctl）专用轻量接口 ----
 *   GET  → {dsp, safe,
 *           ch:[{name,unit,value,min,max} ×4],   （MASTER 百分比、其余 dB，与 /api/state 一致）
 *           level:{l,r},                          （dB，-999 = 无数据）
 *           bridge:{enabled,local_control,running,client},
 *           wifi:{connected,rssi,ch,bssid,ip,...}}
 *         是 /api/state 的子集：去掉图形/映像/EQ 等大块头，
 *         供 PC 端 100ms 级轮询（比整份 state 轻得多）。
 *
 *   POST → {"idx":0..3,"value":v}   （MASTER 传百分比、其余传 dB，与 /ws 的 set 一致）
 *   POST ← {"ok":true,"value":v}    value 为钳位后回读值
 *          {"ok":false,"reason":"bridge"|"safe"|"bad idx"|"write failed"}
 *
 * ⚠️ 与网页的 WebSocket「单客户端槽位」互不冲突：这里纯 HTTP，
 *    网页开着时两边都能同时用（网页看实时推送、PC 端轮询本接口）。
 */
static esp_err_t vol_handler(httpd_req_t *req)
{
    if (s_cb == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no callbacks");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        float db[4], mn[4], mx[4], l, r;
        int sel;
        bool dsp_ok;
        s_cb->get_state(db, mn, mx, &sel, &dsp_ok, &l, &r);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "dsp", dsp_ok);
        cJSON_AddBoolToObject(root, "safe", s_cb->safe_get ? s_cb->safe_get() : false);

        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < 4; i++) {
            bool pct = s_cb->channel_is_percent(i);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", s_cb->channel_name(i));
            cJSON_AddStringToObject(o, "unit", pct ? "%" : "dB");
            cJSON_AddNumberToObject(o, "value", pct ? ui_db_to_percent_float(db[i]) : db[i]);
            cJSON_AddNumberToObject(o, "min", pct ? 0.0f : mn[i]);
            cJSON_AddNumberToObject(o, "max", pct ? 120.0f : mx[i]);
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_AddItemToObject(root, "ch", arr);

        cJSON *lv = cJSON_CreateObject();
        cJSON_AddNumberToObject(lv, "l", l);
        cJSON_AddNumberToObject(lv, "r", r);
        cJSON_AddItemToObject(root, "level", lv);

        sigma_tcp_config_t cfg = sigma_tcp_get_config();
        sigma_tcp_status_t st;
        sigma_tcp_get_status(&st);
        cJSON *br = cJSON_CreateObject();
        cJSON_AddBoolToObject(br, "enabled", cfg.enabled);
        cJSON_AddBoolToObject(br, "local_control", cfg.local_control);
        cJSON_AddBoolToObject(br, "running", st.running);
        cJSON_AddBoolToObject(br, "client", st.client);
        cJSON_AddItemToObject(root, "bridge", br);

        /* USBi 也单独给一份（PC 端据此判断本地音量该不该写） */
        usbi_status_t us;
        usbi_get_status(&us);
        cJSON *usb = cJSON_CreateObject();
        cJSON_AddBoolToObject(usb, "enabled", us.enabled);
        cJSON_AddBoolToObject(usb, "configured", us.configured);
        cJSON_AddItemToObject(root, "usbi", usb);

        cJSON *wf = cJSON_CreateObject();
        add_wifi_link_json(wf);
        cJSON_AddItemToObject(root, "wifi", wf);
        return send_json(req, root);
    }

    /* ---- POST：{"idx":N,"value":v} ---- */
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *idx_it = cJSON_GetObjectItem(root, "idx");
    cJSON *val_it = cJSON_GetObjectItem(root, "value");
    if (!cJSON_IsNumber(idx_it) || !cJSON_IsNumber(val_it)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing idx/value");
        return ESP_FAIL;
    }
    const int i = idx_it->valueint;
    const float v = (float)val_it->valuedouble;
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    if (i < 0 || i >= 4) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "reason", "bad idx");
        return send_json(req, resp);
    }
    /* SigmaStudio 调试期间 DSP 归 SigmaStudio 管：拒绝但回 JSON（而非 HTTPD_500），
     * PC 端好在界面上给出明确提示 */
    if (sigma_tcp_get_config().enabled || sigma_tcp_busy() || usbi_enabled()) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "reason",
                                usbi_enabled() ? "usbi" : "bridge");
        return send_json(req, resp);
    }
    const float dbv = s_cb->channel_is_percent(i) ? ui_percent_to_db(v) : v;
    const bool ok = s_cb->set_volume_db(i, dbv);   /* 安全模式下 apply_volume 会拒绝 */
    cJSON_AddBoolToObject(resp, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(resp, "reason",
                                (s_cb->safe_get && s_cb->safe_get()) ? "safe" : "write failed");
    }
    /* 回读钳位后的当前值，PC 端据此同步显示 */
    float db[4], mn[4], mx[4], l, r;
    int sel;
    bool dsp_ok;
    s_cb->get_state(db, mn, mx, &sel, &dsp_ok, &l, &r);
    cJSON_AddNumberToObject(resp, "value",
                            s_cb->channel_is_percent(i) ? ui_db_to_percent_float(db[i]) : db[i]);
    return send_json(req, resp);
}

/*
 * ---- /api/eq ：EQ 系数的持久化 ----
 *   GET  → {bands:[{addr,words,key,w:[5]}], count, applied}
 *   POST → {"action":"put","addr":A,"key":"cell|param","w":[..5 个..]}
 *          {"action":"del","key":"...","addr":A}   只从保存表里移除，不动 DSP
 *          {"action":"clear"}                        清空保存表（不动 DSP）
 *          {"action":"apply"}                        把保存的全部重新写回 DSP
 *
 * 为什么要设备端存：网页上把 EQ 调好之后，1452 断电重启、或固件映像被自动
 * 重推时，DSP 里的系数会被冲回「录制映像那一刻的值」—— 用户会觉得"调好的 EQ 没了"。
 * 存到设备后，每次 DSP 就绪/映像重推后自动写回。
 *
 * key 是 "cell|parameter"，和首页控件绑定同一套约定：换 DSP 程序后地址会整体平移，
 * 网页按 key 重新解析一次地址再 PUT 回来，所以**不需要改固件**。
 *
 * 桥接服务开启时 POST 一律拒绝（那时 DSP 由 SigmaStudio 管，地址随时可能变）。
 */
static esp_err_t eq_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        eq_band_t bands[EQ_STORE_MAX];
        const int n = eq_store_get(bands, EQ_STORE_MAX);
        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "addr", bands[i].addr);
            cJSON_AddNumberToObject(o, "words", bands[i].words);
            cJSON_AddStringToObject(o, "key", bands[i].key);
            cJSON *w = cJSON_CreateArray();
            for (int k = 0; k < EQ_STORE_WORDS; k++) {
                /* 带符号给出：网页那边要用 >>>0 转回无符号 */
                cJSON_AddItemToArray(w, cJSON_CreateNumber((double)(int32_t)bands[i].w[k]));
            }
            cJSON_AddItemToObject(o, "w", w);
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_AddItemToObject(root, "bands", arr);
        cJSON_AddNumberToObject(root, "count", eq_store_count());
        cJSON_AddNumberToObject(root, "applied", eq_store_applied());
        return send_json(req, root);
    }

    if (bridge_blocks_local_write(req)) {
        return ESP_FAIL;
    }

    char body[640];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *ja = cJSON_GetObjectItem(root, "action");
    const char *act = cJSON_IsString(ja) ? ja->valuestring : "put";

    bool ok = false;
    const char *msg = "";

    if (strcmp(act, "clear") == 0) {
        eq_store_clear();
        ok = true;
        msg = "已清空 EQ 保存表（DSP 当前值未改动）";
    } else if (strcmp(act, "apply") == 0) {
        eq_store_apply();
        const int ap = eq_store_applied();
        ok = (ap > 0);
        msg = ok ? "已把保存的 EQ 写回 DSP"
                 : "写回失败（DSP 离线、或本地控制已关闭）";
    } else if (strcmp(act, "del") == 0) {
        cJSON *jk = cJSON_GetObjectItem(root, "key");
        cJSON *jd = cJSON_GetObjectItem(root, "addr");
        const char *key = cJSON_IsString(jk) ? jk->valuestring : "";
        const uint16_t addr = cJSON_IsNumber(jd) ? (uint16_t)jd->valueint : 0;
        ok = eq_store_del(key, addr);
        msg = ok ? "已从保存表移除" : "没有找到这一段";
    } else {
        cJSON *jd = cJSON_GetObjectItem(root, "addr");
        cJSON *jk = cJSON_GetObjectItem(root, "key");
        cJSON *jw = cJSON_GetObjectItem(root, "w");
        if (!cJSON_IsNumber(jd) || !cJSON_IsArray(jw)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr/w");
            return ESP_FAIL;
        }
        eq_band_t b;
        memset(&b, 0, sizeof(b));
        b.addr  = (uint16_t)jd->valueint;
        b.words = EQ_STORE_WORDS;
        if (cJSON_IsString(jk)) {
            strncpy(b.key, jk->valuestring, sizeof(b.key) - 1);
        }
        int k = 0;
        cJSON *wi = NULL;
        cJSON_ArrayForEach(wi, jw) {
            if (k >= EQ_STORE_WORDS) {
                break;
            }
            b.w[k++] = cJSON_IsNumber(wi) ? (uint32_t)wi->valuedouble : 0;
        }
        const bool wrote = eq_store_put(&b);
        ok = true;                 /* 保存成功即算成功 */
        msg = wrote ? "已保存并写入 DSP"
                    : "已保存（DSP 未写入：离线或本地控制关闭，上电后会再试）";
    }

    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    cJSON_AddStringToObject(resp, "msg", msg);
    cJSON_AddNumberToObject(resp, "count", eq_store_count());
    cJSON_AddNumberToObject(resp, "applied", eq_store_applied());
    return send_json(req, resp);
}

/* ---- /api/log ：GET 取当前日志快照（文本），POST 清空 ---- */
static esp_err_t log_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* 只读快照：不推进读指针，所以不会影响网页的实时推送 */
        char buf[2048];
        size_t n = logbuf_snapshot(buf, sizeof(buf));
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, buf, n);
    }

    net_log_clear();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

/* ---------- HTTP 服务器 ---------- */
static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    /* 实际注册了 31 个 handler（默认 8 个会静默注册失败）——留点余量 */
    cfg.max_uri_handlers = 40;
    /*
     * ⚠️ socket 用满时必须回收最久没用的连接。
     * LWIP_MAX_SOCKETS=10，httpd 默认只拿 7 个；而浏览器对同一源会同时开
     * 多条 keep-alive 连接再加一条 WebSocket。默认 lru_purge_enable=false 时，
     * 池满之后**新连接会一直得不到处理**（不踢旧的，也不回错误），
     * 在浏览器侧表现成 ERR_CONNECTION_TIMED_OUT / WebSocket 一直"连接中…"，
     * 而同时刻用 curl 单独发一个请求却是好的 —— 很容易误判成"设备崩了"。
     */
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t state = {
        .uri = "/api/state", .method = HTTP_GET, .handler = state_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &state);

    httpd_uri_t wifi_get = {
        .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &wifi_get);

    httpd_uri_t wifi_set = {
        .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_set_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &wifi_set);

    httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = false,
    };
    httpd_register_uri_handler(s_server, &ws);

    httpd_uri_t ota = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &ota);

    /* SigmaStudio 桥接配置 */
    httpd_uri_t br_get = {
        .uri = "/api/bridge", .method = HTTP_GET, .handler = bridge_get_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &br_get);

    httpd_uri_t br_set = {
        .uri = "/api/bridge", .method = HTTP_POST, .handler = bridge_set_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &br_set);

    /* USBi（USB 直连 SigmaStudio）开关与状态 */
    httpd_uri_t usbi_get = {
        .uri = "/api/usbi", .method = HTTP_GET, .handler = usbi_get_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &usbi_get);

    httpd_uri_t usbi_set = {
        .uri = "/api/usbi", .method = HTTP_POST, .handler = usbi_set_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &usbi_set);

    /* DSP 程序映像：查询 / 操作 / 下载备份 */
    httpd_uri_t img_get = {
        .uri = "/api/dspimg", .method = HTTP_GET, .handler = dspimg_get_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &img_get);

    httpd_uri_t img_set = {
        .uri = "/api/dspimg", .method = HTTP_POST, .handler = dspimg_set_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &img_set);

    httpd_uri_t img_dl = {
        .uri = "/api/dspimg/download", .method = HTTP_GET,
        .handler = dspimg_download_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &img_dl);

    /* 直接读写任意 DSP 地址 */
    httpd_uri_t dsp_get = {
        .uri = "/api/dsp", .method = HTTP_GET, .handler = dsp_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &dsp_get);

    httpd_uri_t dsp_post = {
        .uri = "/api/dsp", .method = HTTP_POST, .handler = dsp_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &dsp_post);

    /* 日志：GET 取快照 / POST 清空 */
    httpd_uri_t log_get = {
        .uri = "/api/log", .method = HTTP_GET, .handler = log_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &log_get);

    httpd_uri_t log_post = {
        .uri = "/api/log", .method = HTTP_POST, .handler = log_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &log_post);

    httpd_uri_t i2c_get = {
        .uri = "/api/i2c", .method = HTTP_GET, .handler = i2c_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &i2c_get);

    httpd_uri_t i2c_post = {
        .uri = "/api/i2c", .method = HTTP_POST, .handler = i2c_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &i2c_post);

    httpd_uri_t bind_get = {
        .uri = "/api/bind", .method = HTTP_GET, .handler = bind_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &bind_get);

    httpd_uri_t bind_post = {
        .uri = "/api/bind", .method = HTTP_POST, .handler = bind_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &bind_post);

    /* 安全模式开关 */
    httpd_uri_t safe_get = {
        .uri = "/api/safe", .method = HTTP_GET, .handler = safe_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &safe_get);

    httpd_uri_t safe_post = {
        .uri = "/api/safe", .method = HTTP_POST, .handler = safe_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &safe_post);

    httpd_uri_t vol_get = {
        .uri = "/api/vol", .method = HTTP_GET, .handler = vol_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &vol_get);

    httpd_uri_t vol_post = {
        .uri = "/api/vol", .method = HTTP_POST, .handler = vol_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &vol_post);

    httpd_uri_t eq_get = {
        .uri = "/api/eq", .method = HTTP_GET, .handler = eq_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &eq_get);

    httpd_uri_t eq_post = {
        .uri = "/api/eq", .method = HTTP_POST, .handler = eq_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &eq_post);

    httpd_uri_t gr_get = {
        .uri = "/api/graph", .method = HTTP_GET, .handler = graph_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &gr_get);

    httpd_uri_t gr_post = {
        .uri = "/api/graph", .method = HTTP_POST, .handler = graph_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &gr_post);

    /* 两份文件共用同一个 handler，靠 req->uri 里的 "params" 区分 */
    httpd_uri_t gr_nl_get = {
        .uri = "/api/graph/netlist", .method = HTTP_GET, .handler = graph_file_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &gr_nl_get);

    httpd_uri_t gr_nl_post = {
        .uri = "/api/graph/netlist", .method = HTTP_POST, .handler = graph_file_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &gr_nl_post);

    httpd_uri_t gr_pa_get = {
        .uri = "/api/graph/params", .method = HTTP_GET, .handler = graph_file_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &gr_pa_get);

    httpd_uri_t gr_pa_post = {
        .uri = "/api/graph/params", .method = HTTP_POST, .handler = graph_file_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &gr_pa_post);

    return ESP_OK;
}

/* ---------- WiFi 配置 NVS 持久化 ---------- */
static void load_wifi_config_from_nvs(void)
{
    strncpy(s_ap_ssid, DEFAULT_AP_SSID, WIFI_MAX_SSID_LEN);
    strncpy(s_ap_pass, DEFAULT_AP_PASS, WIFI_MAX_PASS_LEN);
    strncpy(s_sta_ssid, DEFAULT_STA_SSID, WIFI_MAX_SSID_LEN);
    strncpy(s_sta_pass, DEFAULT_STA_PASS, WIFI_MAX_PASS_LEN);

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len;
    char buf[WIFI_MAX_PASS_LEN + 1];

    len = sizeof(buf);
    if (nvs_get_str(h, "ap_ssid", buf, &len) == ESP_OK) strncpy(s_ap_ssid, buf, WIFI_MAX_SSID_LEN);
    len = sizeof(buf);
    if (nvs_get_str(h, "ap_pass", buf, &len) == ESP_OK) strncpy(s_ap_pass, buf, WIFI_MAX_PASS_LEN);
    len = sizeof(buf);
    if (nvs_get_str(h, "sta_ssid", buf, &len) == ESP_OK) strncpy(s_sta_ssid, buf, WIFI_MAX_SSID_LEN);
    len = sizeof(buf);
    if (nvs_get_str(h, "sta_pass", buf, &len) == ESP_OK) strncpy(s_sta_pass, buf, WIFI_MAX_PASS_LEN);

    uint8_t u8 = 1;
    if (nvs_get_u8(h, "ap_en", &u8) == ESP_OK)  s_ap_enabled  = (u8 != 0);
    u8 = 1;
    if (nvs_get_u8(h, "sta_en", &u8) == ESP_OK) s_sta_enabled = (u8 != 0);
    /* 两个都关会彻底失联（网页也访问不了），不允许 */
    if (!s_ap_enabled && !s_sta_enabled) {
        s_ap_enabled = true;
    }

    nvs_close(h);
}

static esp_err_t save_wifi_config_to_nvs(const char *ap_ssid, const char *ap_pass,
                                         const char *sta_ssid, const char *sta_pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ap_ssid", ap_ssid);
    nvs_set_str(h, "ap_pass", ap_pass);
    nvs_set_str(h, "sta_ssid", sta_ssid);
    nvs_set_str(h, "sta_pass", sta_pass);
    nvs_set_u8(h, "ap_en", s_ap_enabled ? 1 : 0);
    nvs_set_u8(h, "sta_en", s_sta_enabled ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* 只改开关（不动账号密码）时走这里：立即生效，无需重启 */
esp_err_t net_wifi_set_enabled(bool ap_en, bool sta_en)
{
    /* 两个都关会彻底失联，拒绝 */
    if (!ap_en && !sta_en) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool ap_changed  = (ap_en  != s_ap_enabled);
    const bool sta_changed = (sta_en != s_sta_enabled);

    s_ap_enabled  = ap_en;
    s_sta_enabled = sta_en;
    s_sta_configured = sta_en && (strlen(s_sta_ssid) > 0);

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ap_en", ap_en ? 1 : 0);
        nvs_set_u8(h, "sta_en", sta_en ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }

    if (!ap_changed && !sta_changed) {
        return ESP_OK;
    }

    /*
     * 在线切换模式。注意：关掉 AP 后如果 STA 没连上，网页就访问不到了，
     * 需要重新开机（开机时 NVS 里的开关会生效，所以务必确认 STA 能连再关）。
     */
    wifi_mode_t mode = ap_en ? (sta_en ? WIFI_MODE_APSTA : WIFI_MODE_AP)
                             : WIFI_MODE_STA;
    esp_err_t e = esp_wifi_set_mode(mode);
    if (e != ESP_OK) {
        return e;
    }

    /*
     * 切模式后重新下发接口配置：esp_wifi_set_mode() 之间切换时，
     * 接口配置可能被清掉（尤其从 STA-only 切回带 AP 的模式），
     * 不重设的话 AP 会以默认 SSID/开放认证启动。
     */
    if (ap_en) {
        wifi_config_t ap_cfg = {0};
        strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
        strncpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }

    if (sta_en) {
        if (s_sta_configured) {
            wifi_config_t sta_cfg = {0};
            strncpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
            strncpy((char *)sta_cfg.sta.password, s_sta_pass, sizeof(sta_cfg.sta.password) - 1);
            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
            esp_wifi_connect();
        }
    } else {
        esp_wifi_disconnect();
        s_sta_connected = false;
    }

    /* 切模式后重新确认省电与带宽设置（确保不被底层重置） */
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (sta_en) {
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    }

    ESP_LOGW(TAG, "WiFi 模式已切换：AP=%s STA=%s（mode=%d）",
             ap_en ? "开" : "关", sta_en ? "开" : "关", (int)mode);
    return ESP_OK;
}

/* ---------- WiFi ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_sta_configured) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any = NULL, got_ip = NULL;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                     &wifi_event_handler, NULL, &any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                     &wifi_event_handler, NULL, &got_ip));

    /* 模式由 AP/STA 开关决定（NVS "ap_en"/"sta_en"） */
    wifi_mode_t mode = s_ap_enabled ? (s_sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP)
                                    : WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (s_ap_enabled) {
        wifi_config_t ap_cfg = {0};
        strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
        strncpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    }

    if (s_sta_enabled && strlen(s_sta_ssid) > 0) {
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, s_sta_pass, sizeof(sta_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        s_sta_configured = true;
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * ⚠️ 关闭 WiFi 省电（默认是 WIFI_PS_MIN_MODEM）。
     *
     * 省电模式下 STA 会在信标间隔（DTIM）之间让射频休眠，TCP 收包的唤醒延迟
     * 会有几十毫秒的抖动。SigmaStudio 打开「电平显示」后是**高频轮询**读 DSP 的，
     * 延迟抖动会让它响应超时/请求堆积，直接崩溃。
     * 桥接场景对延迟敏感、对功耗不敏感，所以固定为 WIFI_PS_NONE。
     */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) 失败：%s", esp_err_to_name(ps));
    }

    /*
     * ⚠️ STA 固定 HT20（20MHz 带宽）。
     *
     * 2.4GHz 频段极其拥挤，HT40 在路由器侧经常因为邻频干扰而频繁重传；
     * TCP 一旦丢包就要等 RTO（本工程 1.5s）才会重发，而 SigmaStudio 的
     * 等待极限远小于 1.5s —— 表现就是"偶尔崩一下"。
     * 桥接只需几 KB/s 的带宽（一次电平轮询才 ~72 字节），
     * 所以用 HT20 换稳健性是完全划算的。
     */
    esp_err_t bw = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (bw != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_bandwidth(HT20) 失败：%s", esp_err_to_name(bw));
    }

    if (s_sta_configured) {
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "WiFi started（AP=%s%s，STA=%s）：省电已关闭、STA 固定 HT20",
             s_ap_enabled ? "开" : "关",
             s_ap_enabled ? s_ap_ssid : "",
             s_sta_enabled ? "开" : "关");
}

esp_err_t net_init(const net_callbacks_t *cb)
{
    s_cb = cb;

    load_wifi_config_from_nvs();
    wifi_init();

    /* I2C 时钟/上拉是硬件级参数：在 HTTP 起来之前先恢复（此时 adau1452 已初始化） */
    i2c_config_load();

    /* 连线图数据（SigmaStudio 导出文件）分区：挂了就拿它，失败不影响其它功能 */
    dsp_graph_init();

    esp_err_t ret = start_httpd();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "HTTP server started on port 80, OTA ready");
    return ESP_OK;
}

/* 是否已连接家庭 WiFi（STA 拿到 IP） */
bool net_wifi_connected(void)
{
    return s_sta_connected;
}

void net_get_link_info(net_link_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->sta_connected = s_sta_connected;

    if (s_sta_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
            out->channel = ap.primary;
        }
    }

    esp_netif_ip_info_t ip;
    esp_netif_t *st = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (st != NULL && esp_netif_get_ip_info(st, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
}
