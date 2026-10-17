#include "usbi.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tusb.h"
#include "device/usbd_pvt.h"   /* usbd_class_driver_t / usbd_app_driver_get_cb（TinyUSB 私有 API） */
#include "hal/usb_serial_jtag_ll.h"   /* 关掉 ROM 留下的 USB-Serial-JTAG PHY */

#include "adau1452.h"
#include "dsp_image.h"

static const char *TAG = "usbi";

/* ================= USB 身份：伪装成 Analog Devices USBi ================= */
#define USBI_VID              0x0456
#define USBI_PID              0x7031

/* SigmaStudio 4.7 里 ADIUSBi.dll 的 vendor 命令（控制端点） */
#define USBI_CMD_WRITE        0xB2
#define USBI_CMD_WRITE_READ   0xB3
#define USBI_CMD_READ         0xB4
#define USBI_CMD_CHECK_WRITE  0xB5
#define USBI_CMD_CHECK_READ   0xB6
#define USBI_CMD_RESET        0xB7
#define USBI_CMD_POWERBOARD   0xB8
#define USBI_CMD_LED          0xB9
#define USBI_CMD_RESET_TOGGLE 0xBA

/* TransferStatus 枚举（ADIUSBi.dll）：1=Success，3=Failed */
#define USBI_ST_SUCCESS       1
#define USBI_ST_FAILED        3

/*
 * 控制端点数据缓冲。
 * I2C 模式下 SigmaStudio 每个控制传输的数据段最大 64 字节（=EP0 包长），
 * 这里留 4KB 是为了兼容它在 SPI 模式下把分块提高到 4096 的情况
 * （数据语义相同，我们统一走 I2C 转发）。
 */
#define USBI_BUF_SIZE         4096

/* ================= 描述符 ================= */
/* 设备描述符：类=厂商自定义（0xFF），单接口，无端点，全速，EP0=64 */
static const tusb_desc_device_t s_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = 64,
    .idVendor           = USBI_VID,
    .idProduct          = USBI_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 0,
    .bNumConfigurations = 1,
};

/* 字符串描述符：0 号是语言 ID（0x0409 英文），其余是 UTF-8 */
static const char *s_str_desc[] = {
    "\x09\x04",
    "Analog Devices",
    "USBi",
};

/*
 * 配置描述符（全速，总线供电，500mA）：
 *   配置(9) + 接口(9)，1 个接口、0 个端点、vendor specific 类 0xFF。
 * 没有 IAD/复合设备结构 —— Windows 会直接按 VID/PID 给本设备装 CYUSB 驱动
 * （与真实 USBi 一致，设备管理器里显示 "Analog Devices USBi (programmed)"）。
 */
static const uint8_t s_fs_cfg_desc[] = {
    9, TUSB_DESC_CONFIGURATION, 18, 0,      /* wTotalLength = 18 */
    1,                                      /* bNumInterfaces */
    1,                                      /* bConfigurationValue */
    0,                                      /* iConfiguration */
    0x80,                                   /* bmAttributes：总线供电 */
    250,                                    /* bMaxPower：500mA */
    /* ---- 接口 ---- */
    9, TUSB_DESC_INTERFACE,
    0,                                      /* bInterfaceNumber */
    0,                                      /* bAlternateSetting */
    0,                                      /* bNumEndpoints（只用控制端点） */
    0xFF,                                   /* bInterfaceClass：vendor specific */
    0,                                      /* bInterfaceSubClass */
    0,                                      /* bInterfaceProtocol */
    0,                                      /* iInterface */
};

/* ================= 状态 ================= */
static volatile bool s_enabled   = false;   /* 网页开关（NVS 持久化） */
static volatile bool s_installed = false;   /* TinyUSB 是否已安装 */
static usbi_status_t s_st;                  /* 统计（USB 任务写，其它任务读，仅用于显示） */

/* 控制端点数据缓冲（USB 任务里使用，不再并发调用） */
static uint8_t s_out_buf[USBI_BUF_SIZE];
static uint8_t s_in_buf[USBI_BUF_SIZE];

/* 读命令的地址（0xB3 设置，0xB4 使用） */
static volatile bool     s_pending_read = false;
static volatile uint16_t s_pending_addr = 0;

/* 0xB5/0xB6 返回的状态 */
static volatile uint8_t s_write_status = USBI_ST_SUCCESS;
static volatile uint8_t s_read_status  = USBI_ST_SUCCESS;

