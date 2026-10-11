#include "ui.h"
#include "st7789.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"

static const char *TAG = "ui";

/* 上下屏分界线：上为电平表区，下为音量区（各占半个屏幕） */
#define SPLIT_Y       120

/* 能量条左右不留白，铺满整行 */
#define BAR_INSET     0

/* 能量条高度（粗细） */
#define BAR_HEIGHT    24

/* 状态栏高度（状态点 + 文字，变化时才刷新） */
#define STATUS_H      12

/* L / R 电平标签的起始 y（能量条紧跟其后 +16） */
#define LEVEL_L_Y     14
#define LEVEL_R_Y     64

/* ---------- 内部渲染状态：电平平滑 + 峰值保持 + 刷新快照 ---------- */
static float s_disp_l = -999.0f;
static float s_disp_r = -999.0f;
static float s_peak_l = -999.0f;
static float s_peak_r = -999.0f;
static bool  s_first_frame = true;
static int   s_prev_sel = -1;
static int   s_prev_dsp_state = -1;
static bool  s_prev_wifi_ok = false;
static float s_prev_db[4] = {-999.0f, -999.0f, -999.0f, -999.0f};

/* 4x4 Bayer 有序抖动阈值表（行相位 = y&3，列相位 = x&3） */
static const float s_bayer[4][4] = {
    {  0.0f / 16.0f,  8.0f / 16.0f,  2.0f / 16.0f, 10.0f / 16.0f },
    { 12.0f / 16.0f,  4.0f / 16.0f, 14.0f / 16.0f,  6.0f / 16.0f },
    {  3.0f / 16.0f, 11.0f / 16.0f,  1.0f / 16.0f,  9.0f / 16.0f },
    { 15.0f / 16.0f,  7.0f / 16.0f, 13.0f / 16.0f,  5.0f / 16.0f },
};

/* 预计算全宽渐变调色板（已做字节序交换），索引 = col*4 + (y&3)。
 * 每列只算一次基色，按 4 个行相位抖动量化为 4 种 RGB565。 */
static uint16_t s_grad_pal[ST7789_WIDTH * 4];
static bool s_palette_built = false;

/* 能量条某一帧的图形边界：渐变前端列 fw、峰值线中心列 px（用于局部刷新） */
typedef struct {
    int fw;
    int px;
} bar_extent_t;

/* 上帧 L/R 能量条的边界（用于只刷新变化列区间） */
static bar_extent_t s_ext_l = {0, 0};
static bar_extent_t s_ext_r = {0, 0};
static bar_extent_t s_prev_ext_l = {0, 0};
static bar_extent_t s_prev_ext_r = {0, 0};

/* ---------- 桥接调试页状态 ----------
 * 桥接期间本地电平表必须让位（SigmaStudio 重编过程序后本地地址就失配了），
 * 所以整屏切换成"调试页"：上面是转发状态，下面是网络信息。
 *
 * ⚠️⚠️ 屏幕字库 `font5x7.h` 是**手挑子集**，只有：
 *
 *     space  .  +  -  :  >  %  0-9  A-Z  d
 *
 * 也就是说 **小写字母只有 `d`**（因为 "dB" 要用），别的全是空白！
 * 所以这一整页的文案**必须全大写**，且不能用 `( ) , / < = ? _` 这些符号。
 * （踩过：底部提示全小写，结果 41 个字符只画出 45 个像素 ——
 *   整行只剩 "yiel*d*e*d*, ... *d*isconnect" 里的三个 `d`，
 *   看起来就像屏幕坏了；`0x0000` 的 `x` 也是小写，会显示成 "0 0000"。）
 */
#define BR_ROWS       7
#define BR_MAX_SEGS   4
#define BR_SEG_LEN    40

/* 一行 = 若干"文字 + 颜色"小段，这样同一行里标签可以灰、值可以绿或红 */
typedef struct {
    char     text[BR_MAX_SEGS][BR_SEG_LEN];
    uint16_t color[BR_MAX_SEGS];
    uint8_t  n;
} br_row_t;

static bool s_bridge_page = false;                    /* 上帧是否在桥接页 */
static bool s_need_clear_top = false;                 /* 上半屏布局变了，需整块清黑 */
static int  s_top_layout = -1;                        /* 当前上半屏布局类型（见 ui_render） */
static br_row_t s_br_rows[BR_ROWS];                   /* 上帧各行分段（变化检测） */
static bool s_br_rows_valid = false;
static int  s_br_hold_frames = 0;                     /* 刷新节流计数 */
static int  s_logged_state = -1;                      /* 已记录日志的 DSP 状态 */

/* DSP 状态名称（日志用） */
static const char *dsp_state_name(int st)
{
    switch (st) {
    case UI_DSP_ONLINE:    return "ONLINE";
    case UI_DSP_BRIDGE:    return "BRIDGE(调试页)";
    case UI_DSP_LOCAL_OFF: return "LOCAL_OFF(本地控制关)";
    default:               return "OFFLINE";
    }
}

