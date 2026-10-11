/*
 * wifi_secrets.example.h —— WiFi 默认值模板（**这个文件会提交到仓库**）
 *
 * 用法：
 *   1. 复制本文件为同目录下的 `wifi_secrets.h`
 *      （PowerShell: Copy-Item main\wifi_secrets.example.h main\wifi_secrets.h）
 *   2. 把下面 4 个宏改成你自己的值
 *   3. `wifi_secrets.h` 已在 .gitignore 中排除，不会被提交
 *
 * 若 `wifi_secrets.h` 不存在，net.c 会回退到内置通用值
 * （热点 DSP-2.1 / 12345678，STA 留空 = 默认不连路由器），
 * 设备仍可通过网页「WiFi 设置」卡片配置并写入 NVS。
 */
#ifndef __WIFI_SECRETS_H__
#define __WIFI_SECRETS_H__

/* 设备自建热点（AP）——给别人连的设备热点，保留默认即可 */
#define WIFI_DEFAULT_AP_SSID   "DSP-2.1"
#define WIFI_DEFAULT_AP_PASS   "12345678"

/* 设备要接入的家庭/实验室路由器（STA）——填你自己的 */
#define WIFI_DEFAULT_STA_SSID  "YOUR_WIFI_SSID"
#define WIFI_DEFAULT_STA_PASS  "YOUR_WIFI_PASSWORD"

#endif /* __WIFI_SECRETS_H__ */
