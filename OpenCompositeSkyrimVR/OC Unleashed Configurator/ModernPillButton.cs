using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    internal enum ModernButtonRole
    {
        Neutral,
        Positive,
        Destructive,
        Warning
    }

    /// <summary>
    /// Anti-aliased owner-drawn button. Stock WinForms buttons clipped with a
    /// Region produce stair-stepped pill edges; painting the curve directly
    /// keeps every size smooth and gives active buttons the keyboard-key look.
    /// </summary>
    internal class ModernPillButton : Button
    {
        private bool _hovered;
        private bool _pressed;
        private bool _brightGreen;
        private float _shinePosition = -1f;
        private ModernButtonRole _visualRole;

        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal bool BrightGreen
        {
            get => _brightGreen;
            set
            {
                if (_brightGreen == value)
                    return;
                _brightGreen = value;
                Invalidate();
            }
        }

        // Negative disables the sweep. Values from zero through one move the
        // diagonal sword-shine completely across the pill and its text.
        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal float ShinePosition
        {
            get => _shinePosition;
            set
            {
                float clamped = value < 0f ? -1f : Math.Clamp(value, 0f, 1f);
                if (Math.Abs(_shinePosition - clamped) < 0.001f)
                    return;
                _shinePosition = clamped;
                Invalidate();
            }
        }

        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal ModernButtonRole VisualRole
        {
            get => _visualRole;
            set
            {
                if (_visualRole == value)
                    return;
                _visualRole = value;
                Invalidate();
            }
        }

        internal ModernPillButton()
        {
            SetStyle(ControlStyles.UserPaint
                | ControlStyles.AllPaintingInWmPaint
                | ControlStyles.OptimizedDoubleBuffer
                | ControlStyles.ResizeRedraw
                | ControlStyles.SupportsTransparentBackColor, true);
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            UseVisualStyleBackColor = false;
            Cursor = Cursors.Hand;
            TabStop = true;
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
            _pressed = false;
            Invalidate();
            base.OnMouseLeave(e);
        }

        protected override void OnMouseDown(MouseEventArgs mevent)
        {
            _pressed = true;
            Invalidate();
            base.OnMouseDown(mevent);
        }

        protected override void OnMouseUp(MouseEventArgs mevent)
        {
            _pressed = false;
            Invalidate();
            base.OnMouseUp(mevent);
        }

        protected override void OnMouseCaptureChanged(EventArgs e)
        {
            _pressed = false;
            Invalidate();
            base.OnMouseCaptureChanged(e);
        }

        protected override void OnEnabledChanged(EventArgs e)
        {
            Invalidate();
            base.OnEnabledChanged(e);
        }

        protected override void OnPaintBackground(PaintEventArgs pevent)
        {
            // The full control is painted in one buffered pass below.
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics graphics = e.Graphics;
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.Clear(Parent?.BackColor ?? ModernUiTheme.Surface);

            RectangleF bounds = new(2.5f, 2.5f,
                Math.Max(1f, Width - 5.5f), Math.Max(1f, Height - 5.5f));
            float radius = Math.Max(3f, bounds.Height / 2f);
            using GraphicsPath path = RoundedRectangle(bounds, radius);

            Color top;
            Color bottom;
            Color border;
            Color text;
            int glowAlpha = 0;

            if (!Enabled)
            {
                top = ModernUiTheme.SurfaceRaised;
                bottom = ModernUiTheme.Surface;
                border = ModernUiTheme.BorderSoft;
                text = ModernUiTheme.TextMuted;
            }
            else
            {
                if (_brightGreen)
                {
                    top = _pressed ? ModernUiTheme.AccentPressed
                        : _hovered ? ModernUiTheme.AccentHover
                        : ModernUiTheme.Accent;
                    bottom = ModernUiTheme.AccentSoft;
                    border = _hovered ? ModernUiTheme.KeyGlowBright : ModernUiTheme.KeyGlow;
                    text = Color.FromArgb(240, 255, 244);
                    glowAlpha = _hovered ? 70 : 48;
                }
                else switch (_visualRole)
                {
                    case ModernButtonRole.Positive:
                        top = _pressed ? Color.FromArgb(33, 82, 66)
                            : _hovered ? Color.FromArgb(38, 92, 74)
                            : Color.FromArgb(27, 65, 57);
                        bottom = Color.FromArgb(16, 31, 32);
                        border = _hovered ? ModernUiTheme.KeyGlowHover : ModernUiTheme.KeyGlow;
                        text = Color.FromArgb(228, 255, 241);
                        glowAlpha = _hovered ? 62 : 42;
                        break;

                    case ModernButtonRole.Destructive:
                        top = _pressed ? Color.FromArgb(82, 31, 35)
                            : _hovered ? Color.FromArgb(139, 54, 59)
                            : Color.FromArgb(105, 40, 45);
                        bottom = Color.FromArgb(47, 24, 29);
                        border = _hovered ? Color.FromArgb(224, 103, 108)
                            : Color.FromArgb(184, 75, 82);
                        text = Color.FromArgb(255, 235, 236);
                        glowAlpha = _hovered ? 45 : 25;
                        break;

                    case ModernButtonRole.Warning:
                        top = _pressed ? Color.FromArgb(91, 59, 24)
                            : _hovered ? Color.FromArgb(137, 89, 35)
                            : Color.FromArgb(112, 73, 30);
                        bottom = Color.FromArgb(55, 39, 25);
                        border = ModernUiTheme.Warning;
                        text = ModernUiTheme.TextPrimary;
                        glowAlpha = _hovered ? 40 : 20;
                        break;

                    default:
                        top = _pressed ? Color.FromArgb(18, 22, 28)
                            : _hovered ? ModernUiTheme.SurfaceHover
                            : ModernUiTheme.SurfaceRaised;
                        bottom = ModernUiTheme.Surface;
                        border = ModernUiTheme.Border;
                        text = _hovered ? ModernUiTheme.TextPrimary : ModernUiTheme.TextSecondary;
                        break;
                }
            }

            if (glowAlpha > 0)
            {
                using var glow = new Pen(Color.FromArgb(glowAlpha, border), 4f);
                graphics.DrawPath(glow, path);
            }

            using (var fill = new LinearGradientBrush(bounds, top, bottom, 90f))
                graphics.FillPath(fill, path);

            if (_visualRole == ModernButtonRole.Positive && Enabled)
            {
                GraphicsState clip = graphics.Save();
                graphics.SetClip(path);
                RectangleF hazeBounds = new(
                    bounds.Left + bounds.Width * 0.12f,
                    bounds.Top - bounds.Height * 0.50f,
                    bounds.Width * 0.76f,
                    bounds.Height * 1.45f);
                using GraphicsPath hazePath = new();
                hazePath.AddEllipse(hazeBounds);
                using PathGradientBrush haze = new(hazePath)
                {
                    CenterColor = Color.FromArgb(_hovered ? 115 : 78, ModernUiTheme.KeyGlowFill),
                    SurroundColors = new[] { Color.Transparent }
                };
                graphics.FillEllipse(haze, hazeBounds);
                graphics.Restore(clip);
            }

            if (Enabled && _shinePosition >= 0f)
            {
                GraphicsState shineState = graphics.Save();
                graphics.SetClip(path);
                float travelPadding = 70f;
                float centerX = -travelPadding
                    + _shinePosition * (Width + travelPadding * 2f);
                DrawShineBand(graphics, centerX, 42f, Color.FromArgb(18, 120, 255, 166));
                DrawShineBand(graphics, centerX, 18f, Color.FromArgb(58, 194, 255, 213));
                DrawShineBand(graphics, centerX, 4f, Color.FromArgb(188, 235, 255, 240));
                graphics.Restore(shineState);
            }

            using (var outline = new Pen(border, Enabled ? 1.35f : 1f))
                graphics.DrawPath(outline, path);

            Rectangle textBounds = Rectangle.Inflate(ClientRectangle, -9, -2);
            DrawContent(graphics, textBounds, text);

            if (Enabled && _shinePosition >= 0f)
            {
                float travelPadding = 70f;
                float centerX = -travelPadding
                    + _shinePosition * (Width + travelPadding * 2f);
                GraphicsState textShineState = graphics.Save();
                graphics.SetClip(path);
                using GraphicsPath textBand = CreateShineBand(centerX, 22f, Height);
                graphics.SetClip(textBand, CombineMode.Intersect);
                DrawContent(graphics, textBounds, Color.FromArgb(238, 255, 242));
                graphics.Restore(textShineState);
            }

            if (Focused && ShowFocusCues)
            {
                RectangleF focusBounds = RectangleF.Inflate(bounds, -2.5f, -2.5f);
                using GraphicsPath focusPath = RoundedRectangle(
                    focusBounds, Math.Max(2f, focusBounds.Height / 2f));
                using var focusPen = new Pen(Color.FromArgb(95, text), 1f);
                graphics.DrawPath(focusPen, focusPath);
            }
        }

        // Specialized buttons can add an icon while retaining the shared pill,
        // hover/pressed/focus treatment, and clipped shine pass.
        protected virtual void DrawContent(Graphics graphics, Rectangle bounds, Color color)
        {
            TextRenderer.DrawText(graphics, Text, Font, bounds, color,
                TextFormatFlags.HorizontalCenter
                | TextFormatFlags.VerticalCenter
                | TextFormatFlags.SingleLine
                | TextFormatFlags.EndEllipsis
                | TextFormatFlags.NoPrefix);
        }

        private static GraphicsPath RoundedRectangle(RectangleF bounds, float radius)
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

        private static void DrawShineBand(Graphics graphics, float centerX, float width, Color color)
        {
            using GraphicsPath band = CreateShineBand(centerX, width, graphics.VisibleClipBounds.Height);
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
    }
}
