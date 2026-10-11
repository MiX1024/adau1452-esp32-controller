#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EQ 系数持久化
 * ==============
 *
 * 网页上调过的每一段 EQ（biquad 的 5 个系数）在这里落盘，并在
 *   ① 开机 DSP 就绪后   ② 每次 DSP 程序映像被重新推送后
 * 自动写回 1452 —— 否则 1452 断电重启、或固件重推时，EQ 会被冲回
 * 「录制映像那一刻的值」，用户会发现"调好的 EQ 没了"。
 *
 * 存的每一项同时带一份 **key = "cell|parameter"**（与首页控件绑定同一套约定）：
 * 地址会随着 SigmaStudio 工程重编而整体平移，而名字不会。
 * 网页在重新导入工程文件后按 key 把地址重新解析一遍并写回设备，
 * 所以换 DSP 程序**不需要改固件**。
 *
 * ⚠️ 设备端只用 addr 做写入；key 只是留给网页做重解析的线索。
 * 落盘位置：NVS 命名空间 `eq`、键 `bands`，整个表一个 blob 存取。
 */

#define EQ_STORE_MAX      24   /* 最多记住多少段（24 段 × 80B = 1920B，NVS blob 放得下） */
#define EQ_STORE_WORDS     5   /* 一段 biquad = b0,b1,b2,-a2,-a1 */
#define EQ_STORE_KEY_LEN  56   /* "cell|parameter"，如 "Mid Filter1_3|Targ_B2_1" */

typedef struct {
    uint16_t addr;                     /* 当前地址（换工程后由网页刷新） */
    uint16_t words;                    /* 实际字数，目前恒为 EQ_STORE_WORDS */
    uint32_t w[EQ_STORE_WORDS];        /* 系数，顺序 b0,b1,b2,-a2,-a1 */
    char     key[EQ_STORE_KEY_LEN];    /* "cell|parameter"，可为空（只按地址对） */
} eq_band_t;

/* 读 NVS 恢复保存过的 EQ（在任何 DSP 访问之前调用，建议紧跟 bind_load()） */
void eq_store_init(void);

/* 当前保存了多少段 */
int eq_store_count(void);

/* 取出全部保存项，返回实际条数 */
int eq_store_get(eq_band_t *out, int max);

/*
 * 新增 / 更新一段：写进表 → 立刻写 DSP → 立刻存 NVS。
 * 返回 DSP 是否写成功（写失败也照样保存，下次上电还会再推一次）。
 */
bool eq_store_put(const eq_band_t *b);

/* 删除一段（按 key，key 为空时按 addr）。只从表里删，不动 DSP 当前值。 */
bool eq_store_del(const char *key, uint16_t addr);

/* 清空全部（不动 DSP） */
void eq_store_clear(void);

/* 把保存的全部写回 DSP。DSP 未就绪时静默失败，等下次调用再试。 */
void eq_store_apply(void);

/* 上一次 eq_store_apply() 成功写回了几段（供状态显示） */
int eq_store_applied(void);

#ifdef __cplusplus
}
#endif
