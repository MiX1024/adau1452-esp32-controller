# ADAU1452 Windows 音量控制（DspVolumeCtl）

Windows 11 托盘音量控制器，通过 WiFi 控制 ESP32-S3（ADAU1452 2.1 音频控制器）。
**单个绿色 exe，无需安装任何运行时**（仅用系统自带 .NET Framework 4.8 编译）。

---

## 一、功能

| 操作 | 效果 |
|---|---|
| **开机自启动** | 设置对话框/右键菜单勾选「开机自动启动」，写入当前用户 `Run` 注册表键 |
| **手动设置 IP** | 托盘右键 →「设置 IP 地址…」，支持 IP/主机名 + 端口，带「测试连接」 |
| **状态栏音量图标** | 托盘显示扬声器图标：声波数量随总音量变化；离线灰色、桥接/安全模式橙色、静音显示红色 ✕ |
| **悬停 + 滚轮** | 鼠标悬停托盘图标滚动滚轮，调 **MASTER 总音量**（每格 2%，0~120%），悬停提示里实时显示当前数值（不弹额外窗口） |
| **单击图标** | 展开控制面板：L/R 实时电平表 + 4 路音量滑块 + WiFi 信息 + 开关 |
| **右键菜单** | 打开控制面板 / 设置 IP / 开机自启 / 退出 |

### 展开面板内容（Win11 卡片风格）
- **状态胶囊**：DSP 在线 / DSP 离线 / 桥接调试中 / 安全模式 / 本地控制已关闭 / 未连接
- **输出电平**：L/R 双声道渐变能量条（绿→黄→橙→红，占满整行宽度）+ 白色峰值保持线 + dB 读数（60fps 平滑）
- **音量调节**：MASTER（%）、LOW / HIGH / MID（dB）4 路滑块，名字与设备端同步（绑定改名后跟着变）
- **WiFi 信息**：信号强度、信道、设备 IP
- **开关**：SigmaStudio 桥接 / 本地控制 / 安全模式（直接对接设备现有接口）
- **打开网页面板**：一键打开设备自带的网页控制台

> 桥接调试中或安全模式下，滑块自动禁用并给出原因提示（此时 DSP 不允许本地写入）。

---

## 二、编译

只需 Windows 自带的 .NET Framework 编译器，**不需要装 Visual Studio**：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

