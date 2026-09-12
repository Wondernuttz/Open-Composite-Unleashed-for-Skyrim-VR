using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace OpenCompositeConfigurator;

internal sealed class EyeFoveationButton : ModernPillButton
{
    private readonly Timer _timer = new() { Interval = 60 };
    private float _phase;
    public EyeFoveationButton()
    {
        Text = "Edit eye-tracked rings";
        AccessibleName = "Edit eye-tracked foveation";
        AccessibleDescription = "Opens the ring preview and all eye-tracked foveation settings.";
        // Keep the positive role when the shared theme classifies this button.
        BackColor = ModernUiTheme.Accent;
        VisualRole = ModernButtonRole.Positive;
        _timer.Tick += (_, _) => { _phase += .08f; Invalidate(); };
    }
    protected override void OnVisibleChanged(EventArgs e) { base.OnVisibleChanged(e); _timer.Enabled = Visible; }
    protected override void DrawContent(Graphics graphics, Rectangle bounds, Color color)
    {
        float scale = Height / 28f;
        var eye = new RectangleF(9 * scale, 7 * scale, 27 * scale, 14 * scale);
        EyeFoveationPreview.DrawEye(graphics, eye, (float)Math.Sin(_phase) * 3 * scale,
            Enabled ? ModernUiTheme.KeyGlowBright : ModernUiTheme.TextMuted);
        TextRenderer.DrawText(graphics, Text, Font, new Rectangle((int)(45 * scale), bounds.Top,
            Math.Max(0, Width - (int)(48 * scale)), bounds.Height), color,
            TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis
            | TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix);
    }
    protected override void Dispose(bool disposing) { if (disposing) _timer.Dispose(); base.Dispose(disposing); }
}

internal sealed class EyeFoveationPreview : Control
{
    internal static readonly Color CenterColor = Color.FromArgb(114, 236, 169);
    internal static readonly Color MiddleColor = Color.FromArgb(110, 188, 253);
    internal static readonly Color OuterColor = Color.FromArgb(192, 166, 245);
    private readonly Timer _timer = new() { Interval = 40 };
    private float _phase;
    private EyeFoveationSettings _settings = new();
    private bool _animate = true;
    private bool _showEffect = true;
    private bool _pointerInside;
    private readonly EyeFoveationPhoto _photo = new();
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public EyeFoveationSettings Settings { get => _settings; set { _settings = value; UpdateAccessibility(); Invalidate(); } }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Animate { get => _animate; set { _animate = value; _timer.Enabled = value && Visible; Invalidate(); } }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool ShowEffect { get => _showEffect; set { _showEffect = value; Invalidate(); } }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    internal PointF DemoOffset { get; set; }

