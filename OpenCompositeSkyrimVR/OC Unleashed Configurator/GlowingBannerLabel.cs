using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    /// <summary>
    /// Soft green breathing warning used for unsaved settings. The glow is
    /// restrained so it draws attention without reading as an error.
    /// </summary>
    internal sealed class GlowingBannerLabel : Label
    {
        private float _glowIntensity = 0.45f;
        private float _shinePosition;

        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal float GlowIntensity
        {
            get => _glowIntensity;
            set
            {
                float clamped = Math.Clamp(value, 0f, 1f);
                if (Math.Abs(_glowIntensity - clamped) < 0.001f)
                    return;

                _glowIntensity = clamped;
                Invalidate();
            }
        }

        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal float ShinePosition
        {
            get => _shinePosition;
            set
            {
                float wrapped = value - MathF.Floor(value);
                if (Math.Abs(_shinePosition - wrapped) < 0.001f)
                    return;

                _shinePosition = wrapped;
                Invalidate();
            }
        }

        internal GlowingBannerLabel()
        {
            SetStyle(ControlStyles.UserPaint
                | ControlStyles.AllPaintingInWmPaint
                | ControlStyles.OptimizedDoubleBuffer
                | ControlStyles.ResizeRedraw
                | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
        }

        internal void ApplyTheme()
        {
            BackColor = Color.Transparent;
            Invalidate();
        }

        protected override void OnPaintBackground(PaintEventArgs pevent)
        {
            // Painted with the foreground in one buffered pass.
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics graphics = e.Graphics;
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            graphics.Clear(Parent?.BackColor ?? ModernUiTheme.Window);

            RectangleF panelBounds = new(5f, 3f, Math.Max(1f, Width - 11f), Math.Max(1f, Height - 7f));
            using GraphicsPath panelPath = CreateRoundedRectangle(panelBounds, 11f);

            int panelAlpha = (int)(7 + 13 * _glowIntensity);
            int borderAlpha = (int)(18 + 32 * _glowIntensity);
            using (var panelBrush = new SolidBrush(Color.FromArgb(panelAlpha, 42, 156, 67)))
                graphics.FillPath(panelBrush, panelPath);
            using (var panelPen = new Pen(Color.FromArgb(borderAlpha, 91, 220, 116), 1f))
                graphics.DrawPath(panelPen, panelPath);

            using StringFormat format = new()
            {
                Alignment = StringAlignment.Center,
                LineAlignment = StringAlignment.Center,
                Trimming = StringTrimming.EllipsisCharacter
            };

            RectangleF textBounds = new(6f, 0f, Math.Max(1f, Width - 13f), Height);
            int glowAlpha = (int)(18 + 46 * _glowIntensity);
            using (var glowBrush = new SolidBrush(Color.FromArgb(glowAlpha, 72, 238, 104)))
            {
                for (int radius = 3; radius >= 1; radius--)
                {
                    float offset = radius * 0.7f;
                    graphics.DrawString(Text, Font, glowBrush,
                        new RectangleF(textBounds.X - offset, textBounds.Y, textBounds.Width, textBounds.Height), format);
                    graphics.DrawString(Text, Font, glowBrush,
                        new RectangleF(textBounds.X + offset, textBounds.Y, textBounds.Width, textBounds.Height), format);
                    graphics.DrawString(Text, Font, glowBrush,
                        new RectangleF(textBounds.X, textBounds.Y - offset, textBounds.Width, textBounds.Height), format);
                    graphics.DrawString(Text, Font, glowBrush,
                        new RectangleF(textBounds.X, textBounds.Y + offset, textBounds.Width, textBounds.Height), format);
                }
            }

            Color dim = Color.FromArgb(78, 184, 98);
            Color bright = Color.FromArgb(139, 242, 156);
            using SolidBrush textBrush = new(Lerp(dim, bright, _glowIntensity));
            graphics.DrawString(Text, Font, textBrush, textBounds, format);

            // A narrow diagonal specular sweep, like light running along a
            // polished sword. Redrawing the text through the clipped bands
            // makes the shine travel through the lettering instead of merely
            // floating over the banner background.
            GraphicsState savedState = graphics.Save();
            graphics.SetClip(panelPath);

            float travelPadding = 90f;
            float centerX = -travelPadding + _shinePosition * (Width + travelPadding * 2f);
            DrawShineBand(graphics, centerX, 58f, Color.FromArgb(16, 102, 255, 135));
            DrawShineBand(graphics, centerX, 25f, Color.FromArgb(42, 160, 255, 184));
            DrawShineBand(graphics, centerX, 5f, Color.FromArgb(150, 226, 255, 232));

            using GraphicsPath textShineClip = CreateShineBand(centerX, 31f, Height);
            graphics.SetClip(textShineClip, CombineMode.Intersect);
            using (var shineTextBrush = new SolidBrush(Color.FromArgb(235, 224, 255, 231)))
                graphics.DrawString(Text, Font, shineTextBrush, textBounds, format);

            graphics.Restore(savedState);
        }

        private void DrawShineBand(Graphics graphics, float centerX, float width, Color color)
        {
            using GraphicsPath band = CreateShineBand(centerX, width, Height);
            using SolidBrush brush = new(color);
            graphics.FillPath(brush, band);
        }

        private static GraphicsPath CreateShineBand(float centerX, float width, float height)
        {
            float half = width * 0.5f;
            float slant = Math.Max(10f, height * 0.72f);
            GraphicsPath path = new();
            path.AddPolygon(new[]
            {
                new PointF(centerX + slant - half, -2f),
                new PointF(centerX + slant + half, -2f),
                new PointF(centerX - slant + half, height + 2f),
                new PointF(centerX - slant - half, height + 2f)
            });
            return path;
        }

        private static GraphicsPath CreateRoundedRectangle(RectangleF bounds, float radius)
        {
            float diameter = Math.Min(radius * 2f, Math.Min(bounds.Width, bounds.Height));
            GraphicsPath path = new();
            path.AddArc(bounds.Left, bounds.Top, diameter, diameter, 180f, 90f);
            path.AddArc(bounds.Right - diameter, bounds.Top, diameter, diameter, 270f, 90f);
            path.AddArc(bounds.Right - diameter, bounds.Bottom - diameter, diameter, diameter, 0f, 90f);
            path.AddArc(bounds.Left, bounds.Bottom - diameter, diameter, diameter, 90f, 90f);
            path.CloseFigure();
            return path;
        }

        private static Color Lerp(Color from, Color to, float amount)
        {
            return Color.FromArgb(
                (int)(from.R + (to.R - from.R) * amount),
                (int)(from.G + (to.G - from.G) * amount),
                (int)(from.B + (to.B - from.B) * amount));
        }
    }
}
