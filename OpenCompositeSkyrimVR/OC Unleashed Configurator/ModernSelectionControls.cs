using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    internal sealed class ModernCheckBox : CheckBox
    {
        internal float LayoutScale = 1f;
        private bool _hovered;
        private float _glowIntensity = 0.65f;

        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal float GlowIntensity
        {
            get => _glowIntensity;
            set
            {
                float clamped = Math.Clamp(value, 0f, 1f);
                if (Math.Abs(_glowIntensity - clamped) < 0.005f)
                    return;

                _glowIntensity = clamped;
                if (Checked)
                    Invalidate();
            }
        }

        internal ModernCheckBox()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint
                | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw
                | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
        }

        public override Size GetPreferredSize(Size proposedSize)
        {
            Size text = TextRenderer.MeasureText(Text, Font, Size.Empty,
                TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
            // WinForms' stock CheckBox reserves more glyph/padding space than
            // its reported text width suggests. Keep a little breathing room so
            // AutoSize labels never end in an unnecessary ellipsis.
            return new Size(text.Width + (int)Math.Ceiling(45 * LayoutScale),
                Math.Max((int)Math.Ceiling(20 * LayoutScale), text.Height + (int)Math.Ceiling(3 * LayoutScale)));
        }

        protected override void OnMouseEnter(EventArgs e)
        {
            _hovered = true;
            Invalidate();
            base.OnMouseEnter(e);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            _hovered = false;
            Invalidate();
            base.OnMouseLeave(e);
        }

        protected override void OnCheckedChanged(EventArgs e)
        {
            Invalidate();
            base.OnCheckedChanged(e);
        }

        protected override void OnEnabledChanged(EventArgs e)
        {
            Invalidate();
            base.OnEnabledChanged(e);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            e.Graphics.Clear(ResolveBackground());
            var drawingState = e.Graphics.Save();
            e.Graphics.ScaleTransform(LayoutScale, LayoutScale);

            const int box = 15;
            int top = Math.Max(1, ((int)(Height / LayoutScale) - box) / 2);
            var rect = new Rectangle(4, top, box, box);
            float pulse = Checked
                ? Math.Min(1f, _glowIntensity + (_hovered ? 0.12f : 0f))
                : 0f;
            Color fillTop = Checked
                ? Blend(
                    Color.FromArgb(24, 54, 50),
                    _hovered ? Color.FromArgb(38, 92, 74) : Color.FromArgb(31, 78, 63),
                    pulse)
                : _hovered ? ModernUiTheme.SurfaceHover : ModernUiTheme.Input;
            Color fillBottom = Checked
                ? Color.FromArgb(16, 31, 32)
                : fillTop;
            Color border = Checked
                ? Blend(ModernUiTheme.KeyGlow, ModernUiTheme.KeyGlowBright, pulse)
                : ModernUiTheme.Border;
            if (!Enabled)
            {
                fillTop = ModernUiTheme.SurfaceRaised;
                fillBottom = ModernUiTheme.Surface;
                border = ModernUiTheme.BorderSoft;
            }

            using (GraphicsPath path = RoundedRectangle(rect, 4))
            {
                if (Checked && Enabled)
                {
                    using var glow = new Pen(Color.FromArgb(
                        (int)(22 + 52 * pulse), border), 4f);
                    e.Graphics.DrawPath(glow, path);
                }

                using var brush = new LinearGradientBrush(rect, fillTop, fillBottom, 90f);
                using var pen = new Pen(border, 1.2f);
                e.Graphics.FillPath(brush, path);
                e.Graphics.DrawPath(pen, path);
            }

            if (Checked)
            {
                PointF[] checkPoints =
                {
                    new PointF(7.5f, top + 7.5f),
                    new PointF(10.2f, top + 10.3f),
                    new PointF(15.8f, top + 4.5f)
                };

                if (Enabled)
                {
                    using var checkGlow = new Pen(Color.FromArgb(
                        (int)(35 + 95 * pulse), ModernUiTheme.KeyGlowBright), 4f)
                    {
                        StartCap = LineCap.Round,
                        EndCap = LineCap.Round
                    };
                    e.Graphics.DrawLines(checkGlow, checkPoints);
                }

                Color checkColor = Enabled
                    ? Blend(Color.FromArgb(205, 234, 219), Color.White, pulse)
                    : ModernUiTheme.TextMuted;
                using var check = new Pen(checkColor, 2f)
                {
                    StartCap = LineCap.Round,
                    EndCap = LineCap.Round
                };
                e.Graphics.DrawLines(check, checkPoints);
            }

            e.Graphics.Restore(drawingState);
            Color textColor = Enabled ? ForeColor : ModernUiTheme.TextMuted;
            int textStart = (int)Math.Round(26 * LayoutScale);
            var textRect = new Rectangle(textStart, 0, Math.Max(0, Width - textStart), Height);
            TextRenderer.DrawText(e.Graphics, Text, Font, textRect, textColor,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter
                | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);

            if (Focused && ShowFocusCues)
            {
                Rectangle focus = textRect;
                focus.Inflate(-1, -3);
                ControlPaint.DrawFocusRectangle(e.Graphics, focus, textColor, ResolveBackground());
            }
        }

        private Color ResolveBackground()
        {
            if (BackColor != Color.Transparent && BackColor.A != 0)
                return BackColor;
            return Parent?.BackColor ?? ModernUiTheme.Window;
        }

        private static GraphicsPath RoundedRectangle(Rectangle bounds, int radius)
        {
            int d = radius * 2;
            var path = new GraphicsPath();
            path.AddArc(bounds.Left, bounds.Top, d, d, 180, 90);
            path.AddArc(bounds.Right - d, bounds.Top, d, d, 270, 90);
            path.AddArc(bounds.Right - d, bounds.Bottom - d, d, d, 0, 90);
            path.AddArc(bounds.Left, bounds.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            return path;
        }

        private static Color Blend(Color from, Color to, float amount)
        {
            amount = Math.Clamp(amount, 0f, 1f);
            return Color.FromArgb(
                (int)(from.A + (to.A - from.A) * amount),
                (int)(from.R + (to.R - from.R) * amount),
                (int)(from.G + (to.G - from.G) * amount),
                (int)(from.B + (to.B - from.B) * amount));
        }
    }

    internal sealed class ModernRadioButton : RadioButton
    {
        internal float LayoutScale = 1f;
        private bool _hovered;

        internal ModernRadioButton()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint
                | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw
                | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
        }

        public override Size GetPreferredSize(Size proposedSize)
        {
            Size text = TextRenderer.MeasureText(Text, Font, Size.Empty,
                TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
            return new Size(text.Width + (int)Math.Ceiling(42 * LayoutScale),
                Math.Max((int)Math.Ceiling(20 * LayoutScale), text.Height + (int)Math.Ceiling(3 * LayoutScale)));
        }

        protected override void OnMouseEnter(EventArgs e)
        {
            _hovered = true;
            Invalidate();
            base.OnMouseEnter(e);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            _hovered = false;
            Invalidate();
            base.OnMouseLeave(e);
        }

        protected override void OnCheckedChanged(EventArgs e)
        {
            Invalidate();
            base.OnCheckedChanged(e);
        }

        protected override void OnEnabledChanged(EventArgs e)
        {
            Invalidate();
            base.OnEnabledChanged(e);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            Color background = BackColor != Color.Transparent && BackColor.A != 0
                ? BackColor
                : Parent?.BackColor ?? ModernUiTheme.Window;
            e.Graphics.Clear(background);
            var drawingState = e.Graphics.Save();
            e.Graphics.ScaleTransform(LayoutScale, LayoutScale);

            const int diameter = 15;
            int top = Math.Max(1, ((int)(Height / LayoutScale) - diameter) / 2);
            var rect = new Rectangle(1, top, diameter, diameter);
            Color ring = Checked ? ModernUiTheme.AccentHover : ModernUiTheme.Border;
            Color fill = _hovered ? ModernUiTheme.SurfaceHover : ModernUiTheme.Input;
            if (!Enabled)
            {
                ring = ModernUiTheme.BorderSoft;
                fill = ModernUiTheme.SurfaceRaised;
            }

            using (var brush = new SolidBrush(fill))
            using (var pen = new Pen(ring, 1.3f))
            {
                e.Graphics.FillEllipse(brush, rect);
                e.Graphics.DrawEllipse(pen, rect);
            }
            if (Checked)
            {
                var dot = new Rectangle(rect.X + 4, rect.Y + 4, rect.Width - 8, rect.Height - 8);
                using var dotBrush = new SolidBrush(Enabled ? ModernUiTheme.AccentText : ModernUiTheme.TextMuted);
                e.Graphics.FillEllipse(dotBrush, dot);
            }

            e.Graphics.Restore(drawingState);
            Color textColor = Enabled ? ForeColor : ModernUiTheme.TextMuted;
            int textStart = (int)Math.Round(23 * LayoutScale);
            var textRect = new Rectangle(textStart, 0, Math.Max(0, Width - textStart), Height);
            TextRenderer.DrawText(e.Graphics, Text, Font, textRect, textColor,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter
                | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
        }
    }
}