static bool s_logged_other = false;

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ================= USB 事件 ================= */
static void usbi_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_st.attach_count++;
        ESP_LOGI(TAG, "USB 已连接：主机开始枚举（等待 CYUSB 驱动接管）");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGW(TAG, "USB 已拔出");
        break;
    default:
        break;
    }
}

/* ================= 读/写 DSP ================= */

/*
 * USBi 写入的"快路径"。
 *
 * 背景：SigmaStudio 每发一条 0xB2 命令，带的就是"一段连续地址 + ≤62 字节数据"，
 * 所以一条 USB 命令本来就可以用**一次 I2C 事务**写完。原来的
 * adau1452_write_block() 是给"整片回放/冷启动"设计的保守时序
 * （PROGRAM 区 4 字节一次 + 300µs 间隔），用在 USBi 上会把每条 60 字节命令
 * 拆成 15 次事务、耗时 13.6ms，下载 18KB 程序要 5 秒。
 *
 * 这里改为：
 *   - 寄存器区（>=0xF000）：保持小块（寄存器是字节寻址，且写入有副作用，
 *     沿用原实现）
 *   - PROGRAM 区（0xC000..0xEFFF）：大块写（一次最多 248 字节 = 62 个 32 位字），
 *     每块后留 settle 时间
 *   - 参数/DM（<0xC000）：同样大块写（4 字节对齐）
 * 已用"读出 → 原值写回 → 回读比对"实测验证（含 1KB 级块写），数据一致。
 *
 * 为什么不是"一条命令一次事务"就完事：如果 SigmaStudio 以后把 _maxDataBytes
 * 调大（例如 SPI 模式的 4096），单条命令的数据会超过 I2C 单次事务的合理长度，
 * 所以这里按 248 字节切块循环。
 */
#define USBI_PROG_SETTLE_US   300      /* PROGRAM 每块写后给 DSP 的稳定时间 */
#define USBI_FAST_CHUNK       248      /* 单次 I2C 事务的数据上限（4 的倍数） */

static esp_err_t usbi_write_dsp_fast(uint16_t addr, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    const bool is_reg = (addr >= ADAU1452_ADDR_REG_BASE);
    const bool is_prog = (!is_reg && addr >= ADAU1452_ADDR_PROG_BASE);

    if (is_reg || (len % 4) != 0) {
        /* 寄存器 / 非整字（正常不会出现）：退回原来的保守实现 */
        return adau1452_write_block(addr, data, len);
    }

    size_t off = 0;
    esp_err_t err = ESP_OK;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > USBI_FAST_CHUNK) {
            chunk = USBI_FAST_CHUNK;
        }
        /* 内存区按字寻址：子地址 = addr + 字偏移 */
        err = adau1452_raw_write((uint16_t)(addr + off / 4), data + off, chunk);
        if (err != ESP_OK) {
            break;
        }
        off += chunk;
        if (is_prog) {
            esp_rom_delay_us(USBI_PROG_SETTLE_US);
        } else {
            taskYIELD();
        }
    }
    return err;
}

/* 写：data = [addrH addrL payload...]，地址语义与 TCP 桥接相同 */
static void usbi_do_write(uint16_t total_len)
{
    const int64_t t0 = esp_timer_get_time();
    uint16_t addr = be16(s_out_buf);
    size_t len = (total_len >= 2) ? (size_t)(total_len - 2) : 0;
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    bool ok = false;

    if (len > 0) {
        /* 与 TCP 桥接一致：先录制（若用户开了「开始录制」），再写 DSP */
        dsp_image_record(addr, s_out_buf + 2, len);
        err = usbi_write_dsp_fast(addr, s_out_buf + 2, len);
        ok = (err == ESP_OK);
    } else {
        ok = true;              /* 零数据写：视为成功 */
    }

    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s_st.pkt_write++;
    s_st.last_addr = addr;
    s_st.last_len = (uint32_t)len;
    s_st.last_is_read = false;
    s_st.last_us = us;
    if (us > s_st.max_us) {
        s_st.max_us = us;
    }
    s_write_status = ok ? USBI_ST_SUCCESS : USBI_ST_FAILED;
    s_st.write_status = s_write_status;
    if (!ok) {
        s_st.errors++;
        if (s_st.errors <= 5) {
            ESP_LOGW(TAG, "写 0x%04X (%u 字节) 失败：%s",
                     addr, (unsigned)len, esp_err_to_name(err));
        }
    }
}

/* 读：0xB3 已经给出地址，这里真正去 DSP 取数据。
 *
 * 小读（≤96，SigmaStudio 常规的 4~64 字节）走 adau1452_read_block()；
 * 大读（例如把 ADIUSBi 的 pack size 调到 4096 后）直接按 240 字节分块，
 * 减少每块 ~0.3-0.5ms 的驱动/调度固定开销。 */