void ui_init(void)
{
    s_disp_l = -999.0f;
    s_disp_r = -999.0f;
    s_peak_l = -999.0f;
    s_peak_r = -999.0f;
    s_first_frame = true;
    s_prev_sel = -1;
    s_prev_dsp_state = -1;
    s_prev_wifi_ok = false;
    for (int i = 0; i < 4; i++) {
        s_prev_db[i] = -999.0f;
    }
    s_bridge_page = false;
    s_need_clear_top = true;
    s_top_layout = -1;
    s_br_rows_valid = false;
    s_br_hold_frames = 0;
    s_logged_state = -1;
}

/* 格式化 dB 值，如 "-6.0" 或 "+2.0" */
static void format_db(char *buf, size_t n, float db, bool plus)
{
    if (fabsf(db) < 0.05f) {
        db = 0.0f;
    }
    if (plus) {
        snprintf(buf, n, "%+.1f", db);
    } else {
        snprintf(buf, n, "%.1f", db);
    }
}

/* 感知响度映射：把 dB 转为百分比显示（仅主音量用）。
 * 锚点：-96 dB -> 0%，0 dB -> 100%，+10 dB -> 120%。
 * 0 dB 以下按响度感知（声强 I^0.3，即每 -10 dB 响度约减半）映射，
 * 使 -50 dB 约落在 2%，贴合"几乎听不到"的听感；0 dB 以上线性外推到 120%。 */
float ui_db_to_percent_float(float db)
{
    if (db <= -96.0f) {
        return 0.0f;
    }
    if (db <= 0.0f) {
        return 100.0f * powf(10.0f, db / 30.0f);
    }
    float p = 100.0f + (db / 10.0f) * 20.0f;
    if (p > 120.0f) {
        p = 120.0f;
    }
    return p;
}

static int db_to_percent(float db)
{
    return (int)lroundf(ui_db_to_percent_float(db));
}

/* 反向映射：百分比 -> dB（ui_db_to_percent_float 的逆运算）。
 * 0% -> -96 dB，100% -> 0 dB，120% -> +10 dB。 */
float ui_percent_to_db(float percent)
{
    if (percent <= 0.0f) {
        return -96.0f;
    }
    if (percent <= 100.0f) {
        return 30.0f * log10f(percent / 100.0f);
    }
    float db = (percent - 100.0f) * 0.5f;
    if (db > 10.0f) {
        db = 10.0f;
    }
    return db;
}

/* 能量条色相分段：按条内位置 t(0~1) 返回色相（度）。
 * 绿(120°) -> 黄(60°) -> 橙(30°) -> 红(0°)，整条色相连续渐变，
 * 避免 RGB 三通道独立插值在 黄->橙->红 段产生的色块/偏色。 */
static float bar_hue(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.40f) return 120.0f - 150.0f * t;          /* 120 -> 60 */
    if (t < 0.70f) return 60.0f - 100.0f * (t - 0.40f); /* 60 -> 30 */
    return 30.0f - 100.0f * (t - 0.70f);                /* 30 -> 0 */
}

/* 有序抖动量化：把 [0, max] 的连续值量化到整数，用 Bayer 阈值分散量化误差，
 * 消除 RGB565 低位量化带来的色带（banding）。 */
static int dither_quant(float v, int max, float th)
{
    float scaled = v * (float)max;
    int base = (int)scaled;
    float frac = scaled - (float)base;
    int out = (frac > th) ? base + 1 : base;
    if (out > max) out = max;
    if (out < 0) out = 0;
    return out;
}

/* 能量条渐变颜色：色相渐变 + 4x4 Bayer 有序抖动（按像素坐标），
 * 使 黄->橙->红 段平滑无可见色块。通用回退路径用。 */
static uint16_t bar_gradient_dithered(float t, int x, int y)
{
    float th = s_bayer[y & 3][x & 3];

    float hue = bar_hue(t);
    float c = 1.0f;                       /* 饱和度 S=1，明度 V=1 */
    float hp = hue / 60.0f;
    float xc = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r, g, b;
    if (hp < 1.0f)      { r = c;    g = xc; b = 0.0f; }
    else if (hp < 2.0f) { r = xc;   g = c;  b = 0.0f; }
    else if (hp < 3.0f) { r = 0.0f; g = c;  b = xc; }
    else if (hp < 4.0f) { r = 0.0f; g = xc; b = c; }
    else if (hp < 5.0f) { r = xc;   g = 0.0f; b = c; }
    else                { r = c;    g = 0.0f; b = xc; }

    int ri = dither_quant(r, 31, th);
    int gi = dither_quant(g, 63, th);
    int bi = dither_quant(b, 31, th);
    return (uint16_t)((ri << 11) | (gi << 5) | bi);
}

