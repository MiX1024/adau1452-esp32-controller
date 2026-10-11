#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 连线图数据（SigmaStudio 导出的文本文件）持久化。
 *
 * 网页「连线图」页需要两份文件：
 *   - NetList.xml ：拓扑（方块名字 / 类型 / 坐标 / 连线）
 *   - *.params    ：每个参数的名称与地址
 *
 * 两者都可能上百 KB，而 ESP32-S3 上 WiFi + HTTP 跑起来后最大连续堆块只有 ~12KB，
 * 所以**全程流式**：不整包进内存，直接分区读写 + 分块收发。
 *
 * 分区 "dspgraph"（2MB）布局：
 *   0x000000  头（64 字节，含各文件长度 / CRC / 有效位）
 *   0x001000  NetList 文本区，最大 512KB
 *   0x081000  params  文本区，最大 512KB
 *
 * ⚠️ 上传过程按 4KB 扇区「边写边擦」，所以只擦真正用到的扇区
 * （整块擦 512KB 要好几秒，而这个文件通常只有几十 KB）。
 */

#define DSP_GRAPH_PART_LABEL  "dspgraph"

/* 单个文件上限（同时也是擦除/写入的边界） */
#define DSP_GRAPH_SLOT_MAX    (512 * 1024)

typedef enum {
    DSP_GRAPH_FILE_NETLIST = 0,   /* *_NetList.xml */
    DSP_GRAPH_FILE_PARAMS  = 1,   /* *.params     */
    DSP_GRAPH_FILE_COUNT
} dsp_graph_file_t;

typedef struct {
    bool     valid;   /* 该文件是否有有效内容 */
    uint32_t len;     /* 字节数 */
    uint32_t crc;     /* 上传时算的 CRC32，用来判断内容是否变过 */
} dsp_graph_file_info_t;

/* 挂上分区并读出头信息（未烧过分区表时返回错误，但不致命） */
esp_err_t dsp_graph_init(void);

/* 取两份文件的信息（数组长度 = DSP_GRAPH_FILE_COUNT） */
void dsp_graph_get_info(dsp_graph_file_info_t out[DSP_GRAPH_FILE_COUNT]);

/* 是否至少有一份文件 */
bool dsp_graph_any(void);

const char *dsp_graph_file_name(dsp_graph_file_t f);

/* 流式读：从 off 读 len 字节。用于 HTTP 分块回传，不整包进内存。 */
esp_err_t dsp_graph_read(dsp_graph_file_t f, size_t off, void *buf, size_t len);

/* 流式写：begin → 若干次 data → finish。中途出错用 abort。 */
esp_err_t dsp_graph_upload_begin(dsp_graph_file_t f);
esp_err_t dsp_graph_upload_data(const void *buf, size_t len);
esp_err_t dsp_graph_upload_finish(void);
void      dsp_graph_upload_abort(void);
bool      dsp_graph_uploading(void);

/* 清空两份文件（只擦头扇区，数据区留着，下次上传会按需擦） */
esp_err_t dsp_graph_clear(void);

#ifdef __cplusplus
}
#endif
