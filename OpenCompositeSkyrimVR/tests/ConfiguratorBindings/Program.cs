using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    static T Field<T>(MainForm form, string name) => (T)typeof(MainForm).GetField(name, Private)!.GetValue(form)!;
    static object? Call(MainForm form, string name, params object[] args) => typeof(MainForm).GetMethod(name, Private)!.Invoke(form, args);
    static void Set(MainForm form, string name, object value) => typeof(MainForm).GetField(name, Private)!.SetValue(form, value);
    static void Check(bool value, string message) { if (!value) throw new Exception(message); }

    [STAThread]
    static int Main(string[] args)
    {
        try {
            if (args.Length > 0 && args[0] == "--layout") return AuditLayout();
            if (args.Length > 0 && args[0] == "--foveation") return AuditFoveation();
            if (args.Length > 0 && args[0] == "--trackpads") return AuditTrackpads();
            if (args.Length > 0 && args[0] == "--controller-save") return AuditControllerSave();
            // Fixtures stay beside this test executable; never show the form or install a runtime.
            var root = AppContext.BaseDirectory;
            Directory.CreateDirectory(Path.Combine(root, "root"));
            Directory.CreateDirectory(Path.Combine(root, "interface", "controls", "pc"));
            Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
            Application.EnableVisualStyles();
            using var form = new MainForm();
            var ini = Field<IniFile>(form, "_ini");
            ini.Reset();
            ini.Save(Path.Combine(root, "opencomposite.ini"));
            Field<CheckBox>(form, "_chkDisableMouse").Checked = false;
            var path = (string)Call(form, "GetControlmapSavePath")!;
            File.WriteAllText(path, "// Main Gameplay\r\nForward\t0x11\t0xff\t0xffff\t0xff\t0xff\t0x01\t0xff\t0xff\t0xff\t1\t1\t1\t1\t1\t1\t1\t1\t1\r\n");
            var changes = Field<Dictionary<string, Dictionary<string, Dictionary<int, string>>>>(form, "_controllerChanges");
            changes["Main Gameplay"] = new() { ["Forward"] = new() { [6] = "0x02" } };
            Call(form, "SaveCurrentBindingEdits");
            var fields = File.ReadAllLines(path).Single(line => line.StartsWith("Forward\t")).Split('\t');
            Check(fields[6] == "0x02", "Current controller edit was lost");
            Check(fields[2] == "0xff" && fields[3] == "0xffff", "Unrelated fields changed");
            Check(changes.Count == 0, "Saved edits remain pending");
            Check(Field<Button>(form, "_btnSaveAsBindingPreset").Text == "Save Custom…", "Unclear save label");
            Check(Field<Button>(form, "_btnApplyBindingPreset").Text == "Use Preset", "Unclear apply label");
            Check(Field<Button>(form, "_btnApplyBindingPreset").BackColor == System.Drawing.Color.FromArgb(27, 65, 57), "Use Preset must keep the final themed green action-button styling");
            Check(Field<Button>(form, "_btnApplyBindingPreset").BackColor == Field<Button>(form, "_btnSaveAsBindingPreset").BackColor, "Preset action buttons must match");
            Check(Field<Button>(form, "_btnApplyBindingPreset").FlatAppearance.MouseOverBackColor == Field<Button>(form, "_btnSaveAsBindingPreset").FlatAppearance.MouseOverBackColor, "Preset hover colors must match");
            Check(typeof(MainForm).GetMethod("BtnSaveAsBindingPreset_Click", Private) != null, "Custom save handler missing");
            Call(form, "ApplyControllerModel", "knuckles");
            var dots = Field<Dictionary<string, (string display, System.Drawing.PointF pos, bool isStickDir)>>(form, "_activeControllerButtons");
            Check(dots.Keys.Count(key => key.Contains("trackpad")) == 4, "Index needs upper/lower dots for each hand");
            var contexts = Field<Dictionary<string, List<string[]>>>(form, "_contextBindings");
            contexts.Clear();
            // This fixture replaces the loaded map, including its routing metadata.
            Set(form, "_indexTrackpadCustomRegions", 0);
            Set(form, "_savedIndexTrackpadCustomRegions", 0);
            string[] Row(string name, string left) => new[] { name, "0x11", "0xff", "0xffff", "0x07", left, "0x07", left, "0x07", left, "1", "1", "1", "1", "1", "1", "1", "1", "1" };
            var face = Row("Activate", "0x01");
            var menu = Row("Tween Menu", "0x02");
            var otherMenu = Row("Accept", "0x01");
            contexts["Main Gameplay"] = new() { face, menu };
            contexts["Menu Mode"] = new() { otherMenu };
            var names = Field<List<string>>(form, "_contextNames");
            names.Clear(); names.AddRange(new[] { "Main Gameplay", "Menu Mode" });
            var contextPicker = Field<ComboBox>(form, "_cmbCtrlType");
            contextPicker.SelectedIndex = 0;
            Set(form, "_selectedCtrlButton", "l_trackpad_upper");
            Field<CheckBox>(form, "_chkVRIKKnuckles").Checked = false;
            Field<CheckBox>(form, "_chkDisableTrackpad").Checked = false;
            Call(form, "RefreshSelectedControllerBinding", true);
            var actionPicker = Field<ComboBox>(form, "_cmbCtrlAction");
            Check(actionPicker.Enabled, "Trackpad assignments must be editable");
            Set(form, "_suppressCtrlActionChange", true);
            actionPicker.Items.Clear(); actionPicker.Items.AddRange(new object[] { "(none)", "Tween Menu", "Accept" });
            actionPicker.SelectedIndex = 0;
            Set(form, "_suppressCtrlActionChange", false);
            actionPicker.SelectedItem = "Tween Menu";
            foreach (int col in new[] { 5, 7, 9 }) {
                Check(face[col] == "0x01", "Custom pad changed a face button");
                Check(menu[col] == "0x02,0x05", "Menu assignment lost its existing binding or missed the independent pad id");
                Check(otherMenu[col] == "0x01,0x05", "Other menu context lost inherited behavior");
            }
            Check(menu[4] == "0x07" && menu[6] == "0x07" && menu[8] == "0x07", "Left pad customization changed right-hand columns");
            Check(Field<int>(form, "_indexTrackpadCustomRegions") == 1, "Only selected region should become independent");
            Field<CheckBox>(form, "_chkVRIKKnuckles").Checked = true;
            Check(!actionPicker.Enabled, "VRIK gesture override must be explicit");
            Field<CheckBox>(form, "_chkVRIKKnuckles").Checked = false;
            Check(actionPicker.Enabled && Field<int>(form, "_indexTrackpadCustomRegions") == 1, "Leaving VRIK mode lost saved regions");
            // Exercise real controlmap/INI save paths in the isolated fixture.
            File.WriteAllText(path, "// Main Gameplay\r\n" + string.Join('\t', Row("Activate", "0x01")) + "\r\n"
                + string.Join('\t', Row("Tween Menu", "0x02")) + "\r\n\r\n// Menu Mode\r\n" + string.Join('\t', Row("Accept", "0x01")) + "\r\n");
            Call(form, "SaveCurrentBindingEdits");
            string customPreset = File.ReadAllText(path);
            Check(customPreset.Contains("// OCU IndexTrackpadCustomRegions=1"), "Custom preset lost region metadata");
            Check(ini.Get("", "indexTrackpadCustomRegions", "") == "1", "Runtime routing was not saved");
            Call(form, "TryLoadControlmapVR");
            Check(Field<int>(form, "_indexTrackpadCustomRegions") == 1, "Reload lost independent region routing");
            Check(!names.Any(name => name.Contains("IndexTrackpadCustomRegions")), "Preset metadata leaked into context menu");
            File.WriteAllText(path, customPreset.Replace("// OCU IndexTrackpadCustomRegions=1", ""));
            Call(form, "TryLoadControlmapVR");
            Check(Field<int>(form, "_indexTrackpadCustomRegions") == 0, "Old presets must retain legacy routing");
            // Render the actual controller surface without showing or installing anything.
            var picture = Field<PictureBox>(form, "_picBindingsController");
            using var bitmap = new System.Drawing.Bitmap(picture.Width, picture.Height);
            picture.DrawToBitmap(bitmap, picture.ClientRectangle);
            bitmap.Save(Path.Combine(root, "index-trackpad-four-dots.png"));
            Console.WriteLine("PASS: four Index dots, independent menu assignments, face-button preservation, VRIK override, preset/INI round trip.");
            Console.WriteLine("PASS: current controller edits saved, unrelated mouse/gamepad fields preserved, pending edits cleared, simplified labels.");
            return 0;
        } catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    static int AuditFoveation()
    {
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        using var form = new MainForm();
        var fixedCap = Field<CheckBox>(form, "_chkVrsCompatibilityMode");
        var eyeCap = Field<CheckBox>(form, "_chkVrsEyeCompatibilityMode");
        var custom = Field<CheckBox>(form, "_chkVrsEyeCustomRates");
        var label = Field<Label>(form, "_lblVrsEffectiveRates");
        fixedCap.Checked = true; eyeCap.Checked = false; custom.Checked = false;
        Check(label.Text.Contains("outer 2x2"), "Uncapped moving profile must report the stronger outer ring");
        Check(fixedCap.Checked, "Eye setting changed fixed fallback");
        eyeCap.Checked = true;
        Check(!label.Text.Contains("outer 2x2"), "Capped moving profile readout is misleading");
        fixedCap.Checked = false;
        Check(eyeCap.Checked, "Fixed setting changed eye setting");
        custom.Checked = true; eyeCap.Checked = false;
        Field<ComboBox>(form, "_cboVrsEyeOuterRate").SelectedItem = "4x4";
        Check(label.Text.Contains("outer 4x4"), "Custom eye rate is not displayed");
        eyeCap.Checked = true;
        Check(!label.Text.Contains("outer 4x4"), "Compatibility cap is not displayed");
        var ini = Field<IniFile>(form, "_ini");
        ini.Reset(); ini.Set("", "vrsCompatibilityMode", "false");
        Call(form, "ReadFromIni");
        Check(!fixedCap.Checked && !eyeCap.Checked, "Legacy uncapped INI must remain uncapped");
        ini.Set("", "vrsCompatibilityMode", "true");
        ini.Set("", "vrsEyeCompatibilityMode", "false");
        ini.Set("", "vrsFixedInnerRadius", "0.73");
        Call(form, "ReadFromIni");
        Call(form, "WriteToIni", false);
        Check(ini.Get("", "vrsCompatibilityMode", "") == "true" && ini.Get("", "vrsEyeCompatibilityMode", "") == "false",
            "Independent caps must survive an INI round trip");
        Check(Field<NumericUpDown>(form, "_nudVrsInnerRadius").Value == .73m, "Eye test changed fixed radius");
        var video = Field<Panel>(form, "_tabVideo");
        var region = new System.Drawing.Rectangle(0, eyeCap.Top - 85, video.Width, 205);
        video.Parent = null; video.Visible = true; video.CreateControl();
        using var full = new System.Drawing.Bitmap(video.Width, video.Height);
        video.DrawToBitmap(full, video.ClientRectangle);
        using var crop = full.Clone(region, full.PixelFormat);
        crop.Save(Path.Combine(AppContext.BaseDirectory, "moving-foveation-controls.png"));
        Console.WriteLine("PASS: independent fixed/eye caps, effective default/custom rates, control render");
        return 0;
    }

    static int AuditLayout()
    {
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        string output = Path.Combine(AppContext.BaseDirectory, "layout-audit");
        Directory.CreateDirectory(output);
        foreach (int scale in new[] { 100, 125, 150 })
        foreach (var tab in new[] { (0, "_tabSettings"), (1, "_tabKeyboard"), (4, "_tabSteamHelp") })
        {
            using var form = new MainForm(); // Never Show: no installation/startup side effects.
            Call(form, "ApplyControllerModel", "knuckles");
            Call(form, "SwitchTab", tab.Item1);
            if (tab.Item1 == 1) {
                Set(form, "_selectedCtrlButton", "l_trackpad_upper");
                Call(form, "RefreshSelectedControllerBinding", true);
            }
            var panel = Field<Panel>(form, tab.Item2);
            int naturalHeight = panel.Height;
            // Program is DPI-unaware. Simulate a 1920x1080 monitor's logical
            // working area (40 physical pixels reserved for the taskbar).
            int logicalWindowHeight = 1040 * 100 / scale;
            int nonClient = form.Height - form.ClientSize.Height;
            int viewport = Math.Max(200, Math.Min(naturalHeight, logicalWindowHeight - nonClient - panel.Top - 40));
            panel.Parent = null;
            panel.Visible = true;
            panel.AutoScroll = true;
            panel.Height = viewport;
            panel.CreateControl();
            panel.PerformLayout();
            int maxRight = panel.Controls.Cast<Control>().Where(c => c.Visible).Max(c => c.Right);
            int maxBottom = panel.Controls.Cast<Control>().Where(c => c.Visible).Max(c => c.Bottom);
            Console.WriteLine($"LAYOUT {tab.Item2} scale={scale}% panel={panel.ClientSize} contentRight={maxRight} contentBottom={maxBottom} verticalScroll={panel.VerticalScroll.Visible} horizontalScroll={panel.HorizontalScroll.Visible}");
            foreach (Control control in panel.Controls) {
                if (!control.Visible) continue;
                if (control is CheckBox) {
                    var text = TextRenderer.MeasureText(control.Text, control.Font);
                    if (text.Width + 24 > control.Width)
                        Console.WriteLine($"TEXT-WIDTH {control.Text}: needs={text.Width + 24} available={control.Width}");
                }
            }
            if (tab.Item1 == 1) {
                var status = Field<Label>(form, "_lblKbStatus");
                var text = TextRenderer.MeasureText(status.Text, status.Font,
                    new System.Drawing.Size(status.Width, int.MaxValue), TextFormatFlags.WordBreak);
                Console.WriteLine($"STATUS measured={text} available={status.Size} text={status.Text}");
            }
            using var image = new System.Drawing.Bitmap(panel.Width, panel.Height);
            panel.DrawToBitmap(image, panel.ClientRectangle);
            image.Save(Path.Combine(output, $"{tab.Item2}-{scale}.png"));
            panel.Dispose();
        }
        Console.WriteLine("Layout audit renders: " + output);
        return 0;
    }
}