    public EyeFoveationPreview()
    {
        BackColor = ModernUiTheme.Surface;
        ForeColor = ModernUiTheme.TextPrimary;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        AccessibleRole = AccessibleRole.Graphic;
        _timer.Tick += (_, _) => {
            if (_pointerInside) return;
            _phase += .022f;
            DemoOffset = new PointF((float)Math.Sin(_phase) * .12f, (float)Math.Sin(_phase * .63f) * .075f);
            Invalidate();
        };
        UpdateAccessibility();
    }
    private void UpdateAccessibility()
    {
        var effective = Settings.EffectiveRates;
        AccessibleName = "Eye-tracked ring preview";
        AccessibleDescription = $"Configured center boundary {Settings.Radii.Inner:0.00}, middle boundary {Settings.Radii.Mid:0.00}. " +
            $"Effective rates: center {effective[0]}, middle {effective[1]}, outer {effective[2]}. " +
            "Illustrative gaze motion, not live headset tracking.";
    }
    protected override void OnVisibleChanged(EventArgs e) { base.OnVisibleChanged(e); _timer.Enabled = Visible && Animate; }
    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        var plot = PlotBounds(); _pointerInside = plot.Contains(e.Location);
        if (_pointerInside) {
            DemoOffset = new PointF((e.X - plot.Left) / plot.Width - .5f, (e.Y - plot.Top) / plot.Height - .5f);
            Invalidate();
        }
    }
    protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); _pointerInside = false; }
    internal RectangleF PlotBounds()
    {
        float s = Math.Max(DeviceDpi / 96f, Font.SizeInPoints / 10f);
        float pad = 22 * s;
        float side = Math.Max(80 * s, Math.Min(Width - pad * 2, Height - 265 * s));
        return new RectangleF((Width - side) / 2, pad + 72 * s, side, side);
    }
    internal static void DrawEye(Graphics g, RectangleF r, float pupilOffset, Color color)
    {
        using var path = new GraphicsPath();
        path.AddBezier(r.Left, r.Top + r.Height / 2, r.Left + r.Width * .28f, r.Top - r.Height * .4f,
            r.Right - r.Width * .28f, r.Top - r.Height * .4f, r.Right, r.Top + r.Height / 2);
        path.AddBezier(r.Right, r.Top + r.Height / 2, r.Right - r.Width * .28f, r.Bottom + r.Height * .4f,
            r.Left + r.Width * .28f, r.Bottom + r.Height * .4f, r.Left, r.Top + r.Height / 2);
        using var pen = new Pen(color, Math.Max(1.4f, r.Height / 10));
        g.DrawPath(pen, path);
        float radius = r.Height * .29f;
        using var brush = new SolidBrush(color);
        g.FillEllipse(brush, r.Left + r.Width / 2 + pupilOffset - radius,
            r.Top + r.Height / 2 - radius, radius * 2, radius * 2);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics; g.SmoothingMode = SmoothingMode.AntiAlias;
        float s = Math.Max(DeviceDpi / 96f, Font.SizeInPoints / 10f);
        float pad = 22 * s;
        using var heading = new Font(Font.FontFamily, Font.SizeInPoints * 1.2f, FontStyle.Bold);
        using var small = new Font(Font.FontFamily, Font.SizeInPoints * .9f);
        using var primary = new SolidBrush(ModernUiTheme.TextPrimary);
        using var secondary = new SolidBrush(ModernUiTheme.TextSecondary);
        g.DrawString("PugDragon · detail preview", heading, primary, pad, pad);
        g.DrawString("Move over the photo to aim the rings", small, secondary, pad, pad + 27 * s);

        var plot = PlotBounds();
        var saved = g.Save();
        g.SetClip(plot);
        var gaze = new PointF(plot.Left + plot.Width * (.5f + DemoOffset.X), plot.Top + plot.Height * (.5f + DemoOffset.Y));
        _photo.Draw(g, plot, gaze, Settings, ShowEffect);
        RectangleF ring(decimal radius) => EyeFoveationPhoto.RingBounds(plot, gaze, radius);
        var mid = ring(Settings.Radii.Mid); var inner = ring(Settings.Radii.Inner);
        using var shadow = new Pen(Color.FromArgb(180, 0, 0, 0), 4 * s);
        g.DrawEllipse(shadow, mid); g.DrawEllipse(shadow, inner);
        using var midPen = new Pen(MiddleColor, 2 * s) { DashStyle = DashStyle.Dash };
        using var innerPen = new Pen(CenterColor, 2 * s);
        g.DrawEllipse(midPen, mid); g.DrawEllipse(innerPen, inner);
        using var dot = new SolidBrush(Color.White);
        g.FillEllipse(dot, gaze.X - 3 * s, gaze.Y - 3 * s, 6 * s, 6 * s);
        DrawEye(g, new RectangleF(gaze.X - 12 * s, gaze.Y + 10 * s, 24 * s, 11 * s), 0, Color.White);
        g.Restore(saved);
        using var border = new Pen(ModernUiTheme.Border, s); g.DrawRectangle(border, plot.X, plot.Y, plot.Width, plot.Height);
        float y = plot.Bottom + 16 * s;
        var requested = Settings.RequestedRates; var effective = Settings.EffectiveRates;
        string[] labels = { $"Center  ·  {Settings.Radii.Inner:0.00}", $"Middle  ·  {Settings.Radii.Mid:0.00}", "Outer  ·  beyond middle" };
        Color[] colors = { CenterColor, MiddleColor, OuterColor };
        for (int i = 0; i < 3; i++) {
            using var color = new SolidBrush(colors[i]);
            g.FillEllipse(color, pad, y + 5 * s, 8 * s, 8 * s);
            g.DrawString(labels[i], Font, primary, pad + 18 * s, y);
            string rate = Settings.Backend == 3 ? "Effects only" : requested[i] + "  →  " + effective[i];
            var size = g.MeasureString(rate, Font);
            g.DrawString(rate, Font, color, Width - pad - size.Width, y);
            y += 27 * s;
        }
        g.DrawString("Requested → effective rate · square eye-texture view", small, secondary, pad, y + 5 * s);
        g.DrawString("Photo sampling illustration. In-game VRS / Density Mask results differ.", small, secondary,
            new RectangleF(pad, y + 30 * s, Width - 2 * pad, 42 * s));
    }
    protected override void Dispose(bool disposing) { if (disposing) { _timer.Dispose(); _photo.Dispose(); } base.Dispose(disposing); }
}

