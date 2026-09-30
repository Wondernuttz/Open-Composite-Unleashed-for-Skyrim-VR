using System;
using System.Drawing;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Windows.Forms;

[assembly: InternalsVisibleTo("IndexGripConfiguratorTests")]

namespace OpenCompositeConfigurator;

internal sealed class IndexGripControl : UserControl
{
    internal ComboBox Mode { get; } = new() { DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat };
    internal NumericUpDown Grab { get; } = new() { DecimalPlaces = 2, Increment = .01m, Minimum = .02m, Maximum = 1m, Value = .50m };
    internal NumericUpDown Release { get; } = new() { DecimalPlaces = 2, Increment = .01m, Minimum = .01m, Maximum = .49m, Value = .25m };
    private readonly Label grabLabel = new ModernLabel() { Text = "Grab threshold:" };
    private readonly Label releaseLabel = new ModernLabel() { Text = "Release threshold:" };
    private readonly ToolTip tips = new();
    private readonly Label help = new();
    private bool indexSelected = true;

    internal IndexGripControl()
    {
        Size = new Size(365, 151);
        BackColor = Color.Transparent;
        Font = new Font("Segoe UI", 9f);
        var title = new Label { Text = "Index grip holding", Location = new Point(0, 0), Size = new Size(130, 24) };
        Mode.Items.AddRange(new object[] { "Runtime default", "Custom sensitivity" });
        Mode.SetBounds(135, 0, 218, 25);
        grabLabel.SetBounds(0, 35, 130, 24);
        releaseLabel.SetBounds(0, 66, 130, 24);
        Grab.SetBounds(135, 32, 80, 25);
        Release.SetBounds(135, 63, 80, 25);
        help.Location = new Point(0, 94);
        help.Size = new Size(360, 54);
        help.Font = new Font("Segoe UI", 8.5f);
        Controls.AddRange(new Control[] { title, Mode, grabLabel, Grab, releaseLabel, Release, help });
        tips.SetToolTip(Mode, "Runtime default keeps the runtime grip thresholds. Custom sensitivity applies only when the game detects physical Index controllers, with any headset.");
        tips.SetToolTip(Grab, "Grip value needed to begin holding (0.02–1.00). Lower is easier to activate. These are tuning values, not factory calibration.");
        tips.SetToolTip(Release, "Release when the grip value falls below this level. Lower is easier to keep holding. Must stay below Grab. Brief grip dropouts are filtered for 20 ms. A sustained release is confirmed on the next input update after that. Confirmed disconnection or loss of game input focus releases immediately.");
        Grab.ValueChanged += (_, _) => Release.Maximum = Grab.Value - .01m;
        Mode.SelectedIndexChanged += (_, _) => UpdateEnabled();
        Mode.SelectedIndex = 0;
    }

    private void UpdateEnabled()
    {
        Mode.Enabled = indexSelected;
        bool custom = indexSelected && Mode.SelectedIndex == 1;
        Grab.Enabled = Release.Enabled = grabLabel.Enabled = releaseLabel.Enabled = custom;
        help.Text = indexSelected
            ? "For HIGGS Auto/Touch on Index controllers. Lower release keeps a relaxed grip held. Squeeze-button bindings are separate. Save settings and restart Skyrim."
            : "Select Index controllers on the Bindings page to adjust grip holding. Saved sensitivity values are retained. In-game tuning applies only to physical Index controllers.";
    }
    internal void SetIndexSelected(bool selected)
    {
        indexSelected = selected;
        UpdateEnabled();
    }
    internal void ResetSettings()
    {
        Grab.Value = .50m;
        Release.Value = .25m;
        Mode.SelectedIndex = 0;
        UpdateEnabled();
    }
    private static decimal Read(IniFile ini, string key, decimal fallback, decimal min, decimal max)
        => decimal.TryParse(ini.Get("", key, fallback.ToString(CultureInfo.InvariantCulture)), NumberStyles.Float,
            CultureInfo.InvariantCulture, out var value) ? Math.Clamp(value, min, max) : fallback;
    internal void LoadSettings(IniFile ini)
    {
        Grab.Value = Read(ini, "indexGripGrabThreshold", .50m, .02m, 1m);
        Release.Value = Math.Clamp(Read(ini, "indexGripReleaseThreshold", .25m, .01m, .99m), .01m, Release.Maximum);
        string custom = ini.Get("", "indexGripCustom", "false");
        Mode.SelectedIndex = custom.Equals("true", StringComparison.OrdinalIgnoreCase)
            || custom.Equals("on", StringComparison.OrdinalIgnoreCase)
            || custom.Equals("enabled", StringComparison.OrdinalIgnoreCase) ? 1 : 0;
        UpdateEnabled();
    }
    internal void SaveSettings(IniFile ini)
    {
        ini.Set("", "indexGripCustom", Mode.SelectedIndex == 1 ? "true" : "false");
        ini.Set("", "indexGripGrabThreshold", Grab.Value.ToString("0.00", CultureInfo.InvariantCulture));
        ini.Set("", "indexGripReleaseThreshold", Release.Value.ToString("0.00", CultureInfo.InvariantCulture));
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) tips.Dispose();
        base.Dispose(disposing);
    }
}