#define USBI_FAST_READ_CHUNK  240      /* 4 的倍数，且 ≤ adau1452_raw_read 的 250 上限 */

static bool usbi_read_dsp_fast(uint16_t addr, uint8_t *buf, size_t n)
{
    const bool is_reg = (addr >= ADAU1452_ADDR_REG_BASE);
    if (is_reg || n <= 96 || (n % 4) != 0) {
        return adau1452_read_block(addr, buf, n) == ESP_OK;
    }

    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > USBI_FAST_READ_CHUNK) {
            chunk = USBI_FAST_READ_CHUNK;
        }
        if (adau1452_raw_read((uint16_t)(addr + off / 4), buf + off, chunk) != ESP_OK) {
            return false;
        }
        off += chunk;
    }
    return true;
}

static uint16_t usbi_do_read(uint16_t want)
{
    const int64_t t0 = esp_timer_get_time();
    uint16_t n = want;
    if (n == 0) {
        n = 1;                  /* 零长度没意义，按 1 字节回（不会越界） */
    }
    if (n > sizeof(s_in_buf)) {
        n = sizeof(s_in_buf);
    }

    bool ok = false;
    if (s_pending_read) {
        ok = usbi_read_dsp_fast(s_pending_addr, s_in_buf, n);
    }
    if (!ok) {
        memset(s_in_buf, 0, n); /* 保长度：绝不返回短包破坏协议 */
        s_st.errors++;
        if (s_st.errors <= 5) {
            ESP_LOGW(TAG, "读 0x%04X (%u 字节) 失败", s_pending_addr, (unsigned)n);
        }
    }

    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s_st.pkt_read++;
    s_st.last_addr = s_pending_addr;
    s_st.last_len = n;
    s_st.last_is_read = true;
    s_st.last_us = us;
    if (us > s_st.max_us) {
        s_st.max_us = us;
    }
    s_read_status = ok ? USBI_ST_SUCCESS : USBI_ST_FAILED;
    s_st.read_status = s_read_status;
    return n;
}

/* ================= vendor 控制请求 =================
 *
 * ⚠️ TinyUSB 0.21 的流程：
 *   SETUP 阶段：决定数据方向，调用 tud_control_xfer() 提供缓冲/长度
 *                （零长度命令调用 tud_control_status()）
 *   DATA  阶段：OUT 数据已被拷进我们给的缓冲，在这里处理
 *   ACK   阶段：状态阶段完成，返回 true 即可
 *
 * ESP-IDF 里 CFG_TUD_VENDOR=0，所以这个符号只有 usbd.c 里的 weak 空实现，
 * 我们在应用里给出强定义即可接管全部 vendor 请求。
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    const uint8_t req   = request->bRequest;
    const bool    is_in = (request->bmRequestType_bit.direction == TUSB_DIR_IN);
    const uint16_t wlen = request->wLength;

    if (stage == CONTROL_STAGE_SETUP) {
        s_st.last_value = request->wValue;
        s_st.last_index = request->wIndex;
        s_st.last_format = (uint8_t)(request->wIndex & 0xFF);

        if (is_in) {
            switch (req) {
            case USBI_CMD_READ: {
                const uint16_t n = usbi_do_read(wlen);
                return tud_control_xfer(rhport, request, s_in_buf, n);
            }
            case USBI_CMD_CHECK_WRITE:
                s_in_buf[0] = s_write_status;
                return tud_control_xfer(rhport, request, s_in_buf, 1);
            case USBI_CMD_CHECK_READ:
                s_in_buf[0] = s_read_status;
                return tud_control_xfer(rhport, request, s_in_buf, 1);
            default:
                /* 未知 IN 请求：回 1 字节 1，尽量别让主机卡住 */
                s_in_buf[0] = 1;
                {
                    uint16_t n = (wlen > sizeof(s_in_buf)) ? sizeof(s_in_buf) : wlen;
                    return tud_control_xfer(rhport, request, s_in_buf, n);
                }
            }
        }

        switch (req) {
        case USBI_CMD_WRITE:
        case USBI_CMD_WRITE_READ:
            if (wlen < 2 || wlen > sizeof(s_out_buf)) {
                s_st.errors++;
                ESP_LOGW(TAG, "命令 0x%02X 长度异常 %u（缓冲 %u），拒绝",
                         req, wlen, (unsigned)sizeof(s_out_buf));
                return false;             /* STALL：让 SigmaStudio 明确看到失败 */
            }
            return tud_control_xfer(rhport, request, s_out_buf, wlen);

        case USBI_CMD_RESET:
        case USBI_CMD_POWERBOARD:
        case USBI_CMD_LED:
        case USBI_CMD_RESET_TOGGLE:
            s_st.cmd_other++;
            if (!s_logged_other) {
                s_logged_other = true;
                ESP_LOGI(TAG, "收到 USBi 控制命令 0x%02X（复位/电源/LED 等，已确认）", req);
            }
            return tud_control_status(rhport, request);

        default:
            /* 未知 OUT 请求：有数据就收，没数据就 ACK，避免主机等的超时 */
            if (wlen > 0) {
                uint16_t n = (wlen > sizeof(s_out_buf)) ? sizeof(s_out_buf) : wlen;
                return tud_control_xfer(rhport, request, s_out_buf, n);
            }
            return tud_control_status(rhport, request);
        }
    }

    if (stage == CONTROL_STAGE_DATA) {
        if (!is_in) {
            switch (req) {
            case USBI_CMD_WRITE:
                usbi_do_write(wlen);
                break;
            case USBI_CMD_WRITE_READ:
                if (wlen >= 2) {
                    s_pending_addr = be16(s_out_buf);
                    s_pending_read = true;
                    s_st.last_addr = s_pending_addr;
                    s_st.last_len = 0;
                    s_st.last_is_read = true;
                    /* 地址阶段对我们来说是瞬时完成的（读操作在 0xB4 里一次做完） */
                    s_write_status = USBI_ST_SUCCESS;
                    s_st.write_status = USBI_ST_SUCCESS;
                } else {
                    s_write_status = USBI_ST_FAILED;
                }
                break;
            default:
                break;
            }
        }
        return true;
    }

    return true;                       /* ACK 阶段 */
}