internal sealed class EyeFoveationEditor : Form
{
    internal readonly ComboBox Preset = Choice(FoveationProfiles.EyePresetNames, "Eye preset");
    internal readonly ComboBox Backend = Choice(EyeFoveationSettings.BackendNames, "Foveation backend (shared)");
    internal readonly CheckBox EyeEnabled = Toggle("Enable automatic eye tracking", "Enable automatic eye tracking");
    internal readonly CheckBox CustomRates = Toggle("Choose each ring's rate", "Use custom eye-tracked rates");
    internal readonly CheckBox Cap = Toggle("Limit eye rings to half rate", "Eye max half-rate compatibility cap");
    internal readonly CheckBox Horizontal = Toggle("Favor horizontal half rate", "Shared half-rate direction");
    internal readonly CheckBox DebugRings = Toggle("Show rings inside the headset (debug)", "Show eye-tracking debug rings");
    internal readonly NumericUpDown Inner = Number(1m, "Eye center boundary");
    internal readonly NumericUpDown Mid = Number(1.5m, "Eye middle boundary");
    internal readonly ComboBox InnerRate = Choice(FoveationProfiles.RateChoices, "Eye center requested rate");
    internal readonly ComboBox MidRate = Choice(FoveationProfiles.RateChoices, "Eye middle requested rate");
    internal readonly ComboBox OuterRate = Choice(FoveationProfiles.RateChoices, "Eye outer requested rate");
    internal readonly EyeFoveationPreview Preview = new() { Dock = DockStyle.Fill };
    internal readonly Label Status = TextLabel("");
    internal EyeFoveationSettings? AcceptedSettings { get; private set; }
    private bool _loading;
    private readonly ToolTip _tips = new() { AutoPopDelay = 20000, InitialDelay = 300 };

