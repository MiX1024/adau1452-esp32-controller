using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace DspVolumeCtl
{
    /// <summary>
    /// L/R 双声道电平表：渐变能量条（绿→黄→橙→红，按条内位置过渡，与屏幕同款）
        /// + 白色峰值保持线（约 4~6 dB/s 慢落）+ dB 读数。
        /// 60fps 指数时间常数插值，衔接轮询间隙（对齐网页版手感）。
        /// </summary>
        internal class LevelMeter : Control
        {
            private double _tgtL = -999.0, _tgtR = -999.0;
            private double _dispL = -999.0, _dispR = -999.0;
            private double _peakL = -999.0, _peakR = -999.0;
            private readonly Timer _anim;
            private readonly Font _labelFont = Theme.Font(9f, true);
            private readonly Font _valFont = Theme.Font(8.5f, false);
            private DateTime _lastTick = DateTime.Now;

            public LevelMeter()
            {
                SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                         ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
                _anim = new Timer();
                _anim.Interval = 16;                 // ≈60fps
                _anim.Tick += OnAnimTick;
                _anim.Start();
            }

            /// <summary>面板隐藏时停掉动画，省得空转重绘。</summary>
            protected override void OnVisibleChanged(EventArgs e)
            {
                base.OnVisibleChanged(e);
                if (Visible) _anim.Start();
                else _anim.Stop();
            }

        public void SetLevels(double lDb, double rDb)
        {
            _tgtL = lDb;
            _tgtR = rDb;
        }

        /// <summary>电平 dB → 条长度比例：frac = 10^(db/60)（-60dB 落在 10%，与固件一致）。</summary>
        private static double DbToFrac(double db)
        {
            if (db <= -900.0) return 0.0;
            if (db > 0.0) db = 0.0;
            if (db < -60.0) return 0.0;
            double f = Math.Pow(10.0, db / 60.0);
            return f > 1.0 ? 1.0 : f;
        }

        private static double Smooth(double disp, double target, double dt)
        {
            if (target <= -900.0) return disp <= -900.0 ? -999.0 : disp - 24.0 * dt;  // 信号消失时缓缓归零
            if (disp <= -900.0) disp = -60.0;
            // 指数时间常数插值：上升快（55ms）/ 下降慢（130ms），VU 弹道手感，与网页一致
            double tau = target > disp ? 0.055 : 0.13;
            double k = 1.0 - Math.Exp(-dt / tau);
            return disp + (target - disp) * k;
        }

        private void OnAnimTick(object sender, EventArgs e)
        {
            DateTime now = DateTime.Now;
            double dt = (now - _lastTick).TotalSeconds;
            if (dt <= 0 || dt > 0.5) dt = 0.016;
            _lastTick = now;

            _dispL = Smooth(_dispL, _tgtL, dt);
            _dispR = Smooth(_dispR, _tgtR, dt);

            // 峰值保持：跟上瞬时峰，然后极慢滑落
            if (_dispL > _peakL) _peakL = _dispL; else if (_peakL > -900.0) _peakL -= 6.0 * dt;
            if (_dispR > _peakR) _peakR = _dispR; else if (_peakR > -900.0) _peakR -= 6.0 * dt;
            if (_peakL < -60.0) _peakL = -999.0;
            if (_peakR < -60.0) _peakR = -999.0;

            Invalidate();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _anim.Dispose();
                _labelFont.Dispose();
                _valFont.Dispose();
            }
            base.Dispose(disposing);
        }

        private void DrawBar(Graphics g, RectangleF bar, double disp, double peak)
        {
            Theme.FillRounded(g, bar, 4f, new SolidBrush(Theme.Track));

            double frac = DbToFrac(disp);
            if (frac > 0.01)
            {
                RectangleF fill = new RectangleF(bar.X, bar.Y, (float)(bar.Width * frac), bar.Height);
                Color[] colors = Theme.LevelColors;
                using (LinearGradientBrush b = new LinearGradientBrush(
                    new PointF(bar.Left, 0), new PointF(bar.Right, 0),
                    colors[0], colors[3]))
                {
                    ColorBlend blend = new ColorBlend(4);
                    blend.Colors = colors;
                    blend.Positions = new float[] { 0f, 0.38f, 0.72f, 1f };
                    b.InterpolationColors = blend;
                    Theme.FillRounded(g, fill, 4f, b);
                }
            }

            double pfrac = DbToFrac(peak);
            if (pfrac > 0.01)
            {
                float px = bar.X + (float)(bar.Width * pfrac) - 1.5f;
                using (Pen p = new Pen(Color.FromArgb(235, 255, 255, 255), 3f))
                {
                    g.DrawLine(p, px, bar.Y - 1f, px, bar.Bottom + 1f);
                }
            }
        }

        /// <summary>dB 读数浮在能量条内右端（半透明底），条本身可以占满整个横向空间。</summary>
        private void DrawValueOverlay(Graphics g, RectangleF bar, string txt, Brush brush)
        {
            SizeF ms = g.MeasureString(txt, _valFont);
            float w = ms.Width + 16f;
            float h = ms.Height + 3f;
            RectangleF chip = new RectangleF(bar.Right - w - 3f, bar.Y + (bar.Height - h) / 2f, w, h);
            Theme.FillRounded(g, chip, 5f, new SolidBrush(Color.FromArgb(165, 18, 18, 24)));
            StringFormat sf = new StringFormat();
            sf.Alignment = StringAlignment.Center;
            sf.LineAlignment = StringAlignment.Center;
            g.DrawString(txt, _valFont, brush, chip, sf);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Theme.Card);

            // 只留 22px 给 L/R 标签，能量条占满其余全部宽度
            float labelW = 22f;
            float barX = labelW;
            float barW = Width - labelW - 2f;
            if (barW < 40f) barW = 40f;

            float barH = 18f;
            float y1 = 6f;
            float y2 = Height - barH - 6f;

            RectangleF barL = new RectangleF(barX, y1, barW, barH);
            RectangleF barR = new RectangleF(barX, y2, barW, barH);

            using (Brush tb = new SolidBrush(Theme.Text))
            using (Brush dim = new SolidBrush(Theme.TextDim))
            {
                StringFormat sf = new StringFormat();
                sf.LineAlignment = StringAlignment.Center;

                g.DrawString("L", _labelFont, tb, new RectangleF(0, y1, labelW, barH), sf);
                g.DrawString("R", _labelFont, tb, new RectangleF(0, y2, labelW, barH), sf);

                DrawBar(g, barL, _dispL, _peakL);
                DrawBar(g, barR, _dispR, _peakR);

                string lTxt = _dispL <= -900.0 ? "—" : _dispL.ToString("0.0") + " dB";
                string rTxt = _dispR <= -900.0 ? "—" : _dispR.ToString("0.0") + " dB";
                using (Brush lb = _dispL <= -900.0 ? (Brush)new SolidBrush(Theme.TextDim)
                                                   : new SolidBrush(Theme.LevelColor(_dispL)))
                using (Brush rb = _dispR <= -900.0 ? (Brush)new SolidBrush(Theme.TextDim)
                                                   : new SolidBrush(Theme.LevelColor(_dispR)))
                {
                    DrawValueOverlay(g, barL, lTxt, lb);
                    DrawValueOverlay(g, barR, rTxt, rb);
                }
            }
        }
    }
}
