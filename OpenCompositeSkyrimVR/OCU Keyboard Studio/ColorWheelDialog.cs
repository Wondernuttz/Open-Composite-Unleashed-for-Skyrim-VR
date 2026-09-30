using System.ComponentModel;
using System.Drawing.Imaging;

namespace OCUKeyboardStudio;

internal sealed class ColorWheelDialog : Form
{
    private readonly HsvWheel _wheel = new() { Dock = DockStyle.Fill };
    private readonly TrackBar _value = new()
    {
        Orientation = Orientation.Vertical,
        Minimum = 10,
        Maximum = 100,
        TickFrequency = 10,
        Value = 100,
        Height = 190
    };
    private readonly TrackBar _alpha = new()
    {
        Orientation = Orientation.Horizontal,
        Minimum = 0,
        Maximum = 100,
        TickFrequency = 25,
        Value = 100,
        Width = 116,
        Height = 42
    };
    private readonly Panel _swatch = new() { Width = 92, Height = 44 };
    private readonly Label _hex = new() { AutoSize = true };

    public Color SelectedColor
    {
        get
        {
            Color rgb = _wheel.SelectedColor;
            return Color.FromArgb((int)Math.Round(_alpha.Value * 2.55), rgb.R, rgb.G, rgb.B);
        }
    }

    public ColorWheelDialog(string title, Color initial)
    {
        Text = title;
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ClientSize = new Size(480, 365);
        BackColor = StudioTheme.Window;
        ForeColor = StudioTheme.TextPrimary;
        Font = new Font("Segoe UI", 9.5f);
        StudioTheme.EnableDarkWindowChrome(this);

        var wheelPanel = new Panel { Dock = DockStyle.Fill, Padding = new Padding(14), BackColor = StudioTheme.Window };
        wheelPanel.Controls.Add(_wheel);

        var side = new FlowLayoutPanel
        {
            Dock = DockStyle.Right,
            Width = 145,
            Padding = new Padding(10, 15, 10, 10),
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            BackColor = StudioTheme.Surface
        };
        side.Controls.Add(new Label { Text = "BRIGHTNESS", AutoSize = true, ForeColor = StudioTheme.TextMuted });
        side.Controls.Add(_value);
        side.Controls.Add(new Label { Text = "OPACITY", AutoSize = true, ForeColor = StudioTheme.TextMuted });
        side.Controls.Add(_alpha);
        side.Controls.Add(_swatch);
        side.Controls.Add(_hex);

        var buttons = new FlowLayoutPanel
        {
            Dock = DockStyle.Bottom,
            Height = 55,
            Padding = new Padding(0, 8, 12, 8),
            FlowDirection = FlowDirection.RightToLeft,
            BackColor = StudioTheme.Surface
        };
        var ok = DialogButton("Use Color", DialogResult.OK);
        var cancel = DialogButton("Cancel", DialogResult.Cancel);
        buttons.Controls.Add(ok);
        buttons.Controls.Add(cancel);
        AcceptButton = ok;
        CancelButton = cancel;

        Controls.Add(wheelPanel);
        Controls.Add(side);
        Controls.Add(buttons);

        _wheel.SetColor(initial);
        _value.Value = Math.Clamp((int)Math.Round(_wheel.Value * 100), 10, 100);
        _alpha.Value = Math.Clamp((int)Math.Round(initial.A / 2.55), 0, 100);
        _wheel.ColorChanged += (_, _) => RefreshSwatch();
        _value.ValueChanged += (_, _) =>
        {
            _wheel.Value = _value.Value / 100.0;
            RefreshSwatch();
        };
        _alpha.ValueChanged += (_, _) => RefreshSwatch();
        RefreshSwatch();
    }

    private void RefreshSwatch()
    {
        Color color = SelectedColor;
        _swatch.BackColor = color;
        _hex.Text = $"#{color.R:X2}{color.G:X2}{color.B:X2}{color.A:X2}";
    }

    private static Button DialogButton(string text, DialogResult result)
    {
        var button = new ModernPillButton
        {
            Text = text,
            DialogResult = result,
            Width = 100,
            Height = 32,
            ForeColor = StudioTheme.TextPrimary,
            Margin = new Padding(7, 0, 0, 0)
        };
        return button;
    }
}

internal sealed class HsvWheel : Control
{
    private Bitmap? _bitmap;
    private double _hue;
    private double _saturation;
    private double _value = 1;

    public event EventHandler? ColorChanged;

