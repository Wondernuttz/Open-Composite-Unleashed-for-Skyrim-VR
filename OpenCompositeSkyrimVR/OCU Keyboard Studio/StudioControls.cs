using System.ComponentModel;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace OCUKeyboardStudio;

internal static class StudioTheme
{
    internal static readonly Color Window = Color.FromArgb(14, 17, 22);
    internal static readonly Color Surface = Color.FromArgb(22, 26, 33);
    internal static readonly Color SurfaceRaised = Color.FromArgb(27, 32, 40);
    internal static readonly Color SurfaceHover = Color.FromArgb(36, 42, 51);
    internal static readonly Color Input = Color.FromArgb(20, 24, 31);
    internal static readonly Color Border = Color.FromArgb(48, 56, 68);
    internal static readonly Color BorderSoft = Color.FromArgb(37, 43, 53);
    internal static readonly Color KeyGlow = Color.FromArgb(62, 190, 143);
    internal static readonly Color KeyGlowHover = Color.FromArgb(95, 229, 176);
    internal static readonly Color KeyGlowBright = Color.FromArgb(132, 242, 158);
    internal static readonly Color KeyGlowFill = Color.FromArgb(61, 188, 144);
    internal static readonly Color TextPrimary = Color.FromArgb(237, 240, 245);
    internal static readonly Color TextSecondary = Color.FromArgb(181, 188, 199);
    internal static readonly Color TextMuted = Color.FromArgb(126, 136, 150);

    internal static void EnableDarkWindowChrome(Form form)
    {
        void Apply()
        {
            try
            {
                int enabled = 1;
                if (DwmSetWindowAttribute(form.Handle, 20, ref enabled, sizeof(int)) != 0)
                    DwmSetWindowAttribute(form.Handle, 19, ref enabled, sizeof(int));
            }
            catch
            {
                // Older Windows builds can reject the dark-title-bar attribute.
            }
        }

        form.HandleCreated += (_, _) => Apply();
        if (form.IsHandleCreated)
            Apply();
    }

    internal static GraphicsPath RoundedRectangle(RectangleF bounds, float radius)
    {
        float diameter = Math.Min(radius * 2f, Math.Min(bounds.Width, bounds.Height));
        var path = new GraphicsPath();
        path.AddArc(bounds.Left, bounds.Top, diameter, diameter, 180f, 90f);
        path.AddArc(bounds.Right - diameter, bounds.Top, diameter, diameter, 270f, 90f);
        path.AddArc(bounds.Right - diameter, bounds.Bottom - diameter, diameter, diameter, 0f, 90f);
        path.AddArc(bounds.Left, bounds.Bottom - diameter, diameter, diameter, 90f, 90f);
        path.CloseFigure();
        return path;
    }

    internal static Color Blend(Color from, Color to, float amount)
    {
        amount = Math.Clamp(amount, 0f, 1f);
        return Color.FromArgb(
            (int)(from.A + (to.A - from.A) * amount),
            (int)(from.R + (to.R - from.R) * amount),
            (int)(from.G + (to.G - from.G) * amount),
            (int)(from.B + (to.B - from.B) * amount));
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int valueSize);
}

internal sealed class ModernPillButton : Button
{
    private bool _hovered;
    private bool _pressed;

    [Browsable(false)]
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    internal bool Destructive { get; set; }