/* 预计算全宽渐变调色板：每列一次 HSV->RGB，按 4 行相位抖动量化。
 * 只执行一次，此后 draw_bar 直接查表写入帧缓冲。 */
static void build_gradient_palette(void)
{
    for (int x = 0; x < ST7789_WIDTH; x++) {
        float t = (ST7789_WIDTH > 1) ? (float)x / (float)(ST7789_WIDTH - 1) : 0.0f;
        float hue = bar_hue(t);
        float hp = hue / 60.0f;
        float xc = 1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f);
        float r, g, b;
        if (hp < 1.0f)      { r = 1.0f; g = xc;   b = 0.0f; }
        else if (hp < 2.0f) { r = xc;   g = 1.0f; b = 0.0f; }
        else if (hp < 3.0f) { r = 0.0f; g = 1.0f; b = xc; }
        else if (hp < 4.0f) { r = 0.0f; g = xc;   b = 1.0f; }
        else if (hp < 5.0f) { r = xc;   g = 0.0f; b = 1.0f; }
        else                { r = 1.0f; g = 0.0f; b = xc; }

        for (int phase = 0; phase < 4; phase++) {
            float th = s_bayer[phase][x & 3];
            int ri = dither_quant(r, 31, th);
            int gi = dither_quant(g, 63, th);
            int bi = dither_quant(b, 31, th);
            s_grad_pal[x * 4 + phase] =
                st7789_color_swap((uint16_t)((ri << 11) | (gi << 5) | bi));
        }
    }
    s_palette_built = true;
}

/* 画一个实心圆点（用于 DSP 状态指示） */
static void draw_dot(int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                st7789_draw_pixel(cx + dx, cy + dy, color);
            }
        }
    }
}

/* 画 WiFi 图标（三条弧 + 底部圆点），cx/cy 为底部中心坐标。 */
static void draw_wifi_icon(int cx, int cy, uint16_t color)
{
    draw_dot(cx, cy, 1, color);
    const int radii[3] = {3, 5, 7};
    for (int k = 0; k < 3; k++) {
        int r = radii[k];
        for (int a = 45; a <= 135; a += 2) {
            float rad = a * 3.14159265f / 180.0f;
            int x = cx + (int)lroundf(r * cosf(rad));
            int y = cy - (int)lroundf(r * sinf(rad));
            st7789_draw_pixel(x, y, color);
        }
    }
}

/* VU 表弹道模拟：一阶低通包络检测（attack/release）。 */
static float smooth_level(float *disp, float target)
{
    /* 帧间隔约 15ms，对应时间常数 tau ≈ 120ms / 250ms */
    const float ALPHA_ATTACK  = 0.12f;
    const float ALPHA_RELEASE = 0.06f;

    if (target < -200.0f) {
        target = -96.0f;
    }
    if (*disp < -200.0f) {
        *disp = target;
        return *disp;
    }

    float alpha = (target > *disp) ? ALPHA_ATTACK : ALPHA_RELEASE;
    *disp += alpha * (target - *disp);
    return *disp;
}

/* 峰值保持标记：快速跟随上升，极缓慢滑落。 */
static float peak_hold(float *peak, float level)
{
    const float PEAK_RELEASE_DB_PER_FRAME = 0.06f;

    if (*peak < -200.0f) {
        *peak = level;
    } else if (level > *peak) {
        *peak = level;
    } else {
        *peak -= PEAK_RELEASE_DB_PER_FRAME;
        if (*peak < level) {
            *peak = level;
        }
    }
    if (*peak < -96.0f) {
        *peak = -96.0f;
    }
    return *peak;
}

static void draw_bar(int x, int y, int w, int h, float db, float peak_db,
                     bar_extent_t *ext)
{
    st7789_fill_rect(x, y, w, h, COLOR_DKGRAY);

    /* 非线性（感知响度）映射：frac = 10^(db/60) */
    float frac;
    if (db <= -60.0f) {
        frac = 0.0f;
    } else {
        frac = powf(10.0f, db / 60.0f);
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }

    /* 渐变填充：全宽时走调色板查表直写帧缓冲（无逐像素浮点运算/函数调用） */
    int fw = (int)(w * frac + 0.5f);
    if (fw > 0) {
        uint16_t *fb = st7789_framebuffer();
        if (fb != NULL) {
            if (!s_palette_built) {
                build_gradient_palette();
            }
            if (x == 0 && w == ST7789_WIDTH) {
                /* 快路径：全宽能量条，逐列查表写帧缓冲 */
                for (int i = 0; i < fw; i++) {
                    uint16_t *dst = &fb[y * ST7789_WIDTH + i];
                    uint16_t *pal = &s_grad_pal[i * 4];
                    for (int j = 0; j < h; j++) {
                        dst[j * ST7789_WIDTH] = pal[(y + j) & 3];
                    }
                }
            } else {
                /* 通用回退：非全宽时逐像素计算（当前布局不会走到） */
                for (int i = 0; i < fw; i++) {
                    float t = (w > 1) ? (float)i / (float)(w - 1) : 0.0f;
                    for (int j = 0; j < h; j++) {
                        st7789_draw_pixel(x + i, y + j,
                                          bar_gradient_dithered(t, x + i, y + j));
                    }
                }
            }
        }
    }

    /* 峰值保持标记线 */
    float pfrac;
    if (peak_db <= -60.0f) {
        pfrac = 0.0f;
    } else {
        pfrac = powf(10.0f, peak_db / 60.0f);
    }
    if (pfrac > 1.0f) {
        pfrac = 1.0f;
    }
    int px = x + (int)(w * pfrac + 0.5f);
    if (px > x) {
        st7789_fill_rect(px - 1, y, 2, h, COLOR_WHITE);
    }

    ext->fw = fw;
    ext->px = px;
}

