#include "sigma_tcp.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "adau1452.h"
#include "dsp_image.h"
#include "logbuf.h"

static const char *TAG = "sigma_tcp";

/*
 * 逐报文日志（verbose）：只写网页日志环、**不走 UART**。
 * 原因：115200 下每行 ESP_LOGI 要花约 6ms，上一次排查就是这么自己制造出延迟、
 * 反而把 SigmaStudio 拖崩的。logbuf_trace() 只拷进 8KB 环形缓冲，代价极低。
 */
#define TRACE(fmt, ...)                                                        \
    do {                                                                       \
        if (s_cfg.verbose) {                                                   \
            logbuf_trace("Σ " fmt, ##__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#define SIGMA_CMD_WRITE   0x09
#define SIGMA_CMD_READ    0x0A
#define SIGMA_CMD_RESP    0x0B
#define SIGMA_HDR_SIZE    14
#define SIGMA_STAGE       256          /* 转发暂存块（4 的倍数，整字对齐） */
#define SIGMA_MAX_WRITE   (4u << 20)   /* 单条写报文的数据上限，超过按错位处理 */

/*
 * ⚠️ 本文件最重要的一条纪律：**永远不能让 TCP 流失去对齐**。
 *
 * SigmaStudio 对报文流的假设非常脆弱：一旦我们少读/多读一个字节，
 * 它后面的报文全部错位，接着就会卡死或直接崩溃（实测"一读就崩"）。
 * 所以：
 *   - 收到写报文后，无论 I2C 成功与否，都必须把 data_length 字节**全部读掉**；
 *   - 遇到无法识别的命令字节不要立刻断开连接，记录后按字节滑动重同步，
 *     否则断线会让 SigmaStudio 直接崩；
 *   - 读请求必须**严格按请求长度**回包（哪怕数据取不到，也要补零凑够长度）。
 */

static volatile bool s_stop;
static volatile bool s_running;
static TaskHandle_t  s_task;
static int           s_listen_fd = -1;
static uint16_t      s_port = SIGMA_TCP_DEFAULT_PORT;

static sigma_tcp_status_t s_st;

/*
 * 活跃连接 fd 表。
 *
 * ⚠️⚠️ 这张表是为了修一个**致命的设计缺陷**：
 *
 * 旧实现只有一个任务，`accept()` 拿到 fd 后直接阻塞在 `handle_session()` 里，
 * 直到这条连接结束才回到 accept。也就是说**同一时刻只能服务一条连接**，
 * 而且 `listen()` 的 backlog 还只有 1。
 *
 * 而 SigmaStudio 打开「电平显示」这类实时面板时，会**另开一条 TCP 连接**
 * 给它的高频轮询线程用。第二条连接握手能完成（内核代答），但请求永远没人处理
 * —— 它等约 2.7 秒超时后内部状态错乱，整个进程野指针崩溃（0xc0000005），
 * 把两条 socket 一起 RST 掉。
 *
 * 这完美对应了所有现象：开一个电平正常、开两个就崩；后来"一开级别就崩"；
 * 网页状态反复跳断（client 在跳）；以及日志里的"最后静默 2753ms + errno=104"。
 *
 * 修复：accept 后为**每条连接**创建一个独立任务，互不阻塞。
 * I2C 总线本来就由 adau1452 内部的递归互斥锁串行化，所以并发访问是安全的。
 */
#define SIGMA_MAX_CONN   4

static portMUX_TYPE s_fd_mux = portMUX_INITIALIZER_UNLOCKED;
static int          s_fds[SIGMA_MAX_CONN];      /* 未用槽位为 -1 */

static void fds_init(void)
{
    for (int i = 0; i < SIGMA_MAX_CONN; i++) {
        s_fds[i] = -1;
    }
}

static void fd_add(int fd)
{
    portENTER_CRITICAL(&s_fd_mux);
    for (int i = 0; i < SIGMA_MAX_CONN; i++) {
        if (s_fds[i] < 0) {
            s_fds[i] = fd;
            break;
        }
    }
    portEXIT_CRITICAL(&s_fd_mux);
}

static void fd_del(int fd)
{
    portENTER_CRITICAL(&s_fd_mux);
    for (int i = 0; i < SIGMA_MAX_CONN; i++) {
        if (s_fds[i] == fd) {
            s_fds[i] = -1;
            break;
        }
    }
    portEXIT_CRITICAL(&s_fd_mux);
}

static int fd_count(void)
{
    /*
     * ⚠️ 必须先判 s_running！
     *
     * 曾经出过一个很隐蔽的 bug：`s_fds[]` 只在 `sigma_tcp_start()` 里被
     * `fds_init()` 初始化成 -1。如果**本次开机桥接从未启动过**（用户把它关了），
     * `s_fds[]` 就是 BSS 的零初始化 = 全 0，而本函数只判 `>= 0`，
     * 于是把 4 个空槽当成 4 条有效连接 ——
     * 结果是 `sigma_tcp_get_status()` 报 `client=true, active=4`，
     * 网页显示"SigmaStudio 调试中"，而屏幕用的 `sigma_tcp_busy()`
     * （它额外要求 s_running）却认为是电平页 → **两边不一致**。
     *
     * 现在服务没跑就直接返回 0，从根上消除这个不一致。
     */
    if (!s_running) {
        return 0;
    }
    int n = 0;
    portENTER_CRITICAL(&s_fd_mux);
    for (int i = 0; i < SIGMA_MAX_CONN; i++) {
        if (s_fds[i] >= 0) {
            n++;
        }
    }
    portEXIT_CRITICAL(&s_fd_mux);
    return n;
}

/* 服务停止时把所有会话 socket 踢掉，让阻塞中的 recv 立刻返回 */
static void fds_shutdown_all(void)
{
    portENTER_CRITICAL(&s_fd_mux);
    for (int i = 0; i < SIGMA_MAX_CONN; i++) {
        if (s_fds[i] >= 0) {
            shutdown(s_fds[i], SHUT_RDWR);
        }
    }
    portEXIT_CRITICAL(&s_fd_mux);
}

/* 配置：定义放前面，因为报文处理函数要用到 */
static sigma_tcp_config_t s_cfg = {
    .enabled = false,
    .local_control = true,
    .verbose = false,
    .resp_gap_us = SIGMA_RESP_GAP_DEFAULT_US,
    .port = SIGMA_TCP_DEFAULT_PORT,
};

/* ---------- 小工具 ---------- */
static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static bool recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static bool send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/* ==================== 接收流缓冲 ====================
 * 用带缓冲的流式解析代替"每次精确 recv 固定字节数"：
 * 这样可以**按字节滑动重同步**，即使报文头长度假设错了也能自愈，
 * 不会因为错位把后续报文全毁掉（那是 SigmaStudio 崩溃的直接原因）。
 */
#define RX_CAP   1024

/* 接收流缓冲：每个连接一份，rx_need() 按需从 socket 补足字节。
 * 用缓冲而不是"每次精确 recv 固定字节数"，是为了能按字节滑动重同步。
 */
typedef struct {
    uint8_t b[RX_CAP];
    size_t  n;
} rx_t;

static bool rx_fill(int fd, rx_t *r)
{
    if (r->n >= sizeof(r->b)) {
        return false;                    /* 缓冲满仍不足一条头，异常 */
    }
    int k = recv(fd, r->b + r->n, sizeof(r->b) - r->n, 0);
    if (k <= 0) {
        return false;                    /* 对端关闭、接收超时或 socket 出错 */
    }

    r->n += (size_t)k;
    return true;
}

/* 保证缓冲内至少有 need 字节 */
static bool rx_need(int fd, rx_t *r, size_t need)
{
    while (r->n < need) {
        if (!rx_fill(fd, r)) {
            return false;
        }
    }
    return true;
}

/* 丢弃缓冲头部 k 字节 */
static void rx_drop(rx_t *r, size_t k)
{
    if (k >= r->n) {
        r->n = 0;
        return;
    }
    memmove(r->b, r->b + k, r->n - k);
    r->n -= k;
}

/* 从流里取 n 字节（先取缓冲内已有的，再从 socket 读），用于消费报文负载 */
static bool stream_read(int fd, rx_t *r, uint8_t *dst, size_t n)
{
    size_t take = (n < r->n) ? n : r->n;
    if (take > 0) {
        memcpy(dst, r->b, take);
        rx_drop(r, take);
        dst += take;
        n -= take;
    }
    if (n > 0) {
        return recv_all(fd, dst, n);
    }
    return true;
}

/* ---------- 每连接上下文 ----------
 * 支持多条连接并发（SigmaStudio 会为不同的实时面板各开一条），
 * 因此所有会话状态都必须是**每连接一份**，不能再用 static 变量。
 */
typedef struct {
    int      fd;
    char     ip[16];
    uint16_t port;
    rx_t     rx;                 /* 约 1KB，放堆上避免每条连接吃掉大块栈 */

    /* 本次会话统计 */
    uint32_t lat_max_us;
    uint32_t i2c_max_us;
    uint32_t lat_last_us;
    uint32_t lat_over_10ms;
    uint64_t lat_sum;
    uint32_t lat_n;
    uint64_t i2c_sum;            /* I2C 耗时累加：算均值用 */
    uint32_t errors;
    uint32_t resync_bytes;
} sess_t;

/* 每个连接独立收发，写 status 的累加字段用临界区保护 */
static void st_bump(uint32_t *field)
{
    portENTER_CRITICAL(&s_fd_mux);
    (*field)++;
    portEXIT_CRITICAL(&s_fd_mux);
}

/* ---------- 读请求处理 ----------
 * ⚠️ 这里有两个曾经让 SigmaStudio 崩溃/卡死的坑，务必保持现在的写法：
 *
 * 1) **绝不能改写请求里的长度**。曾经把 >4096 的请求"静默截断"成 4096 后照发，
 *    但 SigmaStudio 是按它自己请求的长度读取的 —— 流的对齐从此错位，必然崩溃。
 *    现在严格按请求长度回包（超出上限时也照还等长数据，只把 success 置 1）。
 *
 * 2) **不能为大读取申请大块连续内存**。`malloc(14+len)` 在本机碎片化堆上很容易失败
 *    （实测 free 够但最大连续块仅 ~12KB），而失败时若直接 return 不回包，
 *    SigmaStudio 会一直等响应 → 卡死/崩溃。现在改成先读首块（拿准 success），
 *    再分块读+发，全程只用栈缓冲，任意长度都稳。
 *
 * 3) **整个响应必须一次 send 出去**（见下面 RESP_MERGE 的说明）。
 */
/* 每条连接都要在栈上开一块这个大小的缓冲，所以不宜过大。
 * 电平读只有 4~8 字节；我们自己的 I2C 块也只有 96 字节；512 绰绰有余，
 * 更大的读会走下面"余量"分支分多次 send（那类读 SigmaStudio 本来就会循环收）。 */
#define RESP_MERGE 512

static void handle_read(sess_t *s, uint8_t chip_addr, uint16_t addr, uint32_t len)
{
    const int fd = s->fd;
    const int64_t t0 = esp_timer_get_time();

    if (len == 0) {
        len = 4;                       /* 零长度没意义，按一个字回 */
    }

    /*
     * ⚠️⚠️ 本函数最关键的一条纪律：**整个响应（头 + 数据）必须一次 send 出去**。
     *
     * 曾经写成 send(头,14) 然后 send(数据,N) 两次调用 —— 在 TCP 上这是**两个
     * 独立的报文段**。SigmaStudio 把每个请求的响应当成一次完整接收，先到的
     * 14 字节头被它取走，那 4 字节数据还留在它的接收缓冲里，于是下一次解析
     * 就把这 4 个字节当成新的报文头 —— 从此彻底错位，最终崩溃。
     *
     * 这也解释了现象为什么会变差：最早是整包 malloc 后**一次** send，那时
     * 只开一个电平显示是正常的；改成流式两次 send 之后立刻恶化成"一开就崩"。
     * 所以现在把「头 + 数据」拼进同一个缓冲，一次发出。
     *
     * 大读（len > RESP_MERGE）无法避免多次 send，但那一类读 SigmaStudio
     * 本来就必须循环接收，而电平读数都是 4 字节。
     */
    uint8_t  out[SIGMA_HDR_SIZE + RESP_MERGE];
    uint8_t *dat = out + SIGMA_HDR_SIZE;
    const bool is_mem = (addr < ADAU1452_ADDR_REG_BASE);   /* DSP 内存按字寻址 */

    size_t first = (len < RESP_MERGE) ? (size_t)len : (size_t)RESP_MERGE;
    if (is_mem && first >= 4) {
        first = (first / 4) * 4;       /* 内存按字，尾部保持整字 */
    }

    /* 先把第一块读出来，这样 success 字节能反映真实结果 */
    esp_err_t e = adau1452_read_block(addr, dat, first);
    const int64_t t_i2c = esp_timer_get_time();   /* I2C + 拿锁 到此结束 */
    if (e != ESP_OK) {
        s->errors++;
        st_bump(&s_st.errors);
        memset(dat, 0, first);
    }

    /* ---- 响应头 ----
     * total_length 用 AD wiki 的写法：**整包长 = 14 + 数据长**。
     * 曾把它做成运行时可切换（full / echo / hdr14）去排查"下载正常但一读就崩"，
     * 结果三种完全一致 —— 真因是**应答之间没有间隔**，不在这个字段。
     */
    out[0] = SIGMA_CMD_RESP;
    put_be32(out + 1, SIGMA_HDR_SIZE + len);
    out[5] = chip_addr;
    put_be32(out + 6, len);
    out[10] = (uint8_t)(addr >> 8);
    out[11] = (uint8_t)(addr & 0xFF);
    out[12] = (e == ESP_OK) ? 0 : 1;   /* 0 = 成功，1 = 失败 */
    out[13] = 0;

    /* ---- 头 + 数据：**单次 send**（关键修复） ---- */
    if (!send_all(fd, out, SIGMA_HDR_SIZE + first)) {
        s->errors++;
        st_bump(&s_st.errors);
        return;
    }

    /* ---- 余量：只有超大读才会走到这里 ---- */
    uint32_t done = (uint32_t)first;
    while (done < len) {
        size_t n = len - done;
        if (n > RESP_MERGE) {
            n = RESP_MERGE;
        }
        if (is_mem && n >= 4) {
            n = (n / 4) * 4;
        }
        uint16_t sub = is_mem ? (uint16_t)(addr + done / 4)
                              : (uint16_t)(addr + done);
        esp_err_t r = adau1452_read_block(sub, dat, n);
        if (r != ESP_OK) {
            s->errors++;
            st_bump(&s_st.errors);
            memset(dat, 0, n);         /* 保长度，绝不破坏帧对齐 */
        }
        if (!send_all(fd, dat, n)) {
            s->errors++;
            st_bump(&s_st.errors);
            return;
        }
        done += (uint32_t)n;
    }

    st_bump(&s_st.pkt_read);
    s_st.last_addr = addr;
    s_st.last_len = len;
    s_st.last_is_read = true;
    TRACE("READ 0x%04X len=%lu", addr, (unsigned long)len);

    /* ---- 耗时统计：把"慢"拆成 I2C / 网络+调度 两部分 ---- */
    const int64_t t_end  = esp_timer_get_time();
    const uint32_t i2c_us = (uint32_t)(t_i2c - t0);
    const uint32_t tot_us = (uint32_t)(t_end - t0);
    if (tot_us > s->lat_max_us) {
        s->lat_max_us = tot_us;
    }
    if (i2c_us > s->i2c_max_us) {
        s->i2c_max_us = i2c_us;
    }
    if (tot_us > 10000) {
        s->lat_over_10ms++;
    }
    s->lat_sum += tot_us;
    s->i2c_sum += i2c_us;
    s->lat_n++;

    /* 汇总到全局状态（供网页显示；多连接时取"最差"值更有诊断价值） */
    s_st.lat_last_us = tot_us;
    s_st.lat_max_us = s->lat_max_us;
    s_st.i2c_max_us = s->i2c_max_us;
    s_st.lat_over_10ms = s->lat_over_10ms;
    s_st.lat_avg_us = (s->lat_n > 0) ? (uint32_t)(s->lat_sum / s->lat_n) : 0;

    /*
     * ⚠️⚠️ 关键修复：应答之间强制插一个间隔。
     *
     * SigmaStudio 把读请求**流水线**发出（实测一个 TCP 段里塞 3 个读请求），
     * 但它按"一次 recv 拿到一条应答"解析。我们三条应答紧挨着发出时，
     * TCP 会合并成一个段 → 它错位解析 → 野指针崩溃（0xc0000005）。
     *
     * 这个延时让 TCP 栈把上一条应答真实发出去（而不只是排进队列）再处理下一条。
     *
     * ⚠️ 间隔必须**大于 SigmaStudio 处理一条应答的时间**。实测临界值：
     *   2000 / 5000µs → 照旧崩
     *   15000µs       → 坚持一会儿才崩（不够，在临界值附近）
     *   30000µs       → 长时间不崩 ✓（当前默认）
     * 理论上只有"还有请求排队"时才需要延时，但判断有竞争风险，所以一律延时。
     *
     * 注意这是**忙等**而非 vTaskDelay：tick 是 10ms，粒度太粗。
     */
    if (s_cfg.resp_gap_us) {
        /*
         * 上限保护：resp_gap_us 已改为 32 位。esp_rom_delay_us 是忙等，
         * 200ms 已是极端值，再大就会把 SigmaStudio 拖超时了。
         */
        esp_rom_delay_us(s_cfg.resp_gap_us > SIGMA_RESP_GAP_MAX_US
                         ? SIGMA_RESP_GAP_MAX_US : s_cfg.resp_gap_us);
    }
}

/* ---------- 写请求体 ----------
 * 流式边收边转发，不整包缓存。
 * 关键：**I2C 出错也必须把这条报文的全部数据读完**，否则后续报文错位 → SigmaStudio 崩溃。
 * 所以本函数只在 socket 断开时返回 false，其余情况一律返回 true。 */
static bool handle_write_body(sess_t *s, uint16_t addr, uint32_t data_len)
{
    const int fd = s->fd;
    rx_t *rx = &s->rx;
    const bool is_reg = (addr >= ADAU1452_ADDR_REG_BASE);
    uint8_t stage[SIGMA_STAGE];
    size_t staged = 0;
    uint32_t remaining = data_len;
    uint32_t byte_off = 0;
    int i2c_err = 0;                     /* 记一次错误日志就够了，不再反复刷屏 */
    uint32_t dropped = 0;

    while (remaining > 0) {
        size_t want = remaining;
        if (want > sizeof(stage) - staged) {
            want = sizeof(stage) - staged;
        }
        if (!stream_read(fd, rx, stage + staged, want)) {
            return false;                /* socket 断了，只能结束 */
        }
        staged += want;
        remaining -= want;

        /* 攒满缓冲区、或数据已收完，才提交给 I2C */
        if (remaining > 0 && staged < sizeof(stage)) {
            continue;
        }

        size_t commit = staged;
        if (!is_reg) {
            commit &= ~(size_t)3;        /* DSP 内存必须整字（4 字节）提交 */
        }
        if (commit == 0) {
            continue;                    /* 不足整字，先留着（字节已从流消费掉） */
        }

        uint16_t sub = (uint16_t)(addr + (is_reg ? byte_off : byte_off / 4));

        if (i2c_err == 0) {
            /* 先录制（即使 I2C 失败也保留数据，便于事后分析），再写 DSP */
            dsp_image_record(sub, stage, commit);
            esp_err_t e = adau1452_write_block(sub, stage, commit);
            if (e != ESP_OK) {
                i2c_err++;
                ESP_LOGW(TAG, "写 0x%04X (%u 字节) 失败：%s（继续排空数据以保持流对齐）",
                         sub, (unsigned)commit, esp_err_to_name(e));
                TRACE("WRITE 0x%04X +%u 失败", sub, (unsigned)commit);
            } else {
                TRACE("WRITE 0x%04X +%u", sub, (unsigned)commit);
            }
        } else {
            dropped += (uint32_t)commit;
        }

        byte_off += (uint32_t)commit;
        staged -= commit;
        if (staged > 0) {
            memmove(stage, stage + commit, staged);
        }
    }

    if (staged > 0) {
        ESP_LOGW(TAG, "内存写入长度 %lu 不是 4 的倍数，尾部 %u 字节未提交",
                 (unsigned long)data_len, (unsigned)staged);
    }
    if (i2c_err) {
        s->errors++;
        st_bump(&s_st.errors);
        ESP_LOGW(TAG, "本报文因 I2C 错误跳过 %lu 字节", (unsigned long)dropped);
    }
    return true;
}

/* ---------- 会话循环（每条连接一个任务） ---------- */
static void handle_session(sess_t *s)
{
    const int fd = s->fd;
    rx_t *const rx = &s->rx;             /* 指针，下文一律用 rx-> */

    struct timeval tv = { .tv_sec = 300, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    rx->n = 0;

    uint32_t resync = 0;
    /*
     * 退出原因。这一行是排查 SigmaStudio 崩溃最关键的证据，因为两侧的
     * 失败方式完全不同：
     *   "对端关闭/超时" → SigmaStudio 自己先断了（它崩了/主动重连）
     *   "长度异常"      → 我们主动断的（我们解析出了问题）
     */
    const char *why = "服务停止";

    while (!s_stop) {
        /* 先保证 12 字节：operation(1) + total_length(4) + chip(1)
         *              + data_length(4) + address(2)
         * 这 12 字节对 12/14 两种头布局是**完全一致**的，
         * 差异只在后面那 2 个 reserved 字节，因此先按 12 解析最稳。 */
        if (!rx_need(fd, rx, 12)) {
            why = "对端关闭或接收超时（等报文头时）";
            break;
        }

        const uint8_t  cmd   = rx->b[0];
        const uint32_t total = be32(rx->b + 1);
        const uint32_t dlen  = be32(rx->b + 6);

        if (cmd == SIGMA_CMD_WRITE) {
            /* 写头固定 14 字节：operation(0) safeload(1) channel(2)
             * total_length(3..6) chip_address(7) data_length(8..11) address(12..13) */
            const size_t hdr_len = 14;
            if (!rx_need(fd, rx, hdr_len)) {
                why = "对端关闭或接收超时（读写报文头时）";
                break;
            }
            const uint8_t safeload = rx->b[1];
            const uint8_t chip     = rx->b[7];
            const uint16_t waddr   = be16(rx->b + 12);
            const uint32_t wtotal  = be32(rx->b + 3);
            const uint32_t wdlen   = be32(rx->b + 8);

            if (wtotal != wdlen + 14) {
                ESP_LOGW(TAG, "写报文 total=%lu dataLen=%lu 不匹配（差 %ld）",
                         (unsigned long)wtotal, (unsigned long)wdlen,
                         (long)wtotal - (long)wdlen - 14);
                s->errors++;
                st_bump(&s_st.errors);
            }

            rx_drop(rx, hdr_len);

            if (wdlen > SIGMA_MAX_WRITE) {
                ESP_LOGE(TAG, "写长度异常 %lu，断开", (unsigned long)wdlen);
                s->errors++;
                st_bump(&s_st.errors);
                why = "写长度异常（我们主动断开）";
                break;
            }
            if (!handle_write_body(s, waddr, wdlen)) {
                why = "写数据时对端关闭";
                break;                   /* socket 断开 */
            }

            st_bump(&s_st.pkt_write);
            s_st.bytes += wdlen;       /* 仅显示用，非精确计数 */
            s_st.last_addr = waddr;
            s_st.last_len = wdlen;
            s_st.last_is_read = false;
            s_st.last_safeload = safeload;

            /* 写报文平时不刷日志（高频），每 32 条打一条便于确认服务还活着 */
            if ((s_st.pkt_write % 32) == 0) {
                ESP_LOGI(TAG, "写报文 %lu 个（最近 0x%04X +%lu chip=0x%02X）",
                         (unsigned long)s_st.pkt_write, waddr, (unsigned long)wdlen,
                         chip);
            }
            TRACE("WRITE hdr 0x%04X dlen=%lu chip=0x%02X safeload=%u",
                  waddr, (unsigned long)wdlen, chip, safeload);

        } else if (cmd == SIGMA_CMD_READ) {
            /* 读请求头可能是 12 或 14 字节（不同 SigmaStudio 版本/芯片家族不一致，
             * 参考实现里也专门为这个差异打过补丁）。
             * 用 total_length 与 data_length 的关系反推，两种约定都能覆盖：
             *   - total = 头长 + 数据长（SigmaStudio 预先声明响应总长）→ 头长 = total - dlen
             *   - total = 头长（仅头）→ 头长 = total
             * 判定不出来时按 ADAU14xx 的 14 处理。 */
            size_t hdr_len;
            long sz = (long)total - (long)dlen;
            if (sz == 12 || sz == 14) {
                hdr_len = (size_t)sz;
            } else if (total == 12 || total == 14) {
                hdr_len = (size_t)total;
            } else {
                hdr_len = 14;
                ESP_LOGW(TAG, "读头长度无法判定（total=%lu dlen=%lu），按 14 处理",
                         (unsigned long)total, (unsigned long)dlen);
            }

            if (!rx_need(fd, rx, hdr_len)) {
                why = "对端关闭或接收超时（读报文头时）";
                break;
            }
            const uint8_t  chip = rx->b[5];
            const uint32_t rdlen = be32(rx->b + 6);
            const uint16_t raddr = be16(rx->b + 10);
            rx_drop(rx, hdr_len);

            if (rdlen > SIGMA_MAX_WRITE) {
                ESP_LOGE(TAG, "读长度异常 %lu，断开", (unsigned long)rdlen);
                s->errors++;
                st_bump(&s_st.errors);
                why = "读长度异常（我们主动断开）";
                break;
            }

            handle_read(s, chip, raddr, rdlen);

        } else {
            /* 未知命令字节：几乎一定是错位。**绝不能断开连接**（会让 SigmaStudio 崩溃），
             * 只丢弃 1 个字节继续滑动，最多滑动 256 次仍无法同步才放弃。 */
            s->errors++;
            st_bump(&s_st.errors);
            if (resync < 8) {
                ESP_LOGW(TAG, "未知命令 0x%02X（疑似流错位，逐字节重同步中）", cmd);
            }
            rx_drop(rx, 1);
            resync++;
            s->resync_bytes++;
            st_bump(&s_st.resync_bytes);
            if (resync > 256) {
                ESP_LOGE(TAG, "连续滑动 %lu 字节仍未同步，断开", (unsigned long)resync);
                why = "无法重同步（我们主动断开）";
                break;
            }
            continue;
        }

        resync = 0;
    }

    /* 退出原因：区分"对端先关"（SigmaStudio 崩了/退出了）与"我们主动断开"。
     * 对端地址、端口与剩余连接数在 session_task 的收尾日志里打。 */
    ESP_LOGW(TAG, "会话结束原因：%s", why);
}

/* ---------- 会话任务：每条连接一个，互相不阻塞 ---------- */
static void session_task(void *arg)
{
    sess_t *s = (sess_t *)arg;

    handle_session(s);

    fd_del(s->fd);
    shutdown(s->fd, SHUT_RDWR);
    close(s->fd);
    st_bump(&s_st.disconnects);
    portENTER_CRITICAL(&s_fd_mux);
    if (s_st.active > 0) {
        s_st.active--;
    }
    if (s_st.active == 0) {
        s_st.client_ip[0] = '\0';
    }
    portEXIT_CRITICAL(&s_fd_mux);

    ESP_LOGI(TAG, "会话结束：%s:%u（剩余 %d 条连接）", s->ip, (unsigned)s->port, fd_count());

    free(s);
    vTaskDelete(NULL);
}

/* ---------- 任务 ---------- */
static void sigma_tcp_task(void *arg)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(s_port);

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket 创建失败");
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(s_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        ESP_LOGE(TAG, "绑定端口 %u 失败", s_port);
        close(s_listen_fd);
        s_listen_fd = -1;
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /*
     * backlog 必须 > 1：SigmaStudio 会并发开多条连接，
     * 旧代码写死 1 会在它快速重连时把握手丢掉。
     */
    if (listen(s_listen_fd, SIGMA_MAX_CONN) != 0) {
        ESP_LOGE(TAG, "listen 失败");
        close(s_listen_fd);
        s_listen_fd = -1;
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_running = true;
    ESP_LOGI(TAG, "SigmaStudio 桥接已启动，监听 TCP 端口 %u（支持 %d 条并发连接）",
             s_port, SIGMA_MAX_CONN);

    /* 用超时让 accept 周期性返回，以便检查停止标志 */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!s_stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            continue;
        }

        if (fd_count() >= SIGMA_MAX_CONN) {
            ESP_LOGW(TAG, "并发连接已达上限 %d，拒绝新连接", SIGMA_MAX_CONN);
            close(fd);
            continue;
        }

        /* 每条连接一个独立上下文（rx 缓冲约 1KB，放堆上） */
        sess_t *s = (sess_t *)calloc(1, sizeof(sess_t));
        if (s == NULL) {
            ESP_LOGW(TAG, "分配会话上下文失败，拒绝连接");
            close(fd);
            continue;
        }
        s->fd = fd;
        s->port = ntohs(peer.sin_port);
        inet_ntoa_r(peer.sin_addr, s->ip, sizeof(s->ip));

        st_bump(&s_st.conn_count);
        st_bump(&s_st.active);
        strncpy(s_st.client_ip, s->ip, sizeof(s_st.client_ip) - 1);
        fd_add(fd);

        /*
         * ⚠️ 关键：**为每条连接单独起任务**，accept 立刻返回去接下一根。
         * 旧实现是在这里直接 handle_session() 阻塞到底，第二条连接
         * （SigmaStudio 的实时面板会用到）永远拿不到响应 → 它超时崩溃。
         * 端口号能区分是哪条连接，排查时很有用。
         */
        char name[16];
        snprintf(name, sizeof(name), "sigma_c%d", (int)s_st.conn_count);
        if (xTaskCreate(session_task, name, 4096, s, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "创建会话任务失败，断开");
            fd_del(fd);
            close(fd);
            free(s);
            continue;
        }
        ESP_LOGI(TAG, "SigmaStudio 已连接：%s:%u（第 %lu 次，当前 %d 条）",
                 s->ip, (unsigned)s->port, (unsigned long)s_st.conn_count, fd_count());
    }

    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    s_running = false;
    s_task = NULL;
    ESP_LOGI(TAG, "SigmaStudio 桥接已停止");
    vTaskDelete(NULL);
}

/* ---------- 对外接口 ---------- */
esp_err_t sigma_tcp_start(uint16_t port)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_port = (port == 0) ? SIGMA_TCP_DEFAULT_PORT : port;
    s_stop = false;
    memset(&s_st, 0, sizeof(s_st));
    s_st.port = s_port;
    fds_init();                          /* 清空活跃连接表 */

    /* 先等旧任务收尾，避免端口占用 */
    for (int i = 0; i < 50 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /*
     * 任务优先级 6：高于 httpd（默认 5）与主循环（1）。
     * SigmaStudio 打开电平显示后会高频轮询，桥接响应必须优先于网页推送/渲染，
     * 否则响应延迟抖动会让 SigmaStudio 超时崩溃。
     */
    if (xTaskCreate(sigma_tcp_task, "sigma_tcp", 6144, NULL, 6, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 等监听就绪 */
    for (int i = 0; i < 100 && !s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (s_task == NULL) {
            return ESP_FAIL;
        }
    }
    return s_running ? ESP_OK : ESP_FAIL;
}

esp_err_t sigma_tcp_stop(void)
{
    if (s_task == NULL && !s_running) {
        return ESP_OK;
    }

    s_stop = true;

    /* 强制关闭所有会话 socket，让阻塞中的 recv 立刻返回 */
    fds_shutdown_all();
    if (s_listen_fd >= 0) {
        shutdown(s_listen_fd, SHUT_RDWR);
    }

    for (int i = 0; i < 150 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /*
     * 服务已停，把连接表清空。
     * fds_shutdown_all() 只是 shutdown 了 socket，槽位要等会话任务自己调用
     * fd_del() 才会释放；如果任务没来得及收尾，残留的槽位会让
     * "还有连接"的误判延续到下次启动前。
     */
    fds_init();

    s_st.client = false;
    s_st.active = 0;
    s_st.client_ip[0] = '\0';
    return ESP_OK;
}

void sigma_tcp_get_status(sigma_tcp_status_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_st;
    out->running = s_running;
    out->port = s_port;
    /* 活跃连接数是权威来源，避免与任务退出顺序竞争 */
    out->active = (uint32_t)fd_count();
    out->client = (out->active > 0);
}

bool sigma_tcp_busy(void)
{
    return s_running && (fd_count() > 0);
}

/* ================= 配置持久化（NVS namespace "bridge"） ================= */
#define CFG_NS   "bridge"

static void config_save(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败，配置未持久化");
        return;
    }
    nvs_set_u8(h, "en", s_cfg.enabled ? 1 : 0);
    nvs_set_u8(h, "local", s_cfg.local_control ? 1 : 0);
    nvs_set_u8(h, "vbs", s_cfg.verbose ? 1 : 0);
    nvs_set_u32(h, "rgap", s_cfg.resp_gap_us);
    nvs_set_u16(h, "port", s_cfg.port);
    nvs_commit(h);
    nvs_close(h);
}

sigma_tcp_config_t sigma_tcp_get_config(void)
{
    return s_cfg;
}

esp_err_t sigma_tcp_config_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t u8 = 0;
        uint16_t u16 = 0;
        uint32_t u32 = 0;
        if (nvs_get_u8(h, "en", &u8) == ESP_OK)       s_cfg.enabled = (u8 != 0);
        if (nvs_get_u8(h, "local", &u8) == ESP_OK)    s_cfg.local_control = (u8 != 0);
        if (nvs_get_u8(h, "vbs", &u8) == ESP_OK)      s_cfg.verbose = (u8 != 0);
        if (nvs_get_u32(h, "rgap", &u32) == ESP_OK && u32 <= SIGMA_RESP_GAP_MAX_US) {
            s_cfg.resp_gap_us = u32;
        }
        if (nvs_get_u16(h, "port", &u16) == ESP_OK && u16 >= 1024) s_cfg.port = u16;
        nvs_close(h);
    }
    if (s_cfg.port == 0) {
        s_cfg.port = SIGMA_TCP_DEFAULT_PORT;
    }

    ESP_LOGI(TAG, "桥接配置：enabled=%d port=%u local=%d verbose=%d 应答间隔=%luus",
             s_cfg.enabled, s_cfg.port, s_cfg.local_control, s_cfg.verbose,
             (unsigned long)s_cfg.resp_gap_us);
    return ESP_OK;
}

esp_err_t sigma_tcp_init(void)
{
    sigma_tcp_config_load();

    if (s_cfg.enabled && s_task == NULL) {
        return sigma_tcp_start(s_cfg.port);
    }
    return ESP_OK;
}

esp_err_t sigma_tcp_set_verbose(bool on)
{
    if (s_cfg.verbose == on) {
        return ESP_OK;
    }
    s_cfg.verbose = on;
    config_save();
    ESP_LOGW(TAG, "逐报文日志：%s", on ? "开（会产生大量日志）" : "关");
    return ESP_OK;
}

bool sigma_tcp_verbose(void)
{
    return s_cfg.verbose;
}

esp_err_t sigma_tcp_set_config(bool enabled, uint16_t port, bool local_control,
                               uint32_t resp_gap_us)
{
    if (port != 0 && port < 1024) {
        return ESP_ERR_INVALID_ARG;
    }
    if (resp_gap_us > SIGMA_RESP_GAP_MAX_US) {
        resp_gap_us = SIGMA_RESP_GAP_DEFAULT_US;
    }

    const bool port_changed = (port != 0 && port != s_cfg.port);
    const bool gap_changed = (resp_gap_us != s_cfg.resp_gap_us);

    s_cfg.enabled = enabled;
    s_cfg.local_control = local_control;
    s_cfg.resp_gap_us = resp_gap_us;
    if (gap_changed) {
        /* 明确打一条日志，方便确认网页上的改动真的生效了 */
        ESP_LOGW(TAG, "读应答间隔改为 %lu us（越大越稳但刷新越慢；0 = 关闭）",
                 (unsigned long)resp_gap_us);
    }
    if (port != 0) {
        s_cfg.port = port;
    }
    config_save();

    if (!enabled) {
        sigma_tcp_stop();
        return ESP_OK;
    }
    if (port_changed || !s_running) {
        sigma_tcp_stop();
        return sigma_tcp_start(s_cfg.port);
    }
    return ESP_OK;
}