    public EyeFoveationEditor(EyeFoveationSettings settings)
    {
        Text = "Eye-tracked foveation";
        Name = "EyeFoveationEditor";
        AccessibleName = Text;
        BackColor = ModernUiTheme.Window; ForeColor = ModernUiTheme.TextPrimary;
        Font = new Font("Segoe UI", 10);
        AutoScaleDimensions = new SizeF(96, 96); AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(1040, 850); MinimumSize = new Size(880, 720);
        StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false; MinimizeBox = false;
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(24), ColumnCount = 1, RowCount = 3 };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 84));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 65));
        Controls.Add(root);
        var header = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
        header.RowStyles.Add(new RowStyle(SizeType.AutoSize)); header.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var title = TextLabel("Eye-tracked foveation"); title.Font = new Font(Font.FontFamily, 18, FontStyle.Bold);
        header.Controls.Add(title); header.Controls.Add(TextLabel("Shape the detail around your gaze. The preview updates as you edit."));
        root.Controls.Add(header, 0, 0);
        var content = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1 };
        content.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        content.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
        content.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
        root.Controls.Add(content, 0, 1);
        Preview.Margin = new Padding(0, 0, 20, 0); content.Controls.Add(Preview, 0, 0);
        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        content.Controls.Add(scroll, 1, 0);
        var fields = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, Padding = new Padding(0, 0, 12, 8) };
        fields.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); scroll.Controls.Add(fields);
        void add(Control control) { control.Margin = new Padding(0, 0, 0, 9); fields.Controls.Add(control, 0, fields.RowCount++); }
        add(EyeEnabled);
        add(TextLabel("Backend · shared with fixed VRS / fallback")); add(Backend);
        add(TextLabel("Eye preset")); add(Preset);
        var grid = new TableLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Top, ColumnCount = 3, RowCount = 4 };
        for (int row = 0; row < 4; ++row) grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 33)); grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 34)); grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 33));
        grid.Controls.Add(TextLabel("Ring"), 0, 0); grid.Controls.Add(TextLabel("Boundary"), 1, 0); grid.Controls.Add(TextLabel("Rate"), 2, 0);
        grid.Controls.Add(TextLabel("Center", EyeFoveationPreview.CenterColor), 0, 1); grid.Controls.Add(Inner, 1, 1); grid.Controls.Add(InnerRate, 2, 1);
        grid.Controls.Add(TextLabel("Middle", EyeFoveationPreview.MiddleColor), 0, 2); grid.Controls.Add(Mid, 1, 2); grid.Controls.Add(MidRate, 2, 2);
        grid.Controls.Add(TextLabel("Outer", EyeFoveationPreview.OuterColor), 0, 3); grid.Controls.Add(TextLabel("Beyond middle"), 1, 3); grid.Controls.Add(OuterRate, 2, 3);
        add(grid); add(CustomRates); add(Cap); add(Horizontal);
        var axisHint = TextLabel("Direction also applies to fixed foveation. Fixed sizes and cap stay on the main page."); axisHint.Font = new Font(Font.FontFamily, 9); add(axisHint);
        add(DebugRings);
        var effect = Toggle("Show detail reduction in photo", "Show illustrative photo density reduction"); effect.Checked = true;
        effect.CheckedChanged += (_, _) => Preview.ShowEffect = effect.Checked; add(effect);
        var animation = Toggle("Animate illustrative gaze", "Animate the illustrative gaze preview"); animation.Checked = true;
        animation.CheckedChanged += (_, _) => Preview.Animate = animation.Checked; add(animation);
        Status.ForeColor = ModernUiTheme.KeyGlowBright; add(Status);
        var footer = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1, Padding = new Padding(0, 15, 0, 0) };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 104)); footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 112));
        footer.Controls.Add(TextLabel("Apply updates the page. Save the INI there, then restart Skyrim."), 0, 0);
        var cancel = new Button { Text = "Cancel", Dock = DockStyle.Fill, DialogResult = DialogResult.Cancel, FlatStyle = FlatStyle.Flat, BackColor = ModernUiTheme.SurfaceRaised };
        var apply = new Button { Text = "Apply", Dock = DockStyle.Fill, FlatStyle = FlatStyle.Flat, BackColor = ModernUiTheme.Accent, ForeColor = Color.White };
        apply.Click += (_, _) => AcceptChanges();
        footer.Controls.Add(cancel, 1, 0); footer.Controls.Add(apply, 2, 0); root.Controls.Add(footer, 0, 2);
        AcceptButton = apply; CancelButton = cancel;
        _tips.SetToolTip(Cap, "Caps every eye ring at half density without overwriting its requested rate. Selecting an eye preset turns this cap off.");
        _tips.SetToolTip(Inner, "Center boundary in normalized eye-texture units. This is not degrees or percent of image area.");
        _tips.SetToolTip(Mid, "Middle-ring outer boundary. It cannot be smaller than the center boundary.");
        _tips.SetToolTip(Horizontal, "Selects automatic half-rate direction and caps square coarse rates. This setting is shared with fixed VRS / fallback.");
        foreach (var combo in new[] { InnerRate, MidRate, OuterRate })
            _tips.SetToolTip(combo, "1x1: full density; 1x2/2x1: half; 2x2: quarter; 2x4/4x2: eighth; 4x4: sixteenth. VRS and Density Mask reconstruct differently.");
        Preset.SelectedIndexChanged += (_, _) => { if (!_loading) SetDraft(ReadDraft().WithPreset(Preset.SelectedIndex)); };
        foreach (var combo in new[] { Backend, InnerRate, MidRate, OuterRate }) combo.SelectedIndexChanged += (_, _) => Edited();
        foreach (var check in new[] { EyeEnabled, CustomRates, Cap, Horizontal, DebugRings }) check.CheckedChanged += (_, _) => Edited();
        Inner.ValueChanged += (_, _) => Edited(); Mid.ValueChanged += (_, _) => Edited();
        SetDraft(settings);
    }
    internal EyeFoveationSettings ReadDraft() => new() { Enabled = EyeEnabled.Checked, Backend = Math.Max(0, Backend.SelectedIndex),
        DebugRings = DebugRings.Checked, Radii = new(Inner.Value, Mid.Value), CustomRates = CustomRates.Checked,
        Compatibility = Cap.Checked, FavorHorizontal = Horizontal.Checked, InnerRate = InnerRate.SelectedItem?.ToString() ?? EyeFoveationSettings.DefaultInnerRate,
        MidRate = MidRate.SelectedItem?.ToString() ?? EyeFoveationSettings.DefaultMidRate,
        OuterRate = OuterRate.SelectedItem?.ToString() ?? EyeFoveationSettings.DefaultOuterRate };
    internal void SetDraft(EyeFoveationSettings settings)
    {
        _loading = true;
        try {
            EyeEnabled.Checked = settings.Enabled; Backend.SelectedIndex = Math.Clamp(settings.Backend, 0, 3);
            DebugRings.Checked = settings.DebugRings; Inner.Value = settings.Radii.Inner; Mid.Value = settings.Radii.Mid;
            CustomRates.Checked = settings.CustomRates; Cap.Checked = settings.Compatibility; Horizontal.Checked = settings.FavorHorizontal;
            InnerRate.SelectedItem = settings.InnerRate; MidRate.SelectedItem = settings.MidRate; OuterRate.SelectedItem = settings.OuterRate;
            Preset.SelectedIndex = settings.PresetIndex;
        } finally { _loading = false; }
        Edited();
    }
    private void Edited()
    {
        if (_loading) return;
        _loading = true;
        try {
            if (Mid.Value < Inner.Value) Mid.Value = Inner.Value;
            var settings = ReadDraft(); Preset.SelectedIndex = settings.PresetIndex;
            InnerRate.Enabled = MidRate.Enabled = OuterRate.Enabled = settings.CustomRates && settings.Backend != 3;
            Preview.Settings = settings;
            Status.Text = settings.Backend == 3 ? "Effects-only backend: shading rates do not apply. A matching shader build is required."
                : !settings.Enabled ? "Eye tracking is disabled. The preview shows the configured eye profile."
                : settings.Compatibility ? "Half-rate cap ON · requested coarse rates are reduced in every ring."
                : "Eye cap OFF · requested rates apply on eligible scene passes.";
            if (!settings.CustomRates && !settings.Compatibility && settings.Backend is 0 or 2)
                Status.Text += " Legacy Density Mask can reduce detail more at the far edge. Choose each ring's rate for explicit three-ring control.";
        } finally { _loading = false; }
    }
    internal void AcceptChanges() { AcceptedSettings = ReadDraft(); DialogResult = DialogResult.OK; Close(); }
    private static Label TextLabel(string text, Color? color = null) => new() { Text = text, AutoSize = true, Dock = DockStyle.Top,
        ForeColor = color ?? ModernUiTheme.TextSecondary, Margin = new Padding(0, 3, 6, 6), UseMnemonic = false };
    private static CheckBox Toggle(string text, string accessible) => new ModernCheckBox { Text = text, AccessibleName = accessible,
        AutoSize = true, Dock = DockStyle.Top, ForeColor = ModernUiTheme.TextPrimary, UseVisualStyleBackColor = false };
    private static ComboBox Choice(string[] values, string accessible)
    {
        var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top,
            AccessibleName = accessible, BackColor = ModernUiTheme.Input, ForeColor = ModernUiTheme.TextPrimary,
            DrawMode = DrawMode.OwnerDrawFixed, FlatStyle = FlatStyle.Flat, Margin = new Padding(0, 3, 6, 7) };
        box.FontChanged += (_, _) => box.ItemHeight = box.Font.Height + 4;
        box.DrawItem += (_, e) => {
            using var background = new SolidBrush((e.State & DrawItemState.Selected) != 0 ? ModernUiTheme.SurfaceHover : ModernUiTheme.Input);
            e.Graphics.FillRectangle(background, e.Bounds);
            if (e.Index >= 0) TextRenderer.DrawText(e.Graphics, box.Items[e.Index]?.ToString() ?? "", e.Font, Rectangle.Inflate(e.Bounds, -4, 0),
                box.Enabled ? ModernUiTheme.TextPrimary : ModernUiTheme.TextMuted, TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
            e.DrawFocusRectangle();
        };
        box.Items.AddRange(values); return box;
    }
    private static NumericUpDown Number(decimal maximum, string accessible) => new() { DecimalPlaces = 2, Increment = .01m,
        Minimum = .10m, Maximum = maximum, Dock = DockStyle.Top, AccessibleName = accessible,
        BackColor = ModernUiTheme.Input, ForeColor = ModernUiTheme.TextPrimary, Margin = new Padding(0, 3, 8, 7) };
    protected override void Dispose(bool disposing) { if (disposing) _tips.Dispose(); base.Dispose(disposing); }
}
