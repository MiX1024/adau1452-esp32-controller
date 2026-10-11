using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace DspVolumeCtl
{
    /// <summary>自绘音量滑块：圆角轨道 + 渐变填充 + 圆形滑块，支持拖动/滚轮/方向键。</summary>
    internal class VolumeSlider : Control
    {
        private double _min = 0.0;
        private double _max = 100.0;
        private double _value = 50.0;
        private bool _dragging;
        private const float Pad = 10f;

        public event EventHandler ValueChanged;

        public VolumeSlider()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
            Height = 28;
            TabStop = true;
        }

        public bool IsDragging
        {
            get { return _dragging; }
        }

        public double Min
        {
            get { return _min; }
            set { _min = value; Invalidate(); }
        }

        public double Max
        {
            get { return _max; }
            set { _max = value; Invalidate(); }
        }

        public double Value
        {
            get { return _value; }
            set
            {
                double v = Clamp(value);
                if (Math.Abs(v - _value) < 0.0001) return;
                _value = v;
                Invalidate();
                EventHandler h = ValueChanged;
                if (h != null) h(this, EventArgs.Empty);
            }
        }

        private double Clamp(double v)
        {
            if (v < _min) return _min;
            if (v > _max) return _max;
            return v;
        }

        private double Frac
        {
            get
            {
                double span = _max - _min;
                return span <= 0 ? 0.0 : (_value - _min) / span;
            }
        }

        private void ValueFromMouse(int x)
        {
            float usable = Width - 2f * Pad;
            if (usable <= 0) return;
            double f = (x - Pad) / usable;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            Value = _min + f * (_max - _min);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            Focus();
            if (e.Button == MouseButtons.Left)
            {
                _dragging = true;
                Capture = true;
                ValueFromMouse(e.X);
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (_dragging) ValueFromMouse(e.X);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (e.Button == MouseButtons.Left)
            {
                _dragging = false;
                Capture = false;
            }
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e);
            double step = (_max - _min) / 60.0;
            Value = _value + Math.Sign(e.Delta) * step;
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            double step = (_max - _min) / 60.0;
            if (e.KeyCode == Keys.Left) Value = _value - step;
            else if (e.KeyCode == Keys.Right) Value = _value + step;
            else if (e.KeyCode == Keys.Home) Value = _min;
            else if (e.KeyCode == Keys.End) Value = _max;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Theme.Card);

            float trackY = Height / 2f - 4f;
            RectangleF track = new RectangleF(Pad, trackY, Width - 2f * Pad, 8f);
            bool en = Enabled;
            Theme.FillRounded(g, track, 4f, new SolidBrush(en ? Theme.Track : Color.FromArgb(56, 56, 64)));

            float fillW = (float)(track.Width * Frac);
            if (fillW > 2f)
            {
                RectangleF fill = new RectangleF(track.X, track.Y, fillW, track.Height);
                if (en)
                {
                    using (LinearGradientBrush b = new LinearGradientBrush(
                        new PointF(fill.Left, 0), new PointF(fill.Right, 0),
                        Theme.Accent, Theme.AccentSoft))
                    {
                        Theme.FillRounded(g, fill, 4f, b);
                    }
                }
                else
                {
                    Theme.FillRounded(g, fill, 4f, new SolidBrush(Color.FromArgb(80, 82, 94)));
                }
            }

            float cx = track.X + fillW;
            float cy = Height / 2f;
            RectangleF knob = new RectangleF(cx - 10f, cy - 10f, 20f, 20f);
            using (GraphicsPath shadow = Theme.RoundedRect(new RectangleF(knob.X, knob.Y + 2f, knob.Width, knob.Height), 10f))
            {
                g.FillPath(new SolidBrush(Color.FromArgb(60, 0, 0, 0)), shadow);
            }
            Color knobColor = !en ? Color.FromArgb(110, 112, 122)
                           : (Focused || _dragging) ? Theme.AccentSoft : Color.White;
            g.FillEllipse(new SolidBrush(knobColor), knob);
            if (Focused || _dragging)
            {
                using (Pen p = new Pen(Color.FromArgb(90, 255, 255, 255), 3f))
                {
                    g.DrawEllipse(p, knob);
                }
            }
        }
    }
}