    internal ModernPillButton()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint
            | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw
            | ControlStyles.SupportsTransparentBackColor, true);
        FlatStyle = FlatStyle.Flat;
        FlatAppearance.BorderSize = 0;
        UseVisualStyleBackColor = false;
        Cursor = Cursors.Hand;
        TabStop = true;
    }

    protected override void OnMouseEnter(EventArgs e) { _hovered = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hovered = false; _pressed = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { _pressed = true; Invalidate(); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { _pressed = false; Invalidate(); base.OnMouseUp(e); }
    protected override void OnMouseCaptureChanged(EventArgs e) { _pressed = false; Invalidate(); base.OnMouseCaptureChanged(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
    protected override void OnPaintBackground(PaintEventArgs e) { }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics graphics = e.Graphics;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
        graphics.Clear(Parent?.BackColor ?? StudioTheme.Surface);

        RectangleF bounds = new(2.5f, 2.5f, Math.Max(1f, Width - 5.5f), Math.Max(1f, Height - 5.5f));
        using GraphicsPath path = StudioTheme.RoundedRectangle(bounds, Math.Max(3f, bounds.Height / 2f));

        Color top;
        Color bottom;
        Color border;
        Color text;
        int glowAlpha;
        if (!Enabled)
        {
            top = StudioTheme.SurfaceRaised;
            bottom = StudioTheme.Surface;
            border = StudioTheme.BorderSoft;
            text = StudioTheme.TextMuted;
            glowAlpha = 0;
        }
        else if (Destructive)
        {
            top = _pressed ? Color.FromArgb(82, 31, 35)
                : _hovered ? Color.FromArgb(139, 54, 59)
                : Color.FromArgb(105, 40, 45);
            bottom = Color.FromArgb(47, 24, 29);
            border = _hovered ? Color.FromArgb(224, 103, 108) : Color.FromArgb(184, 75, 82);
            text = Color.FromArgb(255, 235, 236);
            glowAlpha = _hovered ? 45 : 25;
        }
        else
        {
            top = _pressed ? Color.FromArgb(33, 82, 66)
                : _hovered ? Color.FromArgb(38, 92, 74)
                : Color.FromArgb(27, 65, 57);
            bottom = Color.FromArgb(16, 31, 32);
            border = _hovered ? StudioTheme.KeyGlowHover : StudioTheme.KeyGlow;
            text = Color.FromArgb(228, 255, 241);
            glowAlpha = _hovered ? 62 : 42;
        }

        if (glowAlpha > 0)
        {
            using var glow = new Pen(Color.FromArgb(glowAlpha, border), 4f);
            graphics.DrawPath(glow, path);
        }
        using (var fill = new LinearGradientBrush(bounds, top, bottom, 90f))
            graphics.FillPath(fill, path);

        if (!Destructive && Enabled)
        {
            GraphicsState clip = graphics.Save();
            graphics.SetClip(path);
            RectangleF hazeBounds = new(bounds.Left + bounds.Width * 0.12f, bounds.Top - bounds.Height * 0.50f,
                bounds.Width * 0.76f, bounds.Height * 1.45f);
            using var hazePath = new GraphicsPath();
            hazePath.AddEllipse(hazeBounds);
            using var haze = new PathGradientBrush(hazePath)
            {
                CenterColor = Color.FromArgb(_hovered ? 115 : 78, StudioTheme.KeyGlowFill),
                SurroundColors = [Color.Transparent]
            };
            graphics.FillEllipse(haze, hazeBounds);
            graphics.Restore(clip);
        }

        using (var outline = new Pen(border, Enabled ? 1.35f : 1f))
            graphics.DrawPath(outline, path);
        Rectangle textBounds = Rectangle.Inflate(ClientRectangle, -9, -2);
        TextRenderer.DrawText(graphics, Text, Font, textBounds, text,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter
            | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
    }
}

internal sealed class ModernCheckBox : CheckBox
{
    private bool _hovered;

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
        return new Size(text.Width + 45, Math.Max(20, text.Height + 3));
    }

    protected override void OnMouseEnter(EventArgs e) { _hovered = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hovered = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnCheckedChanged(EventArgs e) { Invalidate(); base.OnCheckedChanged(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        Color background = Parent?.BackColor ?? StudioTheme.Window;
        e.Graphics.Clear(background);

        const int box = 15;
        int top = Math.Max(1, (Height - box) / 2);
        var rect = new Rectangle(4, top, box, box);
        float pulse = Checked ? (_hovered ? 0.88f : 0.68f) : 0f;
        Color fillTop = Checked
            ? StudioTheme.Blend(Color.FromArgb(24, 54, 50),
                _hovered ? Color.FromArgb(38, 92, 74) : Color.FromArgb(31, 78, 63), pulse)
            : _hovered ? StudioTheme.SurfaceHover : StudioTheme.Input;
        Color fillBottom = Checked ? Color.FromArgb(16, 31, 32) : fillTop;
        Color border = Checked
            ? StudioTheme.Blend(StudioTheme.KeyGlow, StudioTheme.KeyGlowBright, pulse)
            : StudioTheme.Border;
        if (!Enabled)
        {
            fillTop = StudioTheme.SurfaceRaised;
            fillBottom = StudioTheme.Surface;
            border = StudioTheme.BorderSoft;
        }

        using (GraphicsPath path = StudioTheme.RoundedRectangle(rect, 4))
        {
            if (Checked && Enabled)
            {
                using var glow = new Pen(Color.FromArgb((int)(22 + 52 * pulse), border), 4f);
                e.Graphics.DrawPath(glow, path);
            }
            using var brush = new LinearGradientBrush(rect, fillTop, fillBottom, 90f);
            using var pen = new Pen(border, 1.2f);
            e.Graphics.FillPath(brush, path);
            e.Graphics.DrawPath(pen, path);
        }

        if (Checked)
        {
            PointF[] points = [new(7.5f, top + 7.5f), new(10.2f, top + 10.3f), new(15.8f, top + 4.5f)];
            if (Enabled)
            {
                using var checkGlow = new Pen(Color.FromArgb((int)(35 + 95 * pulse), StudioTheme.KeyGlowBright), 4f)
                { StartCap = LineCap.Round, EndCap = LineCap.Round };
                e.Graphics.DrawLines(checkGlow, points);
            }
            using var check = new Pen(Enabled ? Color.White : StudioTheme.TextMuted, 2f)
            { StartCap = LineCap.Round, EndCap = LineCap.Round };
            e.Graphics.DrawLines(check, points);
        }

        var textRect = new Rectangle(26, 0, Math.Max(0, Width - 26), Height);
        TextRenderer.DrawText(e.Graphics, Text, Font, textRect,
            Enabled ? ForeColor : StudioTheme.TextMuted,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter
            | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
    }
}

internal sealed class ColorEntryControl : UserControl
{
    private readonly Button _swatch = new()
    {
        Dock = DockStyle.Left,
        Width = 34,
        FlatStyle = FlatStyle.Flat,
        Cursor = Cursors.Hand,
        TabStop = false,
        AccessibleName = "Open color wheel"
    };
    private readonly TextBox _hex = new()
    {
        Dock = DockStyle.Fill,
        BorderStyle = BorderStyle.FixedSingle,
        BackColor = StudioTheme.Input,
        ForeColor = StudioTheme.TextPrimary,
        TextAlign = HorizontalAlignment.Center
    };
    private Color _value = Color.White;
    private bool _updating;

    internal event EventHandler? SwatchClicked;
    internal event Action<Color>? ColorCommitted;

    internal ColorEntryControl()
    {
        Height = 32;
        MinimumSize = new Size(120, 30);
        BackColor = StudioTheme.Surface;
        _swatch.FlatAppearance.BorderColor = StudioTheme.Border;
        _swatch.FlatAppearance.BorderSize = 1;
        _swatch.Click += (_, e) => SwatchClicked?.Invoke(this, e);
        _hex.KeyDown += (_, e) =>
        {
            if (e.KeyCode != Keys.Enter)
                return;
            CommitText();
            e.SuppressKeyPress = true;
            e.Handled = true;
        };
        _hex.Leave += (_, _) => CommitText();
        Controls.Add(_hex);
        Controls.Add(_swatch);
        Value = Color.White;
    }

    [Browsable(false)]
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    internal Color Value
    {
        get => _value;
        set
        {
            _updating = true;
            try
            {
                _value = value;
                _swatch.BackColor = Color.FromArgb(255, value.R, value.G, value.B);
                _hex.Text = Format(value);
            }
            finally
            {
                _updating = false;
            }
        }
    }

    protected override void OnEnabledChanged(EventArgs e)
    {
        _swatch.Enabled = Enabled;
        _hex.Enabled = Enabled;
        base.OnEnabledChanged(e);
    }

    private void CommitText()
    {
        if (_updating)
            return;
        if (!TryParse(_hex.Text, out Color parsed))
        {
            System.Media.SystemSounds.Beep.Play();
            _hex.Text = Format(_value);
            return;
        }
        if (parsed.ToArgb() == _value.ToArgb())
        {
            _hex.Text = Format(_value);
            return;
        }
        _value = parsed;
        _swatch.BackColor = Color.FromArgb(255, parsed.R, parsed.G, parsed.B);
        _hex.Text = Format(parsed);
        ColorCommitted?.Invoke(parsed);
    }

    private static bool TryParse(string text, out Color color)
    {
        color = Color.Empty;
        string value = text.Trim().TrimStart('#');
        if (value.Length is not (6 or 8)
            || !uint.TryParse(value, System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture, out uint packed))
            return false;

        if (value.Length == 6)
        {
            color = Color.FromArgb(255, (int)((packed >> 16) & 0xff),
                (int)((packed >> 8) & 0xff), (int)(packed & 0xff));
        }
        else
        {
            color = Color.FromArgb((int)(packed & 0xff), (int)((packed >> 24) & 0xff),
                (int)((packed >> 16) & 0xff), (int)((packed >> 8) & 0xff));
        }
        return true;
    }

    private static string Format(Color color) => color.A == 255
        ? $"#{color.R:X2}{color.G:X2}{color.B:X2}"
        : $"#{color.R:X2}{color.G:X2}{color.B:X2}{color.A:X2}";
}

internal sealed class ModernTabControl : TabControl
{
    internal ModernTabControl()
    {
        DrawMode = TabDrawMode.OwnerDrawFixed;
        SizeMode = TabSizeMode.Fixed;
        // Five concise inspector tabs fit the fixed right pane without the
        // WinForms overflow arrow that hides the final tab.
        ItemSize = new Size(80, 34);
        Padding = new Point(0, 0);
    }

    protected override void OnDrawItem(DrawItemEventArgs e)
    {
        Graphics graphics = e.Graphics;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        Rectangle bounds = e.Bounds;
        using (var clear = new SolidBrush(StudioTheme.Window))
            graphics.FillRectangle(clear, bounds);
        RectangleF tab = new(bounds.Left + 3f, bounds.Top + 3f,
            Math.Max(1f, bounds.Width - 6f), Math.Max(1f, bounds.Height - 3f));
        bool selected = e.Index == SelectedIndex;
        using GraphicsPath path = RoundedTopTab(tab, 9f);
        Color fill = selected ? Color.FromArgb(27, 65, 57) : StudioTheme.Surface;
        Color border = selected ? StudioTheme.KeyGlowBright : StudioTheme.Border;
        using (var brush = new SolidBrush(fill))
            graphics.FillPath(brush, path);
        using (var pen = new Pen(border, selected ? 1.35f : 1f))
            graphics.DrawPath(pen, path);
        TextRenderer.DrawText(graphics, TabPages[e.Index].Text, Font, Rectangle.Round(tab),
            selected ? StudioTheme.TextPrimary : StudioTheme.TextSecondary,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter
            | TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix);
    }

    private static GraphicsPath RoundedTopTab(RectangleF rectangle, float radius)
    {
        float diameter = Math.Min(Math.Min(radius * 2f, rectangle.Width), rectangle.Height * 2f);
        var path = new GraphicsPath();
        path.StartFigure();
        path.AddLine(rectangle.Left, rectangle.Bottom, rectangle.Left, rectangle.Top + diameter / 2f);
        path.AddArc(rectangle.Left, rectangle.Top, diameter, diameter, 180, 90);
        path.AddLine(rectangle.Left + diameter / 2f, rectangle.Top,
            rectangle.Right - diameter / 2f, rectangle.Top);
        path.AddArc(rectangle.Right - diameter, rectangle.Top, diameter, diameter, 270, 90);
        path.AddLine(rectangle.Right, rectangle.Top + diameter / 2f, rectangle.Right, rectangle.Bottom);
        path.CloseFigure();
        return path;
    }
}
