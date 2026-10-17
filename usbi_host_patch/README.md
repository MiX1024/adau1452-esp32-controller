# USBi 主机端加速（可选）：替换 `ADIUSBi.dll`

> ⚠️ **实验性、可逆**：只对本项目的 ESP32-S3 USBi 有效。使用真实 USBi 仿真器前请先还原原版 DLL。
> 安装/还原脚本都会先检查 SigmaStudio 是否在运行，并自动备份原文件。

## 为什么原版慢

SigmaStudio 通过 `ADIUSBi.dll` + `CyUSB.sys` 用**控制端点**跟 USBi 通信。原版 DLL 为 FX2 固件做的设计：

1. 每次 USB 控制传输最多 **64 字节** —— 下载 18.9KB 程序要 **316 次往返**；
2. 每次读/写之后还要额外发 `0xB5`/`0xB6` 状态轮询 —— 一次 4 字节读实际要 **4 次控制传输**。

而 ESP32-S3 的原生 USB 是**全速（12Mbps）**，一次控制传输约 **4ms**（≈4 个 1ms 帧，与数据量无关）。
FX2（CY7C68013A）是高速（480Mbps），同样的控制传输约 0.5ms —— 这就是"同样的 I2C，CY7C68013A 更快"的主因。
I2C 本身并不是瓶颈：4 字节读的 I2C 只占 0.3~0.6ms。

## 本 DLL 做了什么

| 优化 | 说明 |
|---|---|
| 跳过 `0xB5`/`0xB6` 轮询 | ESP32 固件在控制传输内**同步完成** I2C，传输结束即成功，不需要再查状态 |
| 传输块 64 → 4096 字节 | 写 18.9KB 程序从 316 次往返降到 **5 次** |

固件侧也已同步优化：USBi 写入不再按"4 字节 + 300µs"拆块（那是冷启动回放用的保守时序），
而是按一条命令一次大块 I2C 写，PROGRAM 写速度提升约 6~7 倍（60 字节命令 13.6ms → 2.0ms）。

## 实测（ESP32-S3 USBi，I2C 400kHz）

| 操作 | 原版 ADIUSBi.dll | 本 DLL | 提升 |
|---|---|---|---|
| 4 字节读 | 16.2 ms | **8.0 ms** | 2.0× |
| 64 字节读 | 17.4 ms | **9.2 ms** | 1.9× |
| 4 字节写 | 8.1 ms | **4.0 ms** | 2.0× |
| 4KB 程序块写 | —（按 64B 拆成 64 次） | **169 ms** | — |
| 18.9KB 程序下载（估算） | ~5.0 s | **~0.9 s** | ~5× |

## 安装

前提：Windows 上已装 SigmaStudio 4.7（含 `CyUSB.dll`），ESP32 固件已烧录并打开 USBi 开关。

```powershell
cd usbi_host_patch

# 1) 编译（需要 .NET Framework 自带的 csc.exe；不需要装 VS）
powershell -ExecutionPolicy Bypass -File build.ps1 -SigmaStudioDir "D:\APPS\SigmaStudio 4.7"

# 2) 关闭 SigmaStudio，然后安装（原版备份为 ADIUSBi.dll.orig）
powershell -ExecutionPolicy Bypass -File install.ps1 -SigmaStudioDir "D:\APPS\SigmaStudio 4.7"
```

之后正常打开 SigmaStudio → Hardware Configuration → USBi → 在线调参 / Link Compile Download。

## 还原

```powershell
powershell -ExecutionPolicy Bypass -File uninstall.ps1 -SigmaStudioDir "D:\APPS\SigmaStudio 4.7"
```

## 已验证 / 未验证

- ✅ 用测试程序直接调用本 DLL 的公开 API（与 SigmaStudio 相同的调用方式）：
  读 DSP 总音量 `0x007A` = `0x000F9AA9`（-24.3dB，与设备 NVS 一致）、原值写回后回读一致、
  4KB 程序区读出→写回→回读一致、`GetWriteStatus/GetReadStatus/PowerBoard/EnableLED` 正常
- ❓ **尚未在真实 SigmaStudio 界面里跑过**（需要你在 GUI 里点 Link Compile Download 验证）。
  如果 SigmaStudio 报错/找不到设备：运行 `uninstall.ps1` 立刻还原，然后用原版 DLL 继续。

## 注意

- SigmaStudio 4.7 与 SigmaStudio+ 3.3 的安装目录不同，两个都要加速就分别对各自目录执行一次。
- 该 DLL 仅供本项目使用；**接真实 USBi（CY7C68013A）时请还原原版**（真机固件是否支持 4096/免轮询未验证）。
- 替换前请确认 `SigmaStudioDir` 下确实有 `ADIUSBi.dll` 与 `CyUSB.dll`；脚本不会删除原文件（只改名备份）。