/* 画一个电平显示：上方小标签（L / R）+ 铺满全宽的渐变能量条（含峰值线、刻度） */
static void draw_level(int y, const char *label, float db, float peak_db,
                       bar_extent_t *ext)
{
    st7789_draw_text(2, y, label, COLOR_WHITE, COLOR_BLACK, 2);
    draw_bar(0, y + 16, ST7789_WIDTH, BAR_HEIGHT, db, peak_db, ext);
}

/* 上半屏：状态栏 + 左右声道电平（动态，每帧重绘） */
static void draw_top(int dsp_state, bool wifi_ok, float lv_l, float lv_r,
                     float pk_l, float pk_r)
{
    /* 状态栏：状态点 + DSP 状态（小字） + 最右端 WiFi 图标 */
    const char *txt;
    uint16_t dot;
    if (dsp_state == UI_DSP_BRIDGE) {
        txt = "SIGMASTUDIO";
        dot = COLOR_CYAN;
    } else if (dsp_state == UI_DSP_LOCAL_OFF) {
        /* DSP 本身在线，只是本地控制被关掉了 —— 用橙色区分，不要误报 OFFLINE */
        txt = "LOCAL CTL OFF";
        dot = COLOR_ORANGE;
    } else if (dsp_state == UI_DSP_ONLINE) {
        txt = "1452 ONLINE";
        dot = COLOR_GREEN;
    } else {
        txt = "1452 OFFLINE";
        dot = COLOR_RED;
    }

    st7789_fill_rect(0, 0, ST7789_WIDTH, STATUS_H, COLOR_BLACK);
    draw_dot(6, 6, 3, dot);
    st7789_draw_text(14, 2, txt, COLOR_WHITE, COLOR_BLACK, 1);
    draw_wifi_icon(ST7789_WIDTH - 11, 8, wifi_ok ? COLOR_GREEN : COLOR_GRAY);

    if (dsp_state == UI_DSP_LOCAL_OFF) {
        /*
         * 本地控制关闭时电平表**没有数据源**。
         * 与其画两条永远空着的能量条（容易被当成坏了），不如直接说明原因与恢复方法。
         */
        st7789_fill_rect(0, STATUS_H + 1, ST7789_WIDTH,
                         SPLIT_Y - STATUS_H - 1, COLOR_BLACK);

        static const char *const msg[3] = {
            "LEVEL METER NEEDS LOCAL CTRL",
            "TURN IT ON IN THE WEB PAGE",
            "TOOLS - SIGMASTUDIO BRIDGE",
        };
        for (int i = 0; i < 3; i++) {
            int tw = st7789_text_width(msg[i], 1);
            st7789_draw_text((ST7789_WIDTH - tw) / 2, 40 + i * 20, msg[i],
                             (i == 0) ? COLOR_ORANGE : COLOR_GRAY, COLOR_BLACK, 1);
        }

        /* 让能量条边界归零，回到电平页时不会刷出一块旧内容 */
        s_ext_l.fw = s_ext_l.px = 0;
        s_ext_r.fw = s_ext_r.px = 0;
        return;
    }

    /* L / R 电平表（铺满上半屏，无左右留白） */
    draw_level(LEVEL_L_Y, "L", lv_l, pk_l, &s_ext_l);
    draw_level(LEVEL_R_Y, "R", lv_r, pk_r, &s_ext_r);
}

/* 只刷新能量条变化的列区间：前端渐变 fw 与峰值线 px 扫过的范围。
 * 相比整条 flush，大幅缩小 SPI 传输，避免前端在大变化时撕裂。 */
static void flush_bar_changed(int bar_y, const bar_extent_t *prev,
                              const bar_extent_t *cur)
{
    int lo = prev->fw;
    int hi = prev->fw;
    if (cur->fw < lo) lo = cur->fw;
    if (cur->fw > hi) hi = cur->fw;

    /* 峰值线占 [px-1, px+1) */
    int pl = prev->px - 1, pr = prev->px + 1;
    int cl = cur->px - 1, cr = cur->px + 1;
    if (pl < lo) lo = pl;
    if (cl < lo) lo = cl;
    if (pr > hi) hi = pr;
    if (cr > hi) hi = cr;

    if (lo < 0) lo = 0;
    if (hi > ST7789_WIDTH) hi = ST7789_WIDTH;
    if (hi > lo) {
        st7789_flush_rect(lo, bar_y, hi - lo, BAR_HEIGHT);
    }
}

