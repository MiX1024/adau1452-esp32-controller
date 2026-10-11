using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace DspVolumeCtl
{
    /// <summary>
    /// 托盘图标点击后展开的控制面板：
    /// 状态胶囊 + L/R 电平表 + 4 路音量滑块 + WiFi/设备信息 + 桥接/本地控制/安全模式开关。
    /// </summary>
    internal class FlyoutForm : Form
    {
        private readonly TrayApp _owner;
        private readonly Label _title;
        private readonly Button _btnGear;
        private readonly Button _btnClose;
        private readonly Button _btnWeb;
        private readonly LevelMeter _meter;
        private readonly Label[] _names = new Label[4];
        private readonly VolumeSlider[] _sliders = new VolumeSlider[4];
        private readonly Label[] _values = new Label[4];
        private readonly Label _secLevel;
        private readonly Label _secVolume;
        private readonly ToggleSwitch _tglBridge;
        private readonly ToggleSwitch _tglLocal;
        private readonly ToggleSwitch _tglSafe;
        private readonly Label _lblBridge;
        private readonly Label _lblLocal;
        private readonly Label _lblSafe;
        private readonly Label _hint;

        private bool _updatingUi;
        private string _pillText = "未连接";
        private Color _pillColor = Theme.TextDim;
        private string _infoText = "";
        private DeviceState _last;

        public FlyoutForm(TrayApp owner)
        {
            _owner = owner;

            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            ShowInTaskbar = false;
            TopMost = true;
            BackColor = Theme.Back;
            ClientSize = new Size(400, 668);
            KeyPreview = true;

            // 双缓冲：否则每次轮询 Invalidate 都会把 OnPaint 画的状态胶囊/信息行闪一下
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            DoubleBuffered = true;

            _title = new Label();
            _title.Text = "ADAU1452 音量控制";
            _title.Font = Theme.Font(12f, true);
            _title.ForeColor = Theme.Text;
            _title.BackColor = Color.Transparent;
            _title.Location = new Point(22, 12);
            _title.Size = new Size(240, 30);
            _title.TextAlign = ContentAlignment.MiddleLeft;
            Controls.Add(_title);

            _btnGear = MakeFlatButton("⚙", new Point(302, 10), new Size(34, 30));
            _btnGear.Font = new Font("Segoe UI Symbol", 11f);
            _btnGear.Click += delegate { _owner.OpenSettings(); };

            _btnClose = MakeFlatButton("✕", new Point(344, 10), new Size(34, 30));
            _btnClose.Font = new Font("Segoe UI Symbol", 10f);
            _btnClose.Click += delegate { Hide(); };

            _secLevel = MakeSectionLabel("输出电平", 88);
            _meter = new LevelMeter();
            _meter.Location = new Point(34, 112);
            _meter.Size = new Size(332, 74);
            Controls.Add(_meter);

            _secVolume = MakeSectionLabel("音量调节", 216);

            for (int i = 0; i < 4; i++)
            {
                int y = 240 + i * 48;

                _names[i] = new Label();
                _names[i].Font = Theme.Font(10f, true);
                _names[i].ForeColor = Theme.Text;
                _names[i].BackColor = Color.Transparent;
                _names[i].Location = new Point(34, y + 11);
                _names[i].Size = new Size(64, 24);
                _names[i].TextAlign = ContentAlignment.MiddleLeft;
                Controls.Add(_names[i]);

                _sliders[i] = new VolumeSlider();
                _sliders[i].Location = new Point(100, y + 8);
                _sliders[i].Size = new Size(182, 28);
                int idx = i;
                _sliders[i].ValueChanged += delegate
                {
                    if (_updatingUi) return;
                    _values[idx].Text = FormatValue(_sliders[idx]);
                    _owner.OnVolumeEdited(idx, _sliders[idx].Value);
                };
                Controls.Add(_sliders[i]);

                _values[i] = new Label();
                _values[i].Font = Theme.Font(9.5f, false);
                _values[i].ForeColor = Theme.Text;
                _values[i].BackColor = Color.Transparent;
                _values[i].Location = new Point(290, y + 10);
                _values[i].Size = new Size(76, 24);
                _values[i].TextAlign = ContentAlignment.MiddleRight;
                Controls.Add(_values[i]);
            }

            // ---- 开关区 ----
            _lblBridge = MakeToggleLabel("SigmaStudio 桥接", 482);
            _tglBridge = MakeToggle(482);
            _tglBridge.CheckChanged += delegate
            {
                if (_updatingUi) return;
                _owner.OnBridgeToggled(_tglBridge.Checked);
            };

            _lblLocal = MakeToggleLabel("本地控制（电平表数据源）", 516);
            _tglLocal = MakeToggle(516);
            _tglLocal.CheckChanged += delegate
            {
                if (_updatingUi) return;
                _owner.OnLocalToggled(_tglLocal.Checked);
            };

            _lblSafe = MakeToggleLabel("安全模式（上电不写 DSP）", 550);
            _tglSafe = MakeToggle(550);
            _tglSafe.CheckChanged += delegate
            {
                if (_updatingUi) return;
                _owner.OnSafeToggled(_tglSafe.Checked);
            };

            _btnWeb = MakeFlatButton("打开网页面板", new Point(16, 626), new Size(130, 30));
            _btnWeb.Font = Theme.Font(9f, false);
            _btnWeb.Click += delegate { _owner.OpenWebPanel(); };

            _hint = new Label();
            _hint.Font = Theme.Font(8.5f, false);
            _hint.ForeColor = Theme.TextDim;
            _hint.BackColor = Color.Transparent;
            _hint.Location = new Point(156, 626);
            _hint.Size = new Size(228, 30);
            _hint.TextAlign = ContentAlignment.MiddleLeft;
            Controls.Add(_hint);
        }

        // ---------- 控件工厂 ----------

        private Button MakeFlatButton(string text, Point loc, Size size)
        {
            Button b = new Button();
            b.Text = text;
            b.Location = loc;
            b.Size = size;
            b.FlatStyle = FlatStyle.Flat;
            b.FlatAppearance.BorderSize = 0;
            b.FlatAppearance.MouseOverBackColor = Theme.Panel;
            b.FlatAppearance.MouseDownBackColor = Theme.Track;
            b.BackColor = Theme.Back;
            b.ForeColor = Theme.Text;
            b.Cursor = Cursors.Hand;
            Controls.Add(b);
            return b;
        }

        private Label MakeSectionLabel(string text, int y)
        {
            Label l = new Label();
            l.Text = text;
            l.Font = Theme.Font(9.5f, true);
            l.ForeColor = Theme.TextDim;
            l.BackColor = Color.Transparent;
            l.Location = new Point(34, y);
            l.Size = new Size(200, 22);
            l.TextAlign = ContentAlignment.MiddleLeft;
            Controls.Add(l);
            return l;
        }

        private Label MakeToggleLabel(string text, int y)
        {
            Label l = new Label();
            l.Text = text;
            l.Font = Theme.Font(9.5f, false);
            l.ForeColor = Theme.Text;
            l.BackColor = Color.Transparent;
            l.Location = new Point(34, y + 1);
            l.Size = new Size(280, 22);
            l.TextAlign = ContentAlignment.MiddleLeft;
            Controls.Add(l);
            return l;
        }

        private ToggleSwitch MakeToggle(int y)
        {
            ToggleSwitch t = new ToggleSwitch();
            t.Location = new Point(324, y);
            t.Size = new Size(42, 22);
            Controls.Add(t);
            return t;
        }

        // ---------- 状态应用 ----------

        public void ApplyState(DeviceState st, bool connected)
        {
            _last = st;
            string oldPill = _pillText;
            Color oldColor = _pillColor;
            string oldInfo = _infoText;
            _updatingUi = true;
            try
            {
                for (int i = 0; i < 4; i++)
                {
                    ChannelState ch = st.Ch[i];
                    _names[i].Text = ch.Name;
                    _sliders[i].Min = ch.Min;
                    _sliders[i].Max = ch.Max;
                    if (!_sliders[i].IsDragging)
                    {
                        _sliders[i].Value = ch.Value;
                        _values[i].Text = FormatValue(_sliders[i]);
                    }
                }

                _meter.SetLevels(st.LevelL, st.LevelR);

                _tglBridge.Checked = st.BridgeEnabled;
                _tglLocal.Checked = st.BridgeLocal;
                _tglSafe.Checked = st.Safe;

                // 状态胶囊 + 提示
                if (!connected)
                {
                    _pillText = "未连接";
                    _pillColor = Theme.Err;
                    _hint.Text = "设备离线：请检查 WiFi 或 IP 设置";
                }
                else if (st.BridgeEnabled)
                {
                    _pillText = st.BridgeClient ? "SigmaStudio 调试中" : "桥接服务已开启";
                    _pillColor = Theme.Purple;
                    _hint.Text = "桥接期间 DSP 由 SigmaStudio 管理，音量不可调";
                }
                else if (st.Safe)
                {
                    _pillText = "安全模式";
                    _pillColor = Theme.Warn;
                    _hint.Text = "安全模式下不写 DSP，音量不可调";
                }
                else if (!st.BridgeLocal)
                {
                    _pillText = "本地控制已关闭";
                    _pillColor = Theme.Warn;
                    _hint.Text = "电平表无数据源，请开启本地控制";
                }
                else if (!st.DspOnline)
                {
                    _pillText = "DSP 离线";
                    _pillColor = Theme.Err;
                    _hint.Text = "ADAU1452 无响应";
                }
                else
                {
                    _pillText = "DSP 在线";
                    _pillColor = Theme.Ok;
                    _hint.Text = "滚轮悬停托盘图标可调总音量";
                }

                // WiFi / IP 信息
                if (connected)
                {
                    string rssi = st.WifiRssi <= -127 ? "—" : st.WifiRssi.ToString("0") + " dBm";
                    string chn = st.WifiCh > 0 ? "CH" + st.WifiCh.ToString("0") : "";
                    string ip = st.WifiIp;
                    _infoText = "WiFi " + rssi + "  " + chn + "  ·  " + ip;
                }
                else
                {
                    _infoText = "";
                }

                bool interactive = connected && !st.BridgeEnabled && !st.Safe;
                for (int i = 0; i < 4; i++)
                {
                    _sliders[i].Enabled = interactive;
                    _sliders[i].Invalidate();
                }
                _tglBridge.Enabled = connected;
                _tglLocal.Enabled = connected;
                _tglSafe.Enabled = connected;
            }
            finally
            {
                _updatingUi = false;
            }
            // 背景（状态胶囊 + WiFi 行）只在内容变化时重绘（已双缓冲，不会闪）
            if (oldPill != _pillText || oldColor.ToArgb() != _pillColor.ToArgb() || oldInfo != _infoText)
            {
                Invalidate();
            }
        }

        private static string FormatValue(VolumeSlider s)
        {
            double v = s.Value;
            if (s.Max > 100.5 && s.Min >= -0.5)   // MASTER：% 通道
            {
                return v.ToString("0") + " %";
            }
            return v.ToString("+0.0;-0.0;0.0") + " dB";
        }

        // ---------- 绘制背景（Win11 卡片 + 状态胶囊） ----------

        /// <summary>Win11 风格卡片：圆角 + 微弱描边。</summary>
        private void DrawCard(Graphics g, float x, float y, float w, float h)
        {
            RectangleF r = new RectangleF(x, y, w, h);
            Theme.FillRounded(g, r, 10f, new SolidBrush(Theme.Card));
            using (Pen p = new Pen(Theme.CardBorder, 1f))
            {
                Theme.DrawRounded(g, r, 10f, p);
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;

            // 三张卡片：输出电平 / 音量调节 / 控制与状态
            DrawCard(g, 16, 78, 368, 118);
            DrawCard(g, 16, 206, 368, 232);
            DrawCard(g, 16, 448, 368, 168);

            // 状态胶囊（标题下方）
            SizeF sz = g.MeasureString(_pillText, Theme.Font(9f, true));
            RectangleF pill = new RectangleF(22, 44, sz.Width + 28f, 26f);
            using (GraphicsPath p = Theme.RoundedRect(pill, 13f))
            using (PathGradientBrush b = new PathGradientBrush(p))
            {
                b.CenterColor = Color.FromArgb(
                    Math.Min(255, _pillColor.R / 3 + 12),
                    Math.Min(255, _pillColor.G / 3 + 12),
                    Math.Min(255, _pillColor.B / 3 + 12));
                b.SurroundColors = new Color[] { Theme.Back };
                g.FillPath(b, p);
            }
            using (Brush tb = new SolidBrush(_pillColor))
            {
                g.DrawString(_pillText, Theme.Font(9f, true), tb, new PointF(pill.X + 14f, pill.Y + 5f));
            }

            // WiFi/IP 信息：控制卡片底部一行
            using (Brush dim = new SolidBrush(Theme.TextDim))
            {
                StringFormat sf = new StringFormat();
                sf.Alignment = StringAlignment.Far;
                sf.LineAlignment = StringAlignment.Center;
                g.DrawString(_infoText, Theme.Font(8.5f, false), dim,
                             new RectangleF(120, 582, Width - 156, 26), sf);
            }
        }

        protected override void OnDeactivate(EventArgs e)
        {
            base.OnDeactivate(e);
            Hide();
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (e.KeyCode == Keys.Escape) Hide();
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            NativeMethods.TrySetRoundCorners(Handle);
        }

        /// <summary>在托盘图标上方展开（图标在溢出区时自动退化到屏幕右下角）。</summary>
        public void ShowNearTray(NativeMethods.RECT iconRect)
        {
            Screen scr = Screen.PrimaryScreen;
            if (iconRect.Left > 0 || iconRect.Top > 0)
            {
                scr = Screen.FromPoint(new Point(iconRect.Left, iconRect.Top));
            }

            int x = iconRect.Left - Width + 40;
            int y = iconRect.Top - Height - 10;

            if (iconRect.Top <= 0 && iconRect.Left <= 0)   // 取不到图标矩形
            {
                x = scr.WorkingArea.Right - Width - 12;
                y = scr.WorkingArea.Bottom - Height - 12;
            }
            else if (y < scr.WorkingArea.Top)              // 任务栏在顶部/侧边
            {
                y = Math.Min(iconRect.Bottom + 10, scr.WorkingArea.Bottom - Height - 12);
            }

            if (x < scr.WorkingArea.Left) x = scr.WorkingArea.Left + 8;
            if (x + Width > scr.WorkingArea.Right) x = scr.WorkingArea.Right - Width - 8;

            Location = new Point(x, y);
            Show();
            Activate();
        }
    }
}