产物：`dist\DspVolumeCtl.exe`（单文件，可拷到任何 Win10/Win11 机器直接跑）。
首次编译会自动生成 `dist\app.ico` 图标；配置写在 exe 旁的 `config.json`（目录不可写时退回 `%AppData%\AdaU1452VolumeCtl\`）。

---

## 三、使用

1. 首次运行（或未配置 IP 时）自动弹出设置对话框，填入 ESP32 的 IP（可在设备网页、OLED 或路由器后台查看），点「测试连接」确认后保存。
2. 程序缩在托盘：**悬停滚轮调总音量，单击展开面板，右键出菜单**。
3. 勾选「开机自动启动」后无需再手动运行。

---

## 四、与固件的通信协议（全部 HTTP，不碰 WebSocket）

网页控制台的 WebSocket 是**单客户端槽位**，本程序刻意不使用它，
因此**和网页可以同时开**（网页看推送、本程序轮询），互不挤占。

| 接口 | 说明 |
|---|---|
| `GET /api/vol` | 轻量状态：`{dsp, safe, ch[4], level{l,r}, bridge{...}, wifi{...}}`（面板打开 100ms / 平时 300ms 轮询） |
| `POST /api/vol` | `{"idx":0..3,"value":v}` 设音量。idx=0 是 MASTER（值为 %，0~120），1/2/3 是 LOW/HIGH/MID（dB）。返回 `{"ok":true,"value":回读值}` 或 `{"ok":false,"reason":"bridge"/"safe"/...}` |
| `POST /api/bridge` | `{"enabled":bool}` 开关 SigmaStudio 桥接；`{"local_control":bool}` 开关本地控制 |
| `POST /api/safe` | `{"on":bool}` 安全模式开关 |

`/api/vol` 是固件 `main/net.c` 为本程序新增的接口（`vol_handler`），
音量写入走设备端正规 `apply_volume` 路径（含钳位、NVS 断电保存、桥接/安全模式拦截），
与网页的 `/ws` 命令、编码器完全同源。

### 主程序与固件状态的对应关系

| 面板状态 | 固件条件 |
|---|---|
| 未连接 | HTTP 轮询失败（离线自动降速到 2s 重试） |
| 桥接调试中 | `bridge.enabled = true`（本地写入被拒绝） |
| 安全模式 | `safe = true`（`apply_volume` 拒绝写 DSP） |
| 本地控制已关闭 | `bridge.local_control = false`（电平无数据源） |
| DSP 离线 | `dsp = false`（ADAU1452 无响应） |

---

## 五、源码结构

```
win-volume-ctl/
├── build.ps1            # 一键编译（系统自带 csc.exe）
├── src/
│   ├── Program.cs       # 入口 + 单实例互斥体
│   ├── TrayApp.cs       # 托盘图标 / 滚轮钩子 / 轮询 / 悬停提示 / 菜单
│   ├── FlyoutForm.cs    # 点击图标展开的控制面板
│   ├── SettingsForm.cs  # IP 设置对话框（含测试连接）
│   ├── DspClient.cs     # /api/vol 等 HTTP 接口封装（含 JSON 解析）
│   ├── Config.cs        # config.json 存取 + 开机自启（注册表 Run 键）
│   ├── NativeMethods.cs # Win32：托盘图标矩形 / 低级鼠标钩子 / DWM 圆角
│   ├── VolumeSlider.cs  # 自绘音量滑块
│   ├── LevelMeter.cs    # 自绘 L/R 电平表（渐变 + 峰值保持）
│   ├── ToggleSwitch.cs  # 自绘胶囊开关
│   ├── Theme.cs         # 深色主题配色
│   └── app.manifest     # DPI 感知 + Win11 兼容声明
└── dist/                # 编译产物（DspVolumeCtl.exe / app.ico / config.json）
```

### 实现要点（踩过的坑）

- **托盘图标收到滚轮**：Windows 不会把 `WM_MOUSEWHEEL` 直接发给 NotifyIcon。
  用 `WH_MOUSE_LL` 低级鼠标钩子 + `Shell_NotifyIconGetRect` 拿图标屏幕矩形，
  光标在矩形内才拦截滚轮并吞掉（返回 1），不会误伤任务栏其他区域。
- **单实例**：`Global\` 命名互斥体，重复运行直接退出。
- **隐藏窗体必须强制建窗**：主窗体用 `SetVisibleCore(false)` 隐藏后，WinForms 不会
  创建句柄，`OnHandleCreated` 从不触发 —— 鼠标钩子就装不上。启动时访问一次 `Handle` 强制建窗。
- **NotifyIcon 图标矩形**：图标注册在 NotifyIcon 的**内部隐藏窗口**上（不是主窗体），
  用反射取 `window`/`id` 字段查询；再加悬停位置缓存兜底。
- **滚轮反馈走托盘悬停提示**：不弹 OSD（与悬停提示重复）。改 `NotifyIcon.Text` 前
  先置空格再设真值，强制正在显示的 tooltip 立即刷新；操作反馈（如"桥接已开启"）
  临时覆盖提示约 2.5 秒后自动恢复。
- **电平表流畅度**：轮询面板打开时 100ms + 60fps 指数时间常数插值
  （上升 55ms / 下降 130ms，VU 弹道），对齐网页版手感；面板隐藏时动画自动停。
- **图标句柄**：`Icon.FromHandle` 的句柄由我们自己 `DestroyIcon`，
  且必须**先缓存句柄再 Dispose**；图标只在外观变化时重建，避免每个轮询周期 churn GDI。
- **轮询与调节竞态**：刚用滚轮/滑块改过总音量后 1.5s 内，不被轮询到的旧值覆盖显示。
- **偶发连接失败静默处理**：HttpClient 复用 keep-alive 时偶尔撞上 ESP32 已关闭的
  socket（表现为“设备无响应”误报）—— 发送失败自动重试一次，仍失败只写诊断日志、
  不弹提示；设备真离线由轮询兜底（图标变灰 + 提示“未连接”）。
- **C# 5 语法**：用系统自带 `csc.exe`（.NET Framework）编译，代码不含 C# 6+ 语法。
