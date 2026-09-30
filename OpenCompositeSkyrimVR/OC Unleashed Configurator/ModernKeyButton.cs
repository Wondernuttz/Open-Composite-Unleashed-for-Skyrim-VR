using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    internal enum KeyVisualState
    {
        Unbound,
        Bound,
        Selected
    }

    /// <summary>
    /// Keyboard key with an intentionally self-contained visual state. The
    /// application theme must never turn bound and unbound keys into the same
    /// generic button again.
    /// </summary>
    internal sealed class ModernKeyButton : Button
    {
        private KeyVisualState _visualState;
        private bool _hovered;
        private bool _pressed;
        private float _glowIntensity = 0.65f;

        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal KeyVisualState VisualState
        {
            get => _visualState;
            set
            {
                if (_visualState == value)
                    return;

                _visualState = value;
                Invalidate();
            }
        }

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
                if (_visualState != KeyVisualState.Unbound)
                    Invalidate();
            }
        }

        internal ModernKeyButton()
        {
            SetStyle(ControlStyles.UserPaint
                | ControlStyles.AllPaintingInWmPaint
                | ControlStyles.OptimizedDoubleBuffer
                | ControlStyles.ResizeRedraw
                | ControlStyles.SupportsTransparentBackColor, true);

            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            UseVisualStyleBackColor = false;
            BackColor = Color.Transparent;
            ForeColor = ModernUiTheme.TextPrimary;
            Cursor = Cursors.Hand;
            TabStop = true;
        }

        internal void ApplyTheme()
        {
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            UseVisualStyleBackColor = false;
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
            Invalidate();
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

        protected override void OnPaintBackground(PaintEventArgs pevent)
        {
            // Painted with the foreground so the clipped corners and glow share
            // one double-buffered pass.
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics graphics = e.Graphics;
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.Clear(Parent?.BackColor ?? ModernUiTheme.Surface);

            RectangleF keyBounds = new(3f, 3f, Math.Max(1f, Width - 7f), Math.Max(1f, Height - 7f));
            using GraphicsPath keyPath = CreateChamferedPath(keyBounds, Math.Min(5f, Height / 5f));

            Color top;
            Color bottom;
            Color border;
            Color text;
            Color smoke;
            float borderWidth;
            float pulse = _visualState == KeyVisualState.Unbound
                ? 0f
                : Math.Min(1f, _glowIntensity + (_hovered ? 0.12f : 0f));

            switch (_visualState)
            {
                case KeyVisualState.Bound:
                    top = _pressed ? Color.FromArgb(33, 82, 66)
                        : _hovered ? Color.FromArgb(38, 92, 74)
                        : Blend(Color.FromArgb(24, 54, 50), Color.FromArgb(31, 78, 63), pulse);
                    bottom = Color.FromArgb(16, 31, 32);
                    border = Blend(
                        Color.FromArgb(42, 132, 102),
                        _hovered ? ModernUiTheme.KeyGlowHover : ModernUiTheme.KeyGlow,
                        pulse);
                    text = Color.FromArgb(228, 255, 241);
                    smoke = Color.FromArgb(
                        (int)(64 + 92 * pulse),
                        ModernUiTheme.KeyGlowFill);
                    borderWidth = 1.55f;
                    break;

                case KeyVisualState.Selected:
                    top = _pressed ? Color.FromArgb(44, 111, 62)
                        : Blend(Color.FromArgb(37, 91, 56), Color.FromArgb(52, 137, 73), pulse);
                    bottom = Color.FromArgb(22, 55, 38);
                    border = Blend(ModernUiTheme.KeyGlow, ModernUiTheme.KeyGlowBright, pulse);
                    text = Color.White;
                    smoke = Color.FromArgb(
                        (int)(110 + 90 * pulse),
                        Color.FromArgb(101, 234, 135));
                    borderWidth = 2f;
                    break;

                default:
                    top = _pressed ? Color.FromArgb(24, 29, 36)
                        : _hovered ? Color.FromArgb(38, 44, 53)
                        : Color.FromArgb(29, 34, 42);
                    bottom = Color.FromArgb(19, 23, 30);
                    border = _hovered ? Color.FromArgb(71, 84, 98)
                        : Color.FromArgb(51, 60, 72);
                    text = _hovered ? ModernUiTheme.TextPrimary
                        : Color.FromArgb(194, 201, 211);
                    smoke = Color.Transparent;
                    borderWidth = 1f;
                    break;
            }

            if (_visualState != KeyVisualState.Unbound)
            {
                using Pen outerGlow = new(Color.FromArgb(
                    _visualState == KeyVisualState.Selected
                        ? (int)(42 + 62 * pulse)
                        : (int)(22 + 48 * pulse),
                    border), 5f);
                graphics.DrawPath(outerGlow, keyPath);
            }

            using (var baseFill = new LinearGradientBrush(keyBounds, top, bottom, 90f))
                graphics.FillPath(baseFill, keyPath);

            if (_visualState != KeyVisualState.Unbound)
            {
                GraphicsState clipState = graphics.Save();
                graphics.SetClip(keyPath);

                RectangleF hazeBounds = new(
                    keyBounds.Left + keyBounds.Width * 0.08f,
                    keyBounds.Top - keyBounds.Height * 0.35f,
                    keyBounds.Width * 0.84f,
                    keyBounds.Height * 1.25f);
                using GraphicsPath hazePath = new();
                hazePath.AddEllipse(hazeBounds);
                using PathGradientBrush haze = new(hazePath)
                {
                    CenterColor = smoke,
                    SurroundColors = new[] { Color.Transparent },
                    CenterPoint = new PointF(
                        keyBounds.Left + keyBounds.Width * 0.52f,
                        keyBounds.Top + keyBounds.Height * 0.33f)
                };
                graphics.FillEllipse(haze, hazeBounds);
                graphics.Restore(clipState);

                RectangleF insetBounds = RectangleF.Inflate(keyBounds, -2.2f, -2.2f);
                using GraphicsPath insetPath = CreateChamferedPath(
                    insetBounds, Math.Max(2f, Math.Min(5f, Height / 5f) - 2f));
                using Pen innerGlow = new(Color.FromArgb(
                    _visualState == KeyVisualState.Selected
                        ? (int)(38 + 62 * pulse)
                        : (int)(18 + 46 * pulse),
                    Color.White), 1f);
                graphics.DrawPath(innerGlow, insetPath);
            }

            using (Pen outline = new(border, borderWidth))
                graphics.DrawPath(outline, keyPath);

            if (_visualState != KeyVisualState.Unbound)
            {
                float stripY = keyBounds.Bottom - 2.5f;
                using Pen activeStrip = new(Color.FromArgb(
                    _visualState == KeyVisualState.Selected
                        ? (int)(130 + 105 * pulse)
                        : (int)(72 + 110 * pulse),
                    border), 1.5f);
                graphics.DrawLine(activeStrip,
                    keyBounds.Left + 7f, stripY,
                    keyBounds.Right - 7f, stripY);
            }

            Rectangle textBounds = Rectangle.Inflate(ClientRectangle, -5, -4);
            TextRenderer.DrawText(
                graphics,
                Text,
                Font,
                textBounds,
                Enabled ? text : ModernUiTheme.TextMuted,
                TextFormatFlags.HorizontalCenter
                    | TextFormatFlags.VerticalCenter
                    | TextFormatFlags.WordBreak
                    | TextFormatFlags.EndEllipsis
                    | TextFormatFlags.NoPrefix);

            if (Focused && ShowFocusCues)
            {
                Rectangle focus = Rectangle.Inflate(textBounds, -1, -1);
                ControlPaint.DrawFocusRectangle(graphics, focus, text, Color.Transparent);
            }
        }

        private static GraphicsPath CreateChamferedPath(RectangleF bounds, float cut)
        {
            float left = bounds.Left;
            float top = bounds.Top;
            float right = bounds.Right;
            float bottom = bounds.Bottom;
            cut = Math.Max(1f, Math.Min(cut, Math.Min(bounds.Width, bounds.Height) / 3f));

            GraphicsPath path = new();
            path.AddPolygon(new[]
            {
                new PointF(left + cut, top),
                new PointF(right - cut, top),
                new PointF(right, top + cut),
                new PointF(right, bottom - cut),
                new PointF(right - cut, bottom),
                new PointF(left + cut, bottom),
                new PointF(left, bottom - cut),
                new PointF(left, top + cut)
            });
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
}