/* 下半屏：MASTER（值跟在名字后）+ 三个音调并排（值跟在名字后） */
static void draw_bottom(const volume_channel_t *ch, int sel)
{
    st7789_fill_rect(0, SPLIT_Y, ST7789_WIDTH, ST7789_HEIGHT - SPLIT_Y, COLOR_BLACK);

    /* 分隔线 */
    st7789_fill_rect(0, SPLIT_Y, ST7789_WIDTH, 2, COLOR_DKGRAY);

    /* MASTER：居中显示，值（百分比）跟在名字后面 */
    char master[32];
    snprintf(master, sizeof(master), "%s MASTER %d%%",
             (sel == 0) ? ">" : " ", db_to_percent(ch[0].db));
    int tw = st7789_text_width(master, 2);
    st7789_draw_text((ST7789_WIDTH - tw) / 2, 150, master,
                     COLOR_WHITE, COLOR_BLACK, 2);

    /* 三个音调并排：值（dB）跟在名字后面 */
    static const int order[3] = {1, 2, 3}; /* LOW, HIGH, MID */
    const int col_x[3] = {8, 120, 232};
    for (int c = 0; c < 3; c++) {
        int idx = order[c];
        char val[16];
        format_db(val, sizeof(val), ch[idx].db, true);
        size_t l = strlen(val);
        snprintf(val + l, sizeof(val) - l, "dB");

        char row[32];
        snprintf(row, sizeof(row), "%s%s %s",
                 (sel == idx) ? ">" : " ", ch[idx].name, val);
        st7789_draw_text(col_x[c], 205, row, COLOR_WHITE, COLOR_BLACK, 1);
    }
}

/* ---------- 桥接调试页（整屏） ----------
 * 版式（320x240）：
 *
 *   y   0..11   状态栏（青点 + SIGMASTUDIO + WiFi 图标）
 *   y    14     标题 "ONLINE DEBUG"（scale 2）
 *   y    33     副标题（等待连接 / 已连接）
 *   y    50     7 行信息，步进 19（标签灰 + 值按语义着色）
 *   y   177     分隔线
 *   y   182     底部提示第一行（scale 2，橙色）
 *   y   205     底部提示第二行（黄色）
 *
 * 原本底部只有一行 COLOR_DKGRAY 的说明，在暗屏上几乎看不见，等于白占地方；
 * 现在改成两行带颜色的说明，把整屏用满。
 */
#define BR_LABEL_X     4
#define BR_VALUE_X     76
#define BR_TITLE_Y     14
#define BR_SUB_Y       33
#define BR_FIRST_Y     50
#define BR_ROW_STEP    19
#define BR_SEP_Y       177
#define BR_NOTE1_Y     182
#define BR_NOTE2_Y     205

/* 每行值的最长可用宽度：(320 - 76) / 6px ≈ 40 字符 */
#define BR_VALUE_CHARS 40

/* 把微秒格式化成 "812US" 或 "1.87MS"（必须大写：字库无小写 s/m） */
static void format_us(char *buf, size_t n, uint32_t us)
{
    if (us < 1000) {
        snprintf(buf, n, "%luUS", (unsigned long)us);
    } else {
        snprintf(buf, n, "%.2fMS", (double)us / 1000.0);
    }
}

/* 读响应耗时配色：正常几百微秒 → 绿；被调度拖慢 → 黄；很慢 → 橙 */
static uint16_t lat_color(uint32_t us)
{
    if (us == 0) {
        return COLOR_GRAY;
    }
    if (us < 1500) {
        return COLOR_GREEN;
    }
    return (us < 5000) ? COLOR_YELLOW : COLOR_ORANGE;
}

/* WiFi 信号配色（与网页顶栏胶囊同一套阈值） */
static uint16_t rssi_color(int rssi)
{
    if (rssi > -60) {
        return COLOR_GREEN;
    }
    return (rssi >= -72) ? COLOR_YELLOW : COLOR_RED;
}

/* 给一行追加一段"文字 + 颜色"，超出段数上限直接忽略 */
static void seg_addf(br_row_t *r, uint16_t color, const char *fmt, ...)
{
    if (r->n >= BR_MAX_SEGS) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->text[r->n], BR_SEG_LEN, fmt, ap);
    va_end(ap);
    r->color[r->n] = color;
    r->n++;
}

