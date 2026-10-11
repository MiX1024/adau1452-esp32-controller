using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace DspVolumeCtl
{
    /// <summary>
    /// 托盘主程序：图标 + 滚轮调总音量 + 点击展开面板 + 后台轮询。
    /// 通过隐藏窗体承载消息循环与定时器。
    /// </summary>
    internal class TrayApp : Form
    {
        private const uint TrayIconId = 1;

        private readonly AppConfig _cfg;
        private readonly DspClient _client;
        private DeviceState _state;
        private bool _connected;
        private bool _polling;

        private NotifyIcon _notify;
        private Icon _trayIcon;
        private int _iconWaves = -1;
        private int _iconColor = 0;
        private string _iconTip = "";
        private string _tipOverride = "";
        private DateTime _tipOverrideUntil = DateTime.MinValue;
        private ContextMenuStrip _menu;
        private ToolStripMenuItem _miAutostart;

        private readonly Timer _pollTimer = new Timer();
        private readonly Timer _sendTimer = new Timer();
        private readonly Dictionary<int, double> _pending = new Dictionary<int, double>();
        private DateTime _lastAdjust = DateTime.MinValue;

        private FlyoutForm _flyout;

        private IntPtr _hookHandle = IntPtr.Zero;
        private NativeMethods.LowLevelMouseProc _hookProc;

        // 图标位置缓存（NotifyIcon.MouseMove 时更新，作为 Shell_NotifyIconGetRect 的兜底）
        private NativeMethods.RECT _hoverRect;
        private bool _hoverRectValid;
        private int _rectMissLog;

        public TrayApp()
        {
            _cfg = AppConfig.Load();
            _client = new DspClient();
            _state = new DeviceState();

            Text = "ADAU1452 音量控制";
            ShowInTaskbar = false;
            WindowState = FormWindowState.Minimized;

            _flyout = new FlyoutForm(this);

            BuildTray();

            _pollTimer.Interval = 300;
            _pollTimer.Tick += OnPollTick;
            _pollTimer.Start();

            _sendTimer.Interval = 120;
            _sendTimer.Tick += OnSendTick;

            /*
             * ⚠️ 必须强制创建隐藏窗体句柄！
             * SetVisibleCore(false) 会阻止 WinForms 自动建窗，导致 OnHandleCreated
             * 从不触发 —— 鼠标钩子（滚轮调音量）和首次启动的设置对话框就全都不会装上。
             * 访问一次 Handle 即可强制建窗。
             */
            if (!IsHandleCreated)
            {
                IntPtr unused = Handle;
            }
        }

        public AppConfig Config
        {
            get { return _cfg; }
        }

        protected override void SetVisibleCore(bool value)
        {
            base.SetVisibleCore(false);   // 主窗体永不显示，只作消息循环载体
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            InstallHook();
            if (!_cfg.HasAddress)
            {
                BeginInvoke(new MethodInvoker(OpenSettings));
            }
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            UninstallHook();
            if (_notify != null) _notify.Visible = false;
            base.OnFormClosed(e);
        }

        // ---------- 托盘图标与菜单 ----------

        private void BuildTray()
        {
            _menu = new ContextMenuStrip();
            _menu.Font = Theme.Font(9f, false);

            ToolStripMenuItem miOpen = new ToolStripMenuItem("打开控制面板");
            miOpen.Font = Theme.Font(9f, true);
            miOpen.Click += delegate { ToggleFlyout(); };

            ToolStripMenuItem miSettings = new ToolStripMenuItem("设置 IP 地址…");
            miSettings.Click += delegate { OpenSettings(); };

            _miAutostart = new ToolStripMenuItem("开机自动启动");
            _miAutostart.CheckOnClick = true;
            _miAutostart.Checked = Autostart.IsEnabled();
            _miAutostart.CheckedChanged += delegate
            {
                Autostart.SetEnabled(_miAutostart.Checked);
            };

            ToolStripMenuItem miExit = new ToolStripMenuItem("退出");
            miExit.Click += delegate { ExitApplication(); };

            _menu.Items.Add(miOpen);
            _menu.Items.Add(miSettings);
            _menu.Items.Add(new ToolStripSeparator());
            _menu.Items.Add(_miAutostart);
            _menu.Items.Add(new ToolStripSeparator());
            _menu.Items.Add(miExit);

            _notify = new NotifyIcon();
            _notify.ContextMenuStrip = _menu;
            _notify.Text = "ADAU1452 音量控制";
            _notify.Visible = true;
            UpdateTrayIcon(false);
            _notify.MouseClick += OnTrayMouseClick;
            _notify.MouseMove += OnTrayMouseMove;   // 鼠标悬停图标时缓存图标位置
        }

        /// <summary>悬停托盘图标时 NotifyIcon 会发 MouseMove，据此缓存图标大致位置。</summary>
        private void OnTrayMouseMove(object sender, MouseEventArgs e)
        {
            NativeMethods.POINT pt;
            if (NativeMethods.GetCursorPos(out pt))
            {
                _hoverRect.Left = pt.X - 16;
                _hoverRect.Top = pt.Y - 16;
                _hoverRect.Right = pt.X + 16;
                _hoverRect.Bottom = pt.Y + 16;
                _hoverRectValid = true;
            }
        }

        /// <summary>
        /// 取托盘图标屏幕矩形。
        /// ⚠️ NotifyIcon 的图标注册在它自己的内部隐藏窗口上（不是本窗体的 Handle），
        /// 必须反射出那个窗口句柄和 id 才能查到矩形；查不到时退回悬停缓存的位置。
        /// </summary>
        private bool TryGetIconRect(out NativeMethods.RECT rc)
        {
            IntPtr hwnd = IntPtr.Zero;
            uint uid = TrayIconId;
            try
            {
                FieldInfo fiWin = typeof(NotifyIcon).GetField("window",
                    BindingFlags.Instance | BindingFlags.NonPublic);
                NativeWindow nw = fiWin != null ? fiWin.GetValue(_notify) as NativeWindow : null;
                if (nw != null) hwnd = nw.Handle;

                FieldInfo fiId = typeof(NotifyIcon).GetField("id",
                    BindingFlags.Instance | BindingFlags.NonPublic);
                if (fiId != null) uid = Convert.ToUInt32(fiId.GetValue(_notify));
            }
            catch { }

            if (hwnd != IntPtr.Zero && NativeMethods.TryGetNotifyIconRect(hwnd, uid, out rc))
            {
                return true;
            }
            if (_hoverRectValid)
            {
                rc = _hoverRect;
                return true;
            }
            rc = new NativeMethods.RECT();
            return false;
        }

        private static void Log(string msg)
        {
            try
            {
                File.AppendAllText(
                    Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "DspVolumeCtl.log"),
                    DateTime.Now.ToString("HH:mm:ss.fff ") + msg + Environment.NewLine);
            }
            catch { }
        }

        private void OnTrayMouseClick(object sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                ToggleFlyout();
            }
        }

        private void ToggleFlyout()
        {
            if (_flyout.Visible)
            {
                _flyout.Hide();
                return;
            }
            NativeMethods.RECT rc;
            if (!TryGetIconRect(out rc))
            {
                rc = new NativeMethods.RECT();
            }
            _flyout.ShowNearTray(rc);
            // 展开时立即推一次状态，避免看到旧值
            if (_connected && _state != null)
            {
                _flyout.ApplyState(_state, true);
            }
        }

        private void UpdateTrayIcon(bool connected)
        {
            DeviceState st = _state;
            Color c;
            if (!connected) c = Color.FromArgb(122, 124, 132);
            else if (st.BridgeEnabled || st.Safe) c = Theme.Warn;
            else c = Color.FromArgb(235, 235, 242);

            int waves = 3;
            if (st != null)
            {
                double v = st.Ch[0].Value;
                waves = v <= 0.5 ? 0 : (v < 40 ? 1 : (v < 80 ? 2 : 3));
            }

            // 图标只在外观变化时重建（否则每个轮询周期都 churn GDI 句柄）
            if (_iconWaves != waves || _iconColor != c.ToArgb())
            {
                _iconWaves = waves;
                _iconColor = c.ToArgb();
                RedrawTrayIcon(c, waves);
            }

            if (_notify != null)
            {
                string tip;
                if (!connected) tip = "ADAU1452 音量控制 — 未连接";
                else
                {
                    string master = st.Ch[0].Name + " " + st.Ch[0].Value.ToString("0") + "%";
                    string state = st.BridgeEnabled ? "桥接调试中" : (st.Safe ? "安全模式" : (st.DspOnline ? "DSP 在线" : "DSP 离线"));
                    tip = master + " · " + state;
                    if (tip.Length > 120) tip = tip.Substring(0, 120);
                }
                SetTrayTip(tip);
            }
        }

        /// <summary>更新托盘悬停提示；先置空格再设真值，强制正在显示的 tooltip 立即刷新。</summary>
        private void SetTrayTip(string tip)
        {
            if (DateTime.Now < _tipOverrideUntil && _tipOverride.Length > 0)
            {
                tip = _tipOverride;
            }
            if (_iconTip == tip) return;
            _iconTip = tip;
            _notify.Text = " ";
            _notify.Text = tip;
        }

        /// <summary>临时把提示换成操作反馈（约 2.5 秒后随轮询恢复正常提示）。</summary>
        private void SetTipOverride(string text)
        {
            _tipOverride = text;
            _tipOverrideUntil = DateTime.Now.AddSeconds(2.5);
            _iconTip = "";
            SetTrayTip(text);
        }

        private void RedrawTrayIcon(Color c, int waves)
        {
            // 64×64 高分辨率绘制（系统缩放到 16/20/24px 更清晰），
            // 字形占满画布几乎整个区域 —— 之前只占一半，状态栏里显得特别小
            Bitmap bmp = new Bitmap(64, 64);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);

                // 扬声器箱体 + 号筒
                GraphicsPath body = new GraphicsPath();
                body.AddPolygon(new PointF[]
                {
                    new PointF(3f, 24f), new PointF(15f, 24f),
                    new PointF(27f, 11f), new PointF(27f, 53f),
                    new PointF(15f, 40f), new PointF(3f, 40f),
                });
                g.FillPath(new SolidBrush(c), body);

                using (Pen p = new Pen(c, 5f))
                {
                    p.StartCap = LineCap.Round;
                    p.EndCap = LineCap.Round;
                    if (waves >= 1) g.DrawArc(p, 15f, 20f, 24f, 24f, -55f, 110f);
                    if (waves >= 2) g.DrawArc(p, 9f, 14f, 36f, 36f, -55f, 110f);
                    if (waves >= 3) g.DrawArc(p, 3f, 8f, 48f, 48f, -55f, 110f);
                    if (waves == 0)
                    {
                        using (Pen red = new Pen(Theme.Err, 5f))
                        {
                            red.StartCap = LineCap.Round;
                            red.EndCap = LineCap.Round;
                            g.DrawLine(red, 38f, 18f, 56f, 46f);
                            g.DrawLine(red, 56f, 18f, 38f, 46f);
                        }
                    }
                }
            }

            IntPtr hIcon = bmp.GetHicon();
            Icon newIcon = Icon.FromHandle(hIcon);
            Icon old = _trayIcon;
            _trayIcon = newIcon;
            if (_notify != null) _notify.Icon = newIcon;
            if (old != null)
            {
                IntPtr hOld = old.Handle;   // 先取句柄再销毁（Dispose 后再取会抛异常）
                old.Dispose();
                NativeMethods.DestroyIcon(hOld);
            }
            bmp.Dispose();
        }

        // ---------- 滚轮钩子 ----------

        private void InstallHook()
        {
            if (_hookHandle != IntPtr.Zero) return;
            _hookProc = MouseHookProc;
            _hookHandle = NativeMethods.SetWindowsHookEx(
                NativeMethods.WH_MOUSE_LL, _hookProc,
                NativeMethods.GetModuleHandle(null), 0);
            Log("hook installed: " + (_hookHandle != IntPtr.Zero ? "OK" : "FAILED err=" +
                Marshal.GetLastWin32Error().ToString()));
        }

        private void UninstallHook()
        {
            if (_hookHandle != IntPtr.Zero)
            {
                NativeMethods.UnhookWindowsHookEx(_hookHandle);
                _hookHandle = IntPtr.Zero;
            }
        }

        private IntPtr MouseHookProc(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode >= 0 && (int)wParam == NativeMethods.WM_MOUSEWHEEL)
            {
                NativeMethods.RECT rc;
                if (TryGetIconRect(out rc))
                {
                    NativeMethods.POINT pt;
                    NativeMethods.GetCursorPos(out pt);
                    if (pt.X >= rc.Left && pt.X <= rc.Right && pt.Y >= rc.Top && pt.Y <= rc.Bottom)
                    {
                        NativeMethods.MSLLHOOKSTRUCT data = (NativeMethods.MSLLHOOKSTRUCT)
                            Marshal.PtrToStructure(lParam, typeof(NativeMethods.MSLLHOOKSTRUCT));
                        int delta = (short)((data.MouseData >> 16) & 0xFFFF);
                        int notches = delta / 120;
                        if (notches == 0) notches = delta > 0 ? 1 : -1;
                        OnTrayWheel(notches);
                        return (IntPtr)1;   // 吞掉：不把滚轮传给任务栏
                    }
                }
                else if (_rectMissLog < 3)
                {
                    _rectMissLog++;
                    Log("wheel: 托盘图标矩形未知，忽略（miss " + _rectMissLog.ToString() + "）");
                }
            }
            return NativeMethods.CallNextHookEx(_hookHandle, nCode, wParam, lParam);
        }

        private void OnTrayWheel(int notches)
        {
            Log("wheel: " + (notches > 0 ? "+" : "") + notches.ToString() +
                " connected=" + _connected.ToString());

            if (!_connected)
            {
                SetTipOverride("未连接：请检查 WiFi 或 IP 设置");
                return;
            }

            ChannelState m = _state.Ch[0];
            double v = m.Value + notches * _cfg.WheelStep;
            if (v < m.Min) v = m.Min;
            if (v > m.Max) v = m.Max;
            m.Value = v;
            _lastAdjust = DateTime.Now;

            // 数值反馈直接走托盘悬停提示（不弹 OSD，避免与悬停提示重复）
            OnVolumeEdited(0, v);
            if (_flyout.Visible) _flyout.ApplyState(_state, true);
            UpdateTrayIcon(true);
        }

        // ---------- 音量/开关的用户操作入口（FlyoutForm 调用） ----------

        public void OnVolumeEdited(int idx, double value)
        {
            _pending[idx] = value;
            if (!_sendTimer.Enabled) _sendTimer.Start();
            if (idx == 0) _lastAdjust = DateTime.Now;
        }

        private async void OnSendTick(object sender, EventArgs e)
        {
            _sendTimer.Stop();
            if (_pending.Count == 0) return;

            List<KeyValuePair<int, double>> batch = new List<KeyValuePair<int, double>>(_pending);
            _pending.Clear();

            foreach (KeyValuePair<int, double> kv in batch)
            {
                int idx = kv.Key;
                double value = kv.Value;
                try
                {
                    SetResult r = await _client.SetVolumeAsync(_cfg, idx, value);
                    if (r.Ok)
                    {
                        _state.Ch[idx].Value = r.Value;
                    }
                    else
                    {
                        string msg = r.Reason == "bridge" ? "桥接调试中，音量由 SigmaStudio 管理"
                                   : r.Reason == "safe" ? "安全模式下不写 DSP"
                                   : "写入失败";
                        SetTipOverride(_state.Ch[idx].Name + "：" + msg);
                    }
                }
                catch
                {
                    // 偶发的连接复用失败不代表设备离线（下一次轮询会核实），
                    // 静默忽略即可 —— 别用"设备无响应"打扰用户
                    Log("set volume failed (ignored)");
                }
            }
            if (_flyout.Visible) _flyout.ApplyState(_state, _connected);
        }

        public async void OnBridgeToggled(bool on)
        {
            try
            {
                await _client.SetBridgeAsync(_cfg, on);
                SetTipOverride("SIGMASTUDIO 桥接：" + (on ? "已开启" : "已关闭"));
            }
            catch
            {
                SetTipOverride("SIGMASTUDIO 桥接：设置失败");
            }
            ForcePoll();
        }

        public async void OnLocalToggled(bool on)
        {
            try
            {
                await _client.SetLocalControlAsync(_cfg, on);
                SetTipOverride("本地控制：" + (on ? "已开启" : "已关闭"));
            }
            catch
            {
                SetTipOverride("本地控制：设置失败");
            }
            ForcePoll();
        }

        public async void OnSafeToggled(bool on)
        {
            try
            {
                await _client.SetSafeAsync(_cfg, on);
                SetTipOverride("安全模式：" + (on ? "已开启" : "已关闭"));
            }
            catch
            {
                SetTipOverride("安全模式：设置失败");
            }
            ForcePoll();
        }

        public void OpenWebPanel()
        {
            try
            {
                Process.Start(_cfg.BaseUrl);
            }
            catch { }
        }

        public void OpenSettings()
        {
            using (SettingsForm dlg = new SettingsForm(this))
            {
                if (dlg.ShowDialog(this) == DialogResult.OK)
                {
                    _cfg.Ip = dlg.NewIp;
                    _cfg.Port = dlg.NewPort;
                    _cfg.Save();
                    if (_miAutostart != null) _miAutostart.Checked = Autostart.IsEnabled();
                    ForcePoll();
                }
            }
        }

        public void ExitApplication()
        {
            UninstallHook();
            if (_notify != null)
            {
                _notify.Visible = false;
                _notify.Dispose();
            }
            if (_trayIcon != null)
            {
                IntPtr h = _trayIcon.Handle;
                _trayIcon.Dispose();
                NativeMethods.DestroyIcon(h);
            }
            Application.Exit();
        }

        // ---------- 轮询 ----------

        private void ForcePoll()
        {
            _pollTimer.Interval = 300;
            _pollTimer.Stop();
            _pollTimer.Start();
        }

        private async void OnPollTick(object sender, EventArgs e)
        {
            if (_polling) return;
            if (!_cfg.HasAddress) return;
            _polling = true;
            try
            {
                DeviceState st = await _client.PollAsync(_cfg);

                // 刚用滚轮/滑块调过总音量时，短窗口内不被轮询旧值覆盖
                if ((DateTime.Now - _lastAdjust).TotalSeconds < 1.5)
                {
                    st.Ch[0].Value = _state.Ch[0].Value;
                }

                _state = st;
                _connected = true;
                if (_flyout.Visible) _flyout.ApplyState(st, true);
                UpdateTrayIcon(true);
                _pollTimer.Interval = _flyout.Visible ? 150 : 300;
            }
            catch
            {
                _connected = false;
                if (_flyout.Visible) _flyout.ApplyState(_state, false);
                UpdateTrayIcon(false);
                _pollTimer.Interval = 2000;   // 离线时放慢重试0
            }
            finally
            {
                _polling = false;
            }
        }
    }
}
