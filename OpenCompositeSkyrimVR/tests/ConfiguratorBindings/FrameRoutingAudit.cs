using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    static int AuditFrameRouting()
    {
        string root = AppContext.BaseDirectory;
        Directory.CreateDirectory(Path.Combine(root, "root"));
        Directory.CreateDirectory(Path.Combine(root, "interface", "controls", "pc"));
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        using var form = new MainForm();
        Call(form, "ApplyControllerModel", "frame");
        string path = (string)Call(form, "GetControlmapSavePath")!;
        object[] apply = { "OpenCompositeConfigurator.controlmapvr_template.txt", null!, "" };
        Check((bool)Call(form, "ApplyControllerPresetMergingKeyboard", apply)!, "Frame baseline failed");
        string baseline = File.ReadAllText(path);
        // Independent expected mapping, from physical Frame controls to Skyrim's
        // Touch column and OpenVR ID. Do not derive this from the code under test.
        var buttons = new (string id, int column, int key)[] {
            ("left_stick",7,32), ("x_button",7,7), ("y_button",7,1),
            ("frame_dpad_left",7,5), ("frame_dpad_right",7,6), ("frame_view",7,35),
            ("l_trigger",7,33), ("frame_l_bumper",7,3), ("l_grip",7,2),
            ("right_stick",6,32), ("a_button",6,7), ("b_button",6,1),
            ("frame_x",6,5), ("frame_y",6,6), ("frame_menu",6,35),
            ("r_trigger",6,33), ("frame_r_bumper",6,3), ("r_grip",6,2)
        };
        bool Has(string value, int key) => value.Split(',').Any(token =>
            token.Trim().StartsWith("0x") && int.TryParse(token.Trim()[2..],
                System.Globalization.NumberStyles.HexNumber, null, out int parsed) && parsed == key);
        var names = Field<List<string>>(form, "_contextNames").ToArray();
        int cases = 0;
        foreach (string context in names)
        foreach (var button in buttons)
        {
            File.WriteAllText(path, baseline);
            Call(form, "TryLoadControlmapVR");
            var contexts = Field<Dictionary<string,List<string[]>>>(form, "_contextBindings");
            if (!contexts.TryGetValue(context, out var rows) || rows.Count == 0) continue;
            Field<ComboBox>(form, "_cmbCtrlType").SelectedIndex = Array.IndexOf(names, context);
            Set(form, "_selectedCtrlButton", button.id);
            Call(form, "RefreshSelectedControllerBinding", false);
            var picker = Field<ComboBox>(form, "_cmbCtrlAction");
            Check(picker.Enabled, "Button not editable: " + button.id + "/" + context);
            var choices = picker.Items.Cast<string>().Where(s => s != "(none)" && !s.StartsWith("Multiple: ")).Take(2).ToArray();
            if (choices.Length == 0) continue;
            int otherColumn = button.column == 6 ? 7 : 6;
            var otherHand = rows.ToDictionary(r => r[0], r => r[otherColumn]);
            var otherContexts = contexts.Where(c => c.Key != context).ToDictionary(c => c.Key,
                c => string.Join('\n', c.Value.Select(r => string.Join('\t', r))));
            foreach (string action in choices.Append("(none)"))
            {
                // Invoke the production change handler even when the first
                // selected action happened to match the preset already.
                Set(form, "_suppressCtrlActionChange", true);
                picker.SelectedItem = action;
                Set(form, "_suppressCtrlActionChange", false);
                Call(form, "CmbCtrlAction_SelectedIndexChanged", null!, EventArgs.Empty);
                Call(form, "SaveCurrentBindingEdits");
                Call(form, "TryLoadControlmapVR");
                contexts = Field<Dictionary<string,List<string[]>>>(form, "_contextBindings");
                rows = contexts[context];
                foreach (var row in rows)
                {
                    Check(Has(row[button.column], button.key) == (row[0] == action),
                        $"Wrong destination after save/reload: {context}/{button.id}/{action}/{row[0]}");
                    Check(row[otherColumn] == otherHand[row[0]], "Assignment changed the opposite hand");
                }
                foreach (var other in otherContexts)
                    Check(string.Join('\n', contexts[other.Key].Select(r => string.Join('\t', r))) == other.Value,
                        "Assignment changed unrelated context: " + other.Key);
                ++cases;
            }
        }
        // Stick direction markers describe analog movement, not independent
        // digital buttons; they must not pretend to accept an action assignment.
        foreach (string side in new[] { "left", "right" })
        foreach (string direction in new[] { "up", "down", "left", "right" })
        {
            Set(form, "_selectedCtrlButton", side + "_stick_" + direction);
            Call(form, "RefreshSelectedControllerBinding", false);
            Check(!Field<ComboBox>(form, "_cmbCtrlAction").Enabled, "Analog marker exposed as a digital button");
        }
        Console.WriteLine($"PASS: all 18 Frame buttons across {names.Length} contexts; {cases} assignment/reassignment/unbind round trips; opposite-hand and unrelated-context isolation; 8 analog markers");
        return 0;
    }
}