/* ================= 自定义 USB 类驱动（只用于"认领接口"） =================
 *
 * TinyUSB 的 SET_CONFIGURATION 处理要求**每个接口都有驱动**（process_set_config
 * 里 drv_id < TOTAL_DRIVER_COUNT，否则整个配置失败）。我们的设备只有控制端点，
 * 没有内置类可用，所以注册一个最小的应用驱动：
 *   - open() 认领 class=0xFF 的 vendor 接口（无端点）
 *   - 真正的 vendor 控制请求仍然由上面的 tud_vendor_control_xfer_cb() 处理
 */
static void usbi_driver_init(void)
{
}

static void usbi_driver_reset(uint8_t rhport)
{
    (void)rhport;
}

static uint16_t usbi_driver_open(uint8_t rhport, tusb_desc_interface_t const *desc_itf,
                                 uint16_t max_len)
{
    (void)rhport;
    if (max_len < sizeof(tusb_desc_interface_t)) {
        return 0;
    }
    if (desc_itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }
    return sizeof(tusb_desc_interface_t);   /* 只认领接口描述符，没有端点 */
}

/* 兜底：interface 的 class 请求（正常不会来），直接拒绝 */
static bool usbi_driver_control(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    (void)rhport;
    (void)stage;
    (void)request;
    return false;
}

static const usbd_class_driver_t s_usbi_class_driver = {
    .name = "USBi",
    .init = usbi_driver_init,
    .deinit = NULL,
    .reset = usbi_driver_reset,
    .open = usbi_driver_open,
    .control_xfer_cb = usbi_driver_control,
    .xfer_cb = NULL,
    .xfer_isr = NULL,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count);
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_usbi_class_driver;
}

/* ================= 安装 / 卸载 ================= */
static esp_err_t usbi_install(void)
{
    if (s_installed) {
        return ESP_OK;
    }

    static const tinyusb_config_t cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy  = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 6144,
            /*
             * 优先级要高于 WiFi(23)：I2C 事务完成的中断里会唤醒正在等信号的
             * USB 任务，如果它比 WiFi 低，唤醒后还要等 WiFi 让出 CPU，
             * 单次 I2C 固定开销会从 ~0.3ms 涨到 ~0.6ms（实测）。
             */
            .priority = 24,
            .xCoreID = 0,
        },
        .descriptor = {
            .device = &s_dev_desc,
            .qualifier = NULL,
            .string = s_str_desc,
            .string_count = (int)(sizeof(s_str_desc) / sizeof(s_str_desc[0])),
            .full_speed_config = s_fs_cfg_desc,
            .high_speed_config = NULL,
        },
        .pm_lock_enable = false,
        .event_cb = usbi_event_cb,
        .event_arg = NULL,
    };

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB 安装失败：%s", esp_err_to_name(err));
        return err;
    }
    s_installed = true;
    ESP_LOGI(TAG, "USB 已安装：VID 0x%04X / PID 0x%04X（全速，USBi 兼容）",
             USBI_VID, USBI_PID);
    return ESP_OK;
}

