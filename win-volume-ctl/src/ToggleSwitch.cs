using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace DspVolumeCtl
{
    /// <summary>自绘胶囊开关。</summary>
    internal class ToggleSwitch : Control
    {
        private bool _checked;
        private float _animPos;   // 0 = 关，1 = 开
        private readonly Timer _anim;

        public event EventHandler CheckChanged;

        public ToggleSwitch()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
            Size = new Size(42, 22);
            _anim = new Timer();
            _anim.Interval = 15;
            _anim.Tick += OnAnimTick;
        }

        public bool Checked
        {
            get { return _checked; }
            set
            {
                if (_checked == value) return;
                _checked = value;
                _anim.Start();
                EventHandler h = CheckChanged;
                if (h != null) h(this, EventArgs.Empty);
                Invalidate();
            }
        }

        private void OnAnimTick(object sender, EventArgs e)
        {
            float target = _checked ? 1f : 0f;
            float step = 0.18f;
            if (_animPos < target) _animPos = Math.Min(target, _animPos + step);
            else if (_animPos > target) _animPos = Math.Max(target, _animPos - step);
            else
            {
                _anim.Stop();
            }
            Invalidate();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) _anim.Dispose();
            base.Dispose(disposing);
        }

        protected override void OnClick(EventArgs e)
        {
            base.OnClick(e);
            Checked = !Checked;
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (e.KeyCode == Keys.Space) Checked = !Checked;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Theme.Card);

            float w = Width - 2f, h = Height - 2f;
            RectangleF pill = new RectangleF(1f, 1f, w, h);
            Color on = Theme.Accent;
            Color off = Theme.Track;
            Color bg = Blend(off, on, _animPos);
            Theme.FillRounded(g, pill, h / 2f, new SolidBrush(bg));

            float knobR = h / 2f - 3f;
            float travel = w - 2f * (knobR + 3f);
            float cx = 3f + knobR + travel * _animPos;
            RectangleF knob = new RectangleF(cx - knobR, h / 2f + 1f - knobR, knobR * 2f, knobR * 2f);
            g.FillEllipse(new SolidBrush(Color.White), knob);
        }

        private static Color Blend(Color a, Color b, float t)
        {
            if (t < 0f) t = 0f;
            if (t > 1f) t = 1f;
            return Color.FromArgb(
                (int)(a.R + (b.R - a.R) * t),
                (int)(a.G + (b.G - a.G) * t),
                (int)(a.B + (b.B - a.B) * t));
        }
    }
}
