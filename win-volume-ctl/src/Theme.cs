using System;
using System.Drawing;
using System.Drawing.Drawing2D;

namespace DspVolumeCtl
{
    /// <summary>全局配色（Win11 深色卡片风）。</summary>
    internal static class Theme
    {
        public static readonly Color Back = Color.FromArgb(32, 32, 36);
        public static readonly Color Card = Color.FromArgb(45, 45, 51);
        public static readonly Color CardBorder = Color.FromArgb(36, 255, 255, 255);
        public static readonly Color Panel = Color.FromArgb(58, 58, 66);
        public static readonly Color Track = Color.FromArgb(58, 58, 70);
        public static readonly Color Accent = Color.FromArgb(0, 168, 232);
        public static readonly Color AccentSoft = Color.FromArgb(70, 190, 235);
        public static readonly Color Text = Color.FromArgb(235, 235, 242);
        public static readonly Color TextDim = Color.FromArgb(150, 152, 164);
        public static readonly Color Ok = Color.FromArgb(86, 200, 126);
        public static readonly Color Warn = Color.FromArgb(240, 176, 66);
        public static readonly Color Err = Color.FromArgb(240, 96, 96);
        public static readonly Color Purple = Color.FromArgb(120, 130, 235);

        /// <summary>电平表渐变（绿 → 黄 → 橙 → 红），与屏幕同款分级。</summary>
        public static Color[] LevelColors
        {
            get
            {
                return new Color[]
                {
                    Color.FromArgb(70, 205, 110),
                    Color.FromArgb(222, 210, 70),
                    Color.FromArgb(240, 150, 50),
                    Color.FromArgb(235, 70, 70),
                };
            }
        }

        /// <summary>按 dB 给出指示色：绿(&lt;-36) → 黄(-36~-18) → 橙(-18~-6) → 红(&gt;-6)。</summary>
        public static Color LevelColor(double db)
        {
            if (db < -36.0) return Color.FromArgb(70, 205, 110);
            if (db < -18.0) return Color.FromArgb(222, 210, 70);
            if (db < -6.0) return Color.FromArgb(240, 150, 50);
            return Color.FromArgb(235, 70, 70);
        }

        public static GraphicsPath RoundedRect(RectangleF r, float radius)
        {
            float d = radius * 2f;
            GraphicsPath p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static void FillRounded(Graphics g, RectangleF r, float radius, Brush brush)
        {
            using (GraphicsPath p = RoundedRect(r, radius))
            {
                g.FillPath(brush, p);
            }
        }

        public static void DrawRounded(Graphics g, RectangleF r, float radius, Pen pen)
        {
            using (GraphicsPath p = RoundedRect(r, radius))
            {
                g.DrawPath(pen, p);
            }
        }

        /// <summary>标准正文字体（中文用雅黑）。</summary>
        public static Font Font(float size, bool bold)
        {
            return new Font("Microsoft YaHei UI", size, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Point);
        }
    }
}