static void usbi_uninstall(void)
{
    if (!s_installed) {
        return;
    }
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB 卸载：%s", esp_err_to_name(err));
    }
    s_installed = false;
    s_pending_read = false;
    ESP_LOGI(TAG, "USB 已卸载（电脑端会看到设备拔出）");
}

/* ================= 对外接口 ================= */
static void usbi_save_nvs(bool on)
{
    nvs_handle_t h;
    if (nvs_open("usbi", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "on", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t usbi_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_write_status = USBI_ST_SUCCESS;
    s_read_status  = USBI_ST_SUCCESS;

    /*
     * ESP32-S3 的 USB-OTG 和 USB-Serial-JTAG 共用同一个内部 FSLS PHY。
     * 本固件不用 USB-Serial-JTAG（控制台在 UART），但 ROM/USJ 的 PHY 焊盘
     * 复位默认是**开**的（USB_PAD_ENABLE=1、DP_PULLUP=1）：如果没人处理
     * USJ 的控制传输，电脑就会把一个"不回答描述符"的设备识别成
     * "未知 USB 设备（设备描述符请求失败）"。
     *
     * 注意：CPU 软复位（idf.py flash / RTS 复位）时 IDF 的
     * esp_perip_clk_init() 会跳过关 USJ 的动作，所以必须自己关。而且 USJ
     * 的 bus clock 可能已经被关掉，寄存器写入会被忽略（读回全 1），
     * 因此先开时钟、改完再关。
     */
    int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));
    bool usj_pad = usb_serial_jtag_ll_phy_is_pad_enabled();
    usb_serial_jtag_ll_enable_bus_clock(true);
    /* 先显式关闭 D+/D- 上拉（复位默认 DP_PULLUP=1），再关焊盘 */
    const usb_serial_jtag_pull_override_vals_t usj_no_pull = {
        .dp_pu = false, .dm_pu = false, .dp_pd = false, .dm_pd = false
    };
    usb_serial_jtag_ll_phy_enable_pull_override(&usj_no_pull);
    usb_serial_jtag_ll_phy_enable_pad(false);      /* 断开 D+/D-，电脑端设备消失 */
    bool usj_pad_after = usb_serial_jtag_ll_phy_is_pad_enabled();
    usb_serial_jtag_ll_enable_bus_clock(false);
    ESP_LOGI(TAG, "USJ PHY pad: %d -> %d", (int)usj_pad, (int)usj_pad_after);

    bool on = false;
    nvs_handle_t h;
    if (nvs_open("usbi", NVS_READONLY, &h) == ESP_OK) {
        uint8_t u8 = 0;
        if (nvs_get_u8(h, "on", &u8) == ESP_OK) {
            on = (u8 != 0);
        }
        nvs_close(h);
    }

    s_enabled = on;
    if (on) {
        esp_err_t err = usbi_install();
        if (err != ESP_OK) {
            s_enabled = false;
            return err;
        }
        ESP_LOGI(TAG, "USBi 开关=开（NVS），USB 设备已就绪");
    } else {
        ESP_LOGI(TAG, "USBi 开关=关（NVS），USB 设备不安装");
    }
    return ESP_OK;
}

esp_err_t usbi_set_enabled(bool on)
{
    if (on == s_enabled && on == s_installed) {
        return ESP_OK;
    }

    if (on) {
        esp_err_t err = usbi_install();
        if (err != ESP_OK) {
            s_enabled = false;
            return err;
        }
        s_enabled = true;
        ESP_LOGW(TAG, "USBi 已开启：DSP 交给 SigmaStudio（本地控制让位）");
    } else {
        usbi_uninstall();
        s_enabled = false;
        ESP_LOGW(TAG, "USBi 已关闭：本地控制恢复");
    }

    usbi_save_nvs(on);
    return ESP_OK;
}

bool usbi_enabled(void)
{
    return s_enabled;
}

void usbi_get_status(usbi_status_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_st;                       /* 统计只用于显示，允许轻微撕裂 */
    out->enabled   = s_enabled;
    out->installed = s_installed;
    /* TinyUSB 0.21：tud_connected()=总线上已连接；tud_mounted()=已被主机配置 */
    out->mounted    = s_installed && tud_connected();
    out->configured = s_installed && tud_mounted();
}

bool usbi_busy(void)
{
    return s_enabled && s_installed && tud_mounted();
}