/*
 * 组装调试页的 7 行内容（含配色）。
 *
 * 关键：**没有流量时不要显示 0 值**。`last_addr`/`lat_avg` 这些字段在
 * 从未连接时都是 0，直接画出来就是 "0 0000  0 B"、"AVG 0US  ERR 0"
 * ——用户看到的就是一堆莫名其妙的数字和字母。改为明确的 "NO TRAFFIC YET"。
 */
static void build_bridge_rows(const ui_bridge_info_t *br, br_row_t *rows)
{
    /* 先整块清零：结构体有填充字节，不清 memcmp 会误判成"内容变了" */
    memset(rows, 0, sizeof(br_row_t) * BR_ROWS);

    const bool has_traffic = (br->pkt_write + br->pkt_read) > 0;

    /* PC：客户端地址与端口（没连时给出明确提示，而不是空白） */
    if (br->client_ip[0] != '\0') {
        seg_addf(&rows[0], COLOR_GREEN, "%s", br->client_ip);
        seg_addf(&rows[0], COLOR_GRAY, ":%u", (unsigned)br->port);
    } else {
        seg_addf(&rows[0], COLOR_ORANGE, "WAITING");
        seg_addf(&rows[0], COLOR_GRAY, " :%u", (unsigned)br->port);
    }

    /* LINK：活跃连接数（SigmaStudio 可能并发开多条） */
    seg_addf(&rows[1], br->clients ? COLOR_GREEN : COLOR_GRAY, "%u", (unsigned)br->clients);
    seg_addf(&rows[1], COLOR_GRAY, " OF 4 MAX");

    /* COUNT：累计转发的写/读报文数 */
    seg_addf(&rows[2], COLOR_GRAY, "W ");
    seg_addf(&rows[2], has_traffic ? COLOR_CYAN : COLOR_GRAY, "%lu",
             (unsigned long)br->pkt_write);
    seg_addf(&rows[2], COLOR_GRAY, "  R ");
    seg_addf(&rows[2], has_traffic ? COLOR_GREEN : COLOR_GRAY, "%lu",
             (unsigned long)br->pkt_read);

    /* LAST：最近一次访问。从未通信时显示 NO TRAFFIC YET 而不是假的 0x0000 */
    if (!has_traffic) {
        seg_addf(&rows[3], COLOR_GRAY, "NO TRAFFIC YET");
    } else {
        seg_addf(&rows[3], br->last_is_read ? COLOR_GREEN : COLOR_ORANGE, "%s",
                 br->last_is_read ? "READ " : "WRITE");
        /* ⚠️ 必须大写 "0X"：字库没有小写 x，会渲染成 "0 0034" */
        seg_addf(&rows[3], COLOR_WHITE, "0X%04X", (unsigned)br->last_addr);
        seg_addf(&rows[3], COLOR_GRAY, "  %lu B", (unsigned long)br->last_len);
    }

    /* RESP：读响应平均耗时 + 出错次数 */
    if (br->lat_avg_us == 0) {
        seg_addf(&rows[4], COLOR_GRAY, "NO DATA YET");
    } else {
        char lat[16];
        format_us(lat, sizeof(lat), br->lat_avg_us);
        seg_addf(&rows[4], COLOR_GRAY, "AVG ");
        seg_addf(&rows[4], lat_color(br->lat_avg_us), "%s", lat);
        seg_addf(&rows[4], COLOR_GRAY, "  ERR ");
        seg_addf(&rows[4], br->errors ? COLOR_RED : COLOR_GREEN, "%lu",
                 (unsigned long)br->errors);
    }

    /* LOCAL：本机 STA 地址 */
    seg_addf(&rows[5], br->local_ip[0] ? COLOR_WHITE : COLOR_ORANGE, "%s",
             br->local_ip[0] ? br->local_ip : "NO WIFI");

    /* WIFI：信号强度与信道（"DBM" 大写，字库无小写 m） */
    if (br->channel != 0) {
        seg_addf(&rows[6], rssi_color((int)br->rssi), "%d DBM", (int)br->rssi);
        seg_addf(&rows[6], COLOR_GRAY, "  CH%u", (unsigned)br->channel);
    } else {
        seg_addf(&rows[6], COLOR_ORANGE, "NOT CONNECTED");
    }
}

