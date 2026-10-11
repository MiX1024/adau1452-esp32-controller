/*
 * 连线图数据持久化（SigmaStudio 导出的 NetList.xml + *.params）
 *
 * 设计要点：
 *  1. **全程流式**，不整包进内存 —— S3 上 WiFi+HTTP 之后最大连续堆块只有 ~12KB，
 *     而这两个文件几百 KB。读用 esp_partition_read 分块，写按 4KB 扇区边擦边写。
 *  2. 只擦真正用到的扇区。整块擦 512KB 要好几秒，而实际文件通常只有几十 KB。
 *  3. 头里存 len + CRC32，网页据此判断「设备上的文件和本地是不是同一份」。
 */

#include "dsp_graph.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "dspgraph";

#define DG_MAGIC    0x31475244u   /* 'D','G','R','1'（小端） */
#define DG_VERSION  1u
#define DG_HDR_OFF  0
#define DG_BUF_SIZE 4096          /* 与 flash 扇区同尺寸：一次 flush = 正好一个扇区 */

/* 两份文件在分区内的起始偏移 */
static const uint32_t DG_FILE_OFF[DSP_GRAPH_FILE_COUNT] = {
    0x001000,   /* NetList */
    0x081000,   /* params  */
};
static const char *DG_FILE_NAME[DSP_GRAPH_FILE_COUNT] = {
    "NetList.xml",
    "params",
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t len[DSP_GRAPH_FILE_COUNT];
    uint32_t crc[DSP_GRAPH_FILE_COUNT];
    uint32_t valid[DSP_GRAPH_FILE_COUNT];
    uint32_t reserved[8];
} dg_hdr_t;

_Static_assert(sizeof(dg_hdr_t) == 64, "dg_hdr_t 必须是 64 字节");

static const esp_partition_t *s_part;
static bool     s_ok;                      /* 分区可用 */
static dg_hdr_t s_hdr;                     /* 头缓存 */
static size_t   s_erase;                   /* 扇区大小 */

/* 上传上下文（同一时刻只允许一个上传） */
static struct {
    bool             active;
    dsp_graph_file_t f;
    uint32_t         off;            /* 已接收字节数 */
    uint32_t         crc;
    uint32_t         erased_upto;    /* [0, erased_upto) 已擦除 */
    size_t           fill;
    uint8_t          buf[DG_BUF_SIZE];
} s_up;

/* ================= 头读写 ================= */

static void hdr_reset(void)
{
    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr.magic = DG_MAGIC;
    s_hdr.version = DG_VERSION;
}

static void hdr_save(void)
{
    /* 头 64 字节落在第 0 扇区，写前先擦（sizeof 是 4 的倍数，偏移 0 对齐） */
    esp_partition_erase_range(s_part, DG_HDR_OFF, s_erase);
    esp_err_t e = esp_partition_write(s_part, DG_HDR_OFF, &s_hdr, sizeof(s_hdr));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "写头失败：%s", esp_err_to_name(e));
    }
}

static void hdr_load(void)
{
    dg_hdr_t tmp;
    if (esp_partition_read(s_part, DG_HDR_OFF, &tmp, sizeof(tmp)) != ESP_OK) {
        hdr_reset();
        return;
    }
    if (tmp.magic != DG_MAGIC || tmp.version != DG_VERSION) {
        ESP_LOGI(TAG, "分区无有效头（首次使用），按空处理");
        hdr_reset();
        return;
    }
    s_hdr = tmp;
    /* 防御：长度非法的一律当无效，避免网页拿到越界数据 */
    for (int i = 0; i < DSP_GRAPH_FILE_COUNT; i++) {
        if (s_hdr.len[i] > DSP_GRAPH_SLOT_MAX) {
            ESP_LOGW(TAG, "%s 长度非法（%lu），忽略", DG_FILE_NAME[i],
                     (unsigned long)s_hdr.len[i]);
            s_hdr.valid[i] = 0;
            s_hdr.len[i] = 0;
        }
    }
}

/* ================= 对外接口 ================= */

esp_err_t dsp_graph_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY,
                                      DSP_GRAPH_PART_LABEL);
    if (s_part == NULL) {
        ESP_LOGW(TAG, "找不到分区 \"%s\"（分区表未更新？）连线图功能不可用",
                 DSP_GRAPH_PART_LABEL);
        hdr_reset();
        return ESP_ERR_NOT_FOUND;
    }
    s_erase = s_part->erase_size ? s_part->erase_size : 4096;
    if (s_part->size < DG_FILE_OFF[DSP_GRAPH_FILE_COUNT - 1] + DSP_GRAPH_SLOT_MAX) {
        ESP_LOGW(TAG, "分区 %lu 字节偏小，两份文件可能写不下",
                 (unsigned long)s_part->size);
    }
    s_ok = true;
    hdr_load();
    ESP_LOGI(TAG, "分区 \"%s\" @0x%08lx 大小 %lu 字节；NetList %s(%lu) / params %s(%lu)",
             DSP_GRAPH_PART_LABEL, (unsigned long)s_part->address,
             (unsigned long)s_part->size,
             s_hdr.valid[0] ? "有" : "无", (unsigned long)s_hdr.len[0],
             s_hdr.valid[1] ? "有" : "无", (unsigned long)s_hdr.len[1]);
    return ESP_OK;
}

