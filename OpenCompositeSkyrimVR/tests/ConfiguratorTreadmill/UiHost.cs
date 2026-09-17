using System.Drawing;
using System.Windows.Forms;

namespace OpenCompositeConfigurator;

// Compile and exercise the actual KAT page without loading the full Configurator,
// a user's INI, a native SDK, or any tracker/runtime integrations.
public partial class MainForm : Form
{
    private readonly Panel _tabTreadmill = new() { Size = new Size(1150, 800) };
    internal readonly Dictionary<(string, string), string> Values = new();
    private readonly TestIni _ini;

    internal MainForm(TreadmillReaderPreferences preferences, bool savedEnabled)
    {
        _ini = new(Values);
        Values[("", "treadmillEnabled")] = savedEnabled ? "true" : "false";
        _treadmillReaderPreferences = preferences;
        Controls.Add(_tabTreadmill);
        BuildTreadmillTab();
        LoadTreadmillSettings();
    }
    internal void SimulateShown() => OnShown(EventArgs.Empty);
    internal Form? ReaderDialog => _treadmillReaderDialog;
    internal string Connection => _lblTreadmillConnection.Text;
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    internal bool UnsavedEnabled { set => _chkTreadmillEnabled.Checked = value; }
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    internal bool CalibrationEnabled { get => _chkTreadmillControllerCalibration.Checked; set => _chkTreadmillControllerCalibration.Checked = value; }
    internal void SaveControls() => SaveTreadmillSettings();
    private static bool ParseBool(string value) => value == "true";
    private void BtnSave_Click(object? sender, EventArgs e) { }
    private void BtnReload_Click(object? sender, EventArgs e) { }
    private static Label MakeSectionLabel(string text, int x, int y) => new() { Text = text, Location = new(x, y) };
    private static Label MakeLabel(string text, int x, int y, int width) => new() { Text = text, Location = new(x, y), Width = width };
    private static CheckBox MakeCheckBox(string text, int x, int y) => new() { Text = text, Location = new(x, y) };
    private static Button MakeButton(string text, int x, int y, int w, int h) => new() { Text = text, Location = new(x, y), Size = new(w, h) };
}

internal sealed class TestIni(Dictionary<(string, string), string> values)
{
    public string Get(string section, string key, string fallback) => values.GetValueOrDefault((section, key), fallback);
    public void Set(string section, string key, string value) => values[(section, key)] = value;
}
internal sealed class BodySilhouettePanel : Panel
{
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    public bool ReferenceOnly { get; set; }
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    public bool RuntimeMonitor { get; set; }
    public void SetTreadmillConnected(bool connected) { }
    public void SetRuntimeTrackerState(uint valid, uint tracked) { }
}
internal static class BodyTrackerMonitor
{
    public static (uint Valid, uint Tracked, string Message) Read() => (0, 0, "Test fixture");
}
internal static class ModernUiTheme
{
    public static Color AccentText => Color.Lime;
    public static Color KeyGlowBright => Color.Lime;
    public static Color TextMuted => Color.Gray;
    public static Color TextSecondary => Color.White;
    public static Color TextPrimary => Color.White;
    public static Color SurfaceRaised => Color.Black;
    public static Color Accent => Color.Green;
    public static Color Window => Color.Black;
    public static void Apply(Control control, bool windowBands) { }
}