static void draw_bridge_page(const br_row_t *rows, bool wifi_ok, bool waiting)
{
    static const char *const labels[BR_ROWS] = {
        "PC", "LINK", "COUNT", "LAST", "RESP", "LOCAL", "WIFI",
    };

    st7789_fill(COLOR_BLACK);

    /* 状态栏：青点 + SIGMASTUDIO + WiFi 图标 */
    draw_dot(6, 6, 3, COLOR_CYAN);
    st7789_draw_text(14, 2, "SIGMASTUDIO", COLOR_CYAN, COLOR_BLACK, 1);
    draw_wifi_icon(ST7789_WIDTH - 11, 8, wifi_ok ? COLOR_GREEN : COLOR_GRAY);
    st7789_fill_rect(0, STATUS_H, ST7789_WIDTH, 1, COLOR_DKGRAY);

    /* 标题 */
    const char *title = "ONLINE DEBUG";
    st7789_draw_text((ST7789_WIDTH - st7789_text_width(title, 2)) / 2, BR_TITLE_Y,
                     title, COLOR_WHITE, COLOR_BLACK, 2);

    /* 副标题：区分"已有连接"与"还在等连接"，免得用户看到 0 不知道怎么下手 */
    const char *sub = waiting ? "WAITING FOR SIGMASTUDIO" : "PC - ESP32 - ADAU1452";
    st7789_draw_text((ST7789_WIDTH - st7789_text_width(sub, 1)) / 2, BR_SUB_Y,
                     sub, waiting ? COLOR_ORANGE : COLOR_GREEN, COLOR_BLACK, 1);

    /* 信息行：标签灰色，值按语义分段着色（读取绿 / 写入橙 / 出错红 …） */
    for (int i = 0; i < BR_ROWS; i++) {
        const int y = BR_FIRST_Y + i * BR_ROW_STEP;
        st7789_draw_text(BR_LABEL_X, y, labels[i], COLOR_GRAY, COLOR_BLACK, 1);

        int x = BR_VALUE_X;
        for (int s = 0; s < rows[i].n; s++) {
            st7789_draw_text(x, y, rows[i].text[s], rows[i].color[s],
                             COLOR_BLACK, 1);
            x += st7789_text_width(rows[i].text[s], 1);
        }
    }

    /* 底部提示：两行、带颜色、第一行放大。
     * （原来是一行 COLOR_DKGRAY 的小字，在暗环境下几乎不可见，等于白占地方。） */
    st7789_fill_rect(0, BR_SEP_Y, ST7789_WIDTH, 1, COLOR_DKGRAY);

    const char *note1 = "LOCAL CONTROL YIELDED";
    st7789_draw_text((ST7789_WIDTH - st7789_text_width(note1, 2)) / 2, BR_NOTE1_Y,
                     note1, COLOR_ORANGE, COLOR_BLACK, 2);

    const char *note2 = "BACK WHEN BRIDGE OFF";
    st7789_draw_text((ST7789_WIDTH - st7789_text_width(note2, 1)) / 2, BR_NOTE2_Y,
                     note2, COLOR_YELLOW, COLOR_BLACK, 1);

    st7789_flush();
}

/*
 * 桥接调试页入口。返回 true 表示本帧已完整接管屏幕（调用方不要再画电平表）。
 * 只在内容变化时重绘，并用帧计数节流（主循环桥接时约 80ms/帧），
 * 避免转发计数每帧变化都把 150KB 的整屏推一遍 SPI。
 */
static bool render_bridge_page(const ui_bridge_info_t *br, bool wifi_ok)
{
    ui_bridge_info_t dummy;
    if (br == NULL) {
        /* 没传信息也要有东西显示，给个空壳 */
        memset(&dummy, 0, sizeof(dummy));
        br = &dummy;
    }

    br_row_t rows[BR_ROWS];
    build_bridge_rows(br, rows);

    /* 逐段 memcmp 判断是否要重绘（build_bridge_rows 已整块清零，填充字节稳定） */
    bool changed = !s_br_rows_valid;
    for (int i = 0; i < BR_ROWS && !changed; i++) {
        if (memcmp(&rows[i], &s_br_rows[i], sizeof(br_row_t)) != 0) {
            changed = true;
        }
    }

    /* 首次进入该页时写一条日志：屏幕看不到时可从网页日志确认切页生效 */
    if (!s_br_rows_valid) {
        ESP_LOGI(TAG, "屏幕切到调试页：client=%s:%u conn=%u W=%lu R=%lu",
                 br->client_ip[0] ? br->client_ip : "(none)", (unsigned)br->port,
                 (unsigned)br->clients, (unsigned long)br->pkt_write,
                 (unsigned long)br->pkt_read);
    }

    if (changed) {
        if (s_br_hold_frames > 0) {
            s_br_hold_frames--;
            return true;                 /* 仍由本页占屏，只是本帧不重绘 */
        }
        draw_bridge_page(rows, wifi_ok, br->clients == 0);

        memcpy(s_br_rows, rows, sizeof(s_br_rows));
        s_br_rows_valid = true;
        s_br_hold_frames = 3;
    }
    return true;
}