void dsp_graph_get_info(dsp_graph_file_info_t out[DSP_GRAPH_FILE_COUNT])
{
    for (int i = 0; i < DSP_GRAPH_FILE_COUNT; i++) {
        out[i].valid = s_ok && s_hdr.valid[i];
        out[i].len   = s_ok ? s_hdr.len[i] : 0;
        out[i].crc   = s_ok ? s_hdr.crc[i] : 0;
    }
}

bool dsp_graph_any(void)
{
    return s_ok && (s_hdr.valid[0] || s_hdr.valid[1]);
}

const char *dsp_graph_file_name(dsp_graph_file_t f)
{
    return ((int)f >= 0 && f < DSP_GRAPH_FILE_COUNT) ? DG_FILE_NAME[f] : "?";
}

esp_err_t dsp_graph_read(dsp_graph_file_t f, size_t off, void *buf, size_t len)
{
    if (!s_ok || (int)f < 0 || f >= DSP_GRAPH_FILE_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }
    if (off >= s_hdr.len[f]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (off + len > s_hdr.len[f]) {
        len = s_hdr.len[f] - off;
    }
    return esp_partition_read(s_part, DG_FILE_OFF[f] + off, buf, len);
}

/* ================= 上传 ================= */

bool dsp_graph_uploading(void)
{
    return s_up.active;
}

esp_err_t dsp_graph_upload_begin(dsp_graph_file_t f)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((int)f < 0 || f >= DSP_GRAPH_FILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_up.active) {
        ESP_LOGW(TAG, "上一个上传未结束，丢弃它");
        dsp_graph_upload_abort();
    }
    s_up.active = true;
    s_up.f = f;
    s_up.off = 0;
    s_up.crc = 0;
    s_up.erased_upto = 0;
    s_up.fill = 0;
    ESP_LOGI(TAG, "开始接收 %s", DG_FILE_NAME[f]);
    return ESP_OK;
}

/* 把一个满/半满缓冲落盘（先把长度补齐到 4 字节，flash 写入要求对齐） */
static esp_err_t up_flush(void)
{
    if (s_up.fill == 0) {
        return ESP_OK;
    }
    size_t n = s_up.fill;
    size_t wr = (n + 3u) & ~(size_t)3u;
    if (wr > n) {
        memset(s_up.buf + n, 0, wr - n);
    }
    uint32_t abs = DG_FILE_OFF[s_up.f] + s_up.off;

    /* 只在真正要用到的扇区上做擦除（buffer 就是一个扇区大小，所以每 flush 一次正好一扇区） */
    if (abs >= s_up.erased_upto) {
        size_t er = ((wr + s_erase - 1) / s_erase) * s_erase;
        esp_err_t e = esp_partition_erase_range(s_part, abs, er);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "擦除 0x%05lx 失败：%s", (unsigned long)abs, esp_err_to_name(e));
            return e;
        }
        s_up.erased_upto = abs + er;
    }

    esp_err_t e = esp_partition_write(s_part, abs, s_up.buf, wr);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "写入 0x%05lx 失败：%s", (unsigned long)abs, esp_err_to_name(e));
        return e;
    }
    s_up.off += (uint32_t)n;
    s_up.fill = 0;
    return ESP_OK;
}

esp_err_t dsp_graph_upload_data(const void *buf, size_t len)
{
    if (!s_up.active) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        size_t space = DG_BUF_SIZE - s_up.fill;
        size_t n = (len < space) ? len : space;
        memcpy(s_up.buf + s_up.fill, p, n);
        s_up.crc = esp_rom_crc32_le(s_up.crc, p, n);
        s_up.fill += n;
        p += n;
        len -= n;
        if (s_up.off + s_up.fill > DSP_GRAPH_SLOT_MAX) {
            ESP_LOGE(TAG, "%s 超过上限 %u 字节", DG_FILE_NAME[s_up.f],
                     (unsigned)DSP_GRAPH_SLOT_MAX);
            return ESP_ERR_INVALID_SIZE;
        }
        if (s_up.fill == DG_BUF_SIZE) {
            esp_err_t e = up_flush();
            if (e != ESP_OK) {
                return e;
            }
        }
    }
    return ESP_OK;
}

esp_err_t dsp_graph_upload_finish(void)
{
    if (!s_up.active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = up_flush();
    if (e != ESP_OK) {
        dsp_graph_upload_abort();
        return e;
    }
    dsp_graph_file_t f = s_up.f;
    s_hdr.len[f]   = s_up.off;
    s_hdr.crc[f]   = s_up.crc;
    s_hdr.valid[f] = (s_up.off > 0) ? 1 : 0;
    s_up.active = false;
    hdr_save();
    ESP_LOGI(TAG, "%s 接收完成：%lu 字节，CRC=0x%08lx",
             DG_FILE_NAME[f], (unsigned long)s_hdr.len[f], (unsigned long)s_hdr.crc[f]);
    return ESP_OK;
}

void dsp_graph_upload_abort(void)
{
    if (s_up.active) {
        ESP_LOGW(TAG, "%s 上传中止（已收 %lu 字节，未生效）",
                 DG_FILE_NAME[s_up.f], (unsigned long)s_up.off);
        s_up.active = false;
        s_up.fill = 0;
    }
}

esp_err_t dsp_graph_clear(void)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    dsp_graph_upload_abort();
    hdr_reset();
    hdr_save();          /* 只擦头扇区；数据区留着，下次上传会按需擦 */
    ESP_LOGI(TAG, "连线图数据已清空");
    return ESP_OK;
}
