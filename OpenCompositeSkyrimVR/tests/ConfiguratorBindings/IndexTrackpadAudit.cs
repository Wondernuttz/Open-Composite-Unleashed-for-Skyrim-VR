using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    static int AuditTrackpads()
    {
        // All persistence stays under this test executable; no form is shown.
        string root = AppContext.BaseDirectory;
        Directory.CreateDirectory(Path.Combine(root, "root"));
        Directory.CreateDirectory(Path.Combine(root, "interface", "controls", "pc"));
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        using var form = new MainForm();
        var ini = Field<IniFile>(form, "_ini");
        ini.Reset();
        Field<CheckBox>(form, "_chkDisableMouse").Checked = false;
        Field<CheckBox>(form, "_chkVRIKKnuckles").Checked = false;
        Field<CheckBox>(form, "_chkDisableTrackpad").Checked = false;
        Call(form, "ApplyControllerModel", "knuckles");
        Set(form, "_indexTrackpadCustomRegions", 0);
        var contexts = Field<Dictionary<string, List<string[]>>>(form, "_contextBindings");
        var changes = Field<Dictionary<string, Dictionary<string, Dictionary<int, string>>>>(form, "_controllerChanges");
        contexts.Clear(); changes.Clear();
        string[] Row(string name, string button) => new[] {
            name, "0xff", "0xff", "0xffff", button, button, button, button, button, button,
            "1", "1", "1", "1", "1", "1", "1", "1", "1" };
        var faceA = Row("Activate", "0x07");
        var faceB = Row("Tween Menu", "0x01");
        var actionNames = new[] { "Jump", "Sneak", "Shout", "Sprint" };
        var rows = new List<string[]> { faceA, faceB };
        rows.AddRange(actionNames.Select(name => Row(name, "0xff")));
        contexts["Main Gameplay"] = rows;
        contexts["Menu Mode"] = new() { Row("Accept", "0x07"), Row("Cancel", "0x01") };
        var names = Field<List<string>>(form, "_contextNames");
        names.Clear(); names.AddRange(new[] { "Main Gameplay", "Menu Mode" });
        Field<ComboBox>(form, "_cmbCtrlType").SelectedIndex = 0;
        string path = (string)Call(form, "GetControlmapSavePath")!;
        File.WriteAllText(path, "// Main Gameplay\n" + string.Join('\n', rows.Select(row => string.Join('\t', row)))
            + "\n\n// Menu Mode\n" + string.Join('\n', contexts["Menu Mode"].Select(row => string.Join('\t', row))) + "\n");
        var ids = new[] { "l_trackpad_upper", "l_trackpad_lower", "r_trackpad_upper", "r_trackpad_lower" };
        var picker = Field<ComboBox>(form, "_cmbCtrlAction");
        for (int region = 0; region < 4; ++region)
        {
            Set(form, "_selectedCtrlButton", ids[region]);
            Call(form, "RefreshSelectedControllerBinding", true);
            Check(picker.Enabled, ids[region] + " cannot be assigned");
            Set(form, "_suppressCtrlActionChange", true);
            picker.Items.Clear(); picker.Items.Add("(none)"); picker.Items.AddRange(actionNames);
            picker.SelectedIndex = 0;
            Set(form, "_suppressCtrlActionChange", false);
            picker.SelectedItem = actionNames[region];
            Check(Field<int>(form, "_indexTrackpadCustomRegions") == (1 << (region + 1)) - 1,
                "Customization changed another region's routing");
        }
        foreach (int column in new[] { 4, 5, 6, 7, 8, 9 })
            Check(faceA[column] == "0x07" && faceB[column] == "0x01", "Custom pad replaced face-button binding");
        for (int region = 0; region < 4; ++region)
        {
            string expected = region % 2 == 0 ? "0x05" : "0x06";
            bool left = region < 2;
            foreach (int column in new[] { 4, 5, 6, 7, 8, 9 })
                Check(rows[region + 2][column] == ((column % 2 == 1) == left ? expected : "0xff"),
                    ids[region] + " written to wrong hand/device/action");
        }
        Call(form, "SaveCurrentBindingEdits");
        Check(ini.Get("", "indexTrackpadCustomRegions", "") == "15", "Runtime INI lost right or left regions");
        Check(File.ReadAllText(path).Contains("// OCU IndexTrackpadCustomRegions=15"), "Controlmap lost routing metadata");
        Call(form, "TryLoadControlmapVR");
        Check(Field<int>(form, "_indexTrackpadCustomRegions") == 15, "Reload lost region mask");
        for (int region = 0; region < 4; ++region)
        {
            Set(form, "_selectedCtrlButton", ids[region]);
            var found = (List<string>)Call(form, "FindSelectedControllerActions")!;
            Check(found.SequenceEqual(new[] { actionNames[region] }), ids[region] + " reload changed its action");
        }
        Console.WriteLine("PASS: all four Index halves save/reload distinct actions across all six VR columns; both hands, face buttons, INI and preset metadata preserved.");
        return 0;
    }
}