void ui_render(const volume_channel_t *channels, int sel, ui_dsp_state_t dsp_state,
               bool wifi_ok, float lv_l, float lv_r,
               const ui_bridge_info_t *br)
{
    /*
     * 桥接页与电平页**整屏互斥**。
     * 切换时强制走"首帧整屏刷新"，否则两张页面的残留会互相叠加。
     */
    const bool want_bridge = (dsp_state == UI_DSP_BRIDGE);

    /* 状态变化打一条日志：屏幕看不到时也能从网页日志确认当前显示状态 */
    if ((int)dsp_state != s_logged_state) {
        ESP_LOGI(TAG, "DSP 状态：%s -> %s",
                 (s_logged_state < 0) ? "(init)" : dsp_state_name(s_logged_state),
                 dsp_state_name((int)dsp_state));
        s_logged_state = (int)dsp_state;
    }

    if (want_bridge != s_bridge_page) {
        ESP_LOGI(TAG, "屏幕切页：%s（dsp_state=%d，bridge_on=%d）",
                 want_bridge ? "电平表 -> 在线调试" : "在线调试 -> 电平表",
                 (int)dsp_state, (int)want_bridge);
        s_bridge_page = want_bridge;
        s_first_frame = true;
        s_br_rows_valid = false;
        s_br_hold_frames = 0;
    }

    /*
     * 「上半屏布局可能变了」—— 包括两类互斥布局：
     *   2 = 桥接调试页；1 = 本地控制关闭提示页；0 = 电平页。
     *
     * 布局变化时必须**整块清黑上半屏，并且把清完的整块推屏**。
     * 因为电平页平时只刷状态栏 + 两条能量条的窄列带，落在这些矩形之外的
     * 文字（调试页 y=54~80 / 104~120 的信息行、LOCAL_OFF 在 y=60 的那行
     * 提示）永远不会被覆盖 —— 只清帧缓冲不推屏，屏上依旧是残影。
     */
    const int top_layout = (dsp_state == UI_DSP_BRIDGE)     ? 2
                         : (dsp_state == UI_DSP_LOCAL_OFF)  ? 1 : 0;
    if (top_layout != s_top_layout) {
        s_top_layout = top_layout;
        s_need_clear_top = true;
    }

    if (want_bridge) {
        render_bridge_page(br, wifi_ok);
        return;
    }

    const bool clear_top = s_need_clear_top;
    if (clear_top) {
        st7789_fill_rect(0, 0, ST7789_WIDTH, SPLIT_Y, COLOR_BLACK);
        s_need_clear_top = false;
    }

    /* 电平平滑 + 峰值保持（内部状态） */
    s_disp_l = smooth_level(&s_disp_l, lv_l);
    s_disp_r = smooth_level(&s_disp_r, lv_r);
    s_peak_l = peak_hold(&s_peak_l, s_disp_l);
    s_peak_r = peak_hold(&s_peak_r, s_disp_r);

    /* 判断下半屏是否发生变化（布局变化时一并重画，保证整屏状态自洽） */
    bool bottom_changed = s_first_frame || clear_top || (sel != s_prev_sel);
    for (int i = 0; i < 4; i++) {
        if (fabsf(channels[i].db - s_prev_db[i]) > 0.01f) {
            bottom_changed = true;
        }
    }

    /* 绘制上半屏（状态栏 + 电平表） */
    draw_top((int)dsp_state, wifi_ok, s_disp_l, s_disp_r, s_peak_l, s_peak_r);

    if (bottom_changed) {
        draw_bottom(channels, sel);
        s_prev_sel = sel;
        for (int i = 0; i < 4; i++) {
            s_prev_db[i] = channels[i].db;
        }
    }

    /* 局部刷新：首帧整屏；之后只刷两条能量条“实际变化的列区间” + 变化的状态栏/下半屏。
     * 变化大时也只刷前端与峰值线扫过的窄列带，SPI 传输最小化，消除前端撕裂。 */
    if (s_first_frame) {
        st7789_flush();
        s_first_frame = false;
        s_prev_dsp_state = (int)dsp_state;
        s_prev_wifi_ok = wifi_ok;
        s_prev_ext_l = s_ext_l;
        s_prev_ext_r = s_ext_r;
    } else {
        /* 上半屏刚整块清过：必须整块推屏，否则清掉的文字仍留在屏上 */
        if (clear_top) {
            st7789_flush_rect(0, 0, ST7789_WIDTH, SPLIT_Y);
        }

        if ((int)dsp_state != s_prev_dsp_state || wifi_ok != s_prev_wifi_ok) {
            st7789_flush_rect(0, 0, ST7789_WIDTH, STATUS_H);
            s_prev_dsp_state = (int)dsp_state;
            s_prev_wifi_ok = wifi_ok;
        }

        /* 计算并刷新 L / R 能量条的变化列区间（前端渐变 + 峰值线扫过的范围） */
        flush_bar_changed(LEVEL_L_Y + 16, &s_prev_ext_l, &s_ext_l);
        flush_bar_changed(LEVEL_R_Y + 16, &s_prev_ext_r, &s_ext_r);
        s_prev_ext_l = s_ext_l;
        s_prev_ext_r = s_ext_r;

        if (bottom_changed) {
            st7789_flush_rect(0, SPLIT_Y, ST7789_WIDTH, ST7789_HEIGHT - SPLIT_Y);
        }
    }
}