    [Browsable(false), DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public double Value
    {
        get => _value;
        set
        {
            _value = Math.Clamp(value, 0.1, 1);
            RebuildBitmap();
            ColorChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    [Browsable(false), DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Color SelectedColor => FromHsv(_hue, _saturation, _value);

    public HsvWheel()
    {
        DoubleBuffered = true;
        Cursor = Cursors.Cross;
        MinimumSize = new Size(260, 260);
        SetStyle(ControlStyles.ResizeRedraw, true);
    }

    public void SetColor(Color color)
    {
        ToHsv(color, out _hue, out _saturation, out _value);
        RebuildBitmap();
        ColorChanged?.Invoke(this, EventArgs.Empty);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _bitmap?.Dispose();
        base.Dispose(disposing);
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        RebuildBitmap();
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        SelectPoint(e.Location);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (e.Button == MouseButtons.Left)
            SelectPoint(e.Location);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        if (_bitmap is not null)
            e.Graphics.DrawImageUnscaled(_bitmap, 0, 0);

        float radius = Math.Max(1, Math.Min(ClientSize.Width, ClientSize.Height) / 2f - 6);
        float centerX = ClientSize.Width / 2f;
        float centerY = ClientSize.Height / 2f;
        double radians = _hue * Math.PI / 180.0;
        float markerX = centerX + (float)Math.Cos(radians) * radius * (float)_saturation;
        float markerY = centerY + (float)Math.Sin(radians) * radius * (float)_saturation;
        using var outer = new Pen(Color.Black, 4);
        using var inner = new Pen(Color.White, 2);
        e.Graphics.DrawEllipse(outer, markerX - 7, markerY - 7, 14, 14);
        e.Graphics.DrawEllipse(inner, markerX - 7, markerY - 7, 14, 14);
    }

    private void SelectPoint(Point point)
    {
        double centerX = ClientSize.Width / 2.0;
        double centerY = ClientSize.Height / 2.0;
        double dx = point.X - centerX;
        double dy = point.Y - centerY;
        double radius = Math.Max(1, Math.Min(ClientSize.Width, ClientSize.Height) / 2.0 - 6);
        double distance = Math.Sqrt(dx * dx + dy * dy);
        _saturation = Math.Clamp(distance / radius, 0, 1);
        _hue = Math.Atan2(dy, dx) * 180.0 / Math.PI;
        if (_hue < 0)
            _hue += 360;
        Invalidate();
        ColorChanged?.Invoke(this, EventArgs.Empty);
    }

    private void RebuildBitmap()
    {
        if (ClientSize.Width <= 0 || ClientSize.Height <= 0)
            return;
        _bitmap?.Dispose();
        _bitmap = new Bitmap(ClientSize.Width, ClientSize.Height, PixelFormat.Format32bppArgb);
        float radius = Math.Max(1, Math.Min(ClientSize.Width, ClientSize.Height) / 2f - 6);
        float centerX = ClientSize.Width / 2f;
        float centerY = ClientSize.Height / 2f;
        for (int y = 0; y < _bitmap.Height; y++)
        {
            for (int x = 0; x < _bitmap.Width; x++)
            {
                double dx = x - centerX;
                double dy = y - centerY;
                double distance = Math.Sqrt(dx * dx + dy * dy);
                if (distance > radius)
                {
                    _bitmap.SetPixel(x, y, Color.Transparent);
                    continue;
                }
                double hue = Math.Atan2(dy, dx) * 180.0 / Math.PI;
                if (hue < 0)
                    hue += 360;
                _bitmap.SetPixel(x, y, FromHsv(hue, distance / radius, _value));
            }
        }
        Invalidate();
    }

    private static Color FromHsv(double hue, double saturation, double value)
    {
        double chroma = value * saturation;
        double x = chroma * (1 - Math.Abs((hue / 60.0) % 2 - 1));
        double match = value - chroma;
        (double r, double g, double b) = hue switch
        {
            < 60 => (chroma, x, 0.0),
            < 120 => (x, chroma, 0.0),
            < 180 => (0.0, chroma, x),
            < 240 => (0.0, x, chroma),
            < 300 => (x, 0.0, chroma),
            _ => (chroma, 0.0, x)
        };
        return Color.FromArgb(255,
            Math.Clamp((int)Math.Round((r + match) * 255), 0, 255),
            Math.Clamp((int)Math.Round((g + match) * 255), 0, 255),
            Math.Clamp((int)Math.Round((b + match) * 255), 0, 255));
    }

    private static void ToHsv(Color color, out double hue, out double saturation, out double value)
    {
        double r = color.R / 255.0;
        double g = color.G / 255.0;
        double b = color.B / 255.0;
        double max = Math.Max(r, Math.Max(g, b));
        double min = Math.Min(r, Math.Min(g, b));
        double delta = max - min;
        hue = delta == 0 ? 0
            : max == r ? 60 * (((g - b) / delta) % 6)
            : max == g ? 60 * (((b - r) / delta) + 2)
            : 60 * (((r - g) / delta) + 4);
        if (hue < 0)
            hue += 360;
        saturation = max == 0 ? 0 : delta / max;
        value = max;
    }
}
