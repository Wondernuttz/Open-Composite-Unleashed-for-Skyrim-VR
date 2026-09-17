using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    static int AuditControllerSave()
    {
        // Real form/save/reload operations; all files belong to this test executable.
        string root = AppContext.BaseDirectory;
        Directory.CreateDirectory(Path.Combine(root, "root"));
        Directory.CreateDirectory(Path.Combine(root, "interface", "controls", "pc"));
        string path = Path.Combine(root, "interface", "controls", "pc", "controlmapvr.txt");
        using (var stream = typeof(MainForm).Assembly.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_template.txt")!)
        using (var reader = new StreamReader(stream)) File.WriteAllText(path, reader.ReadToEnd());
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        string[] Row(MainForm f, string context, string action) =>
            Field<Dictionary<string, List<string[]>>>(f, "_contextBindings")[context].Single(row => row[0] == action);
        void Context(MainForm f, string context) => Field<ComboBox>(f, "_cmbCtrlType").SelectedIndex = Field<List<string>>(f, "_contextNames").IndexOf(context);
        void Select(MainForm f, string context, string button, string action)
        {
            Context(f, context);
            Set(f, "_selectedCtrlButton", button);
            Call(f, "RefreshSelectedControllerBinding", true);
            var picker = Field<ComboBox>(f, "_cmbCtrlAction");
            Check(picker.Enabled && picker.Items.Contains(action), "Action unavailable: " + action);
            picker.SelectedItem = action;
        }
        using (var form = new MainForm())
        {
            Field<IniFile>(form, "_ini").Reset();
            Field<CheckBox>(form, "_chkShortcutEnabled").Checked = false;
            Field<ComboBox>(form, "_cmbKeyboardDesign").SelectedIndex = -1;
            Field<CheckBox>(form, "_chkDisableMouse").Checked = false;
            Field<CheckBox>(form, "_chkVRIKKnuckles").Checked = false;
            Field<CheckBox>(form, "_chkDisableTrackpad").Checked = false;
            Call(form, "ApplyControllerModel", "knuckles");
            Call(form, "TryLoadControlmapVR");
            Call(form, "CaptureSavedState", true);
            Call(form, "MarkDirty");
            Check(!Field<bool>(form, "_dirty"), "Fixture starts dirty");
            Context(form, "Main Gameplay");
            Set(form, "_selectedCtrlButton", "l_trackpad_lower");
            Call(form, "RefreshSelectedControllerBinding", true);
            var picker = Field<ComboBox>(form, "_cmbCtrlAction");
            Check(!picker.Items.Contains("XButton"), "Gameplay offers unsupported XButton");
            var changes = Field<Dictionary<string, Dictionary<string, Dictionary<int, string>>>>(form, "_controllerChanges");
            // A stale/programmatic selection must also be rejected before clearing old bindings.
            string before = string.Join("|", Row(form, "Main Gameplay", "Tween Menu"));
            picker.Items.Add("XButton"); picker.SelectedItem = "XButton";
            Check(changes.Count == 0 && Field<int>(form, "_indexTrackpadCustomRegions") == 0, "Invalid selection mutated bindings/routing");
            Check(before == string.Join("|", Row(form, "Main Gameplay", "Tween Menu")), "Invalid choice cleared gameplay button");
            Select(form, "Item Menus", "l_trackpad_lower", "XButton");
            Check(Field<bool>(form, "_dirty"), "Pending controller edit did not trigger close warning");
            Check(Field<Label>(form, "_lblUnsavedBanner").Text.Contains("controller bindings"), "Unsaved warning omits bindings");
            Call(form, "BtnSave_Click", form, EventArgs.Empty);
            Check(Field<Label>(form, "_lblStatus").Text.StartsWith("Saved to"), "General save failed");
            Check(changes.Count == 0 && !Field<bool>(form, "_dirty"), "General save leaves pending binding edits");
            Select(form, "Item Menus", "l_trackpad_lower", "YButton");
            File.SetAttributes(path, FileAttributes.ReadOnly);
            try
            {
                Call(form, "BtnSave_Click", form, EventArgs.Empty);
                Check(Field<Label>(form, "_lblStatus").Text.StartsWith("Save failed:"), "Write failure reported as saved");
                Check(changes.Count > 0 && Field<bool>(form, "_dirty"), "Write failure discarded pending edits");
            }
            finally { File.SetAttributes(path, FileAttributes.Normal); }
            Select(form, "Item Menus", "l_trackpad_lower", "XButton");
            Call(form, "BtnSave_Click", form, EventArgs.Empty);
            Check(changes.Count == 0, "Retry did not save pending edits");
        }
        using (var form = new MainForm())
        {
            Call(form, "TryLoadControlmapVR");
            Check(Field<int>(form, "_indexTrackpadCustomRegions") == 2, "Fresh form lost left-lower routing");
            foreach (int column in new[] { 5, 7, 9 })
                Check(Row(form, "Item Menus", "XButton")[column].Split(',').Contains("0x06"), "General save lost pad in column " + column);
            string untouchedGameplay = string.Join("|", Row(form, "Main Gameplay", "Tween Menu"));
            string untouchedCharge = string.Join("|", Row(form, "Inventory", "ChargeItem"));
            string[] priorDrop = (string[])Row(form, "Item Menus", "XButton").Clone();
            Call(form, "BindInventoryDropToLeftFaceButton");
            foreach (int column in new[] { 5, 7, 9 })
            {
                Check(Row(form, "Item Menus", "XButton")[column].Split(',').Contains("0x07"), "Drop repair missed left A/X");
                Check(Row(form, "Item Menus", "XButton")[column].Split(',').Contains("0x06"), "Drop repair lost custom pad");
            }
            Check(Row(form, "Item Menus", "XButton")[7].Contains("0x01"), "Drop repair lost old B binding");
            foreach (int column in new[] { 1,2,3,4,6,8 })
                Check(Row(form, "Item Menus", "XButton")[column] == priorDrop[column], "Drop repair touched another input column");
            Check(untouchedGameplay == string.Join("|", Row(form, "Main Gameplay", "Tween Menu")), "Drop repair changed gameplay");
            Check(untouchedCharge == string.Join("|", Row(form, "Inventory", "ChargeItem")), "Drop repair changed charging");
            Call(form, "SaveCurrentBindingEdits");
            Call(form, "TryLoadControlmapVR");
            Check(Row(form, "Item Menus", "XButton")[7].Split(',').Contains("0x07"), "Drop repair lost after reload");
            var panel = Field<Panel>(form, "_tabKeyboard");
            Call(form, "ApplyControllerModel", "knuckles");
            using var host = new Form { ClientSize = panel.Size, Opacity = 0, AutoScaleMode = AutoScaleMode.None };
            panel.Location = System.Drawing.Point.Empty;
            host.Controls.Add(panel);
            panel.Visible = true;
            host.Show(); Application.DoEvents(); host.PerformLayout();
            using var bitmap = new System.Drawing.Bitmap(panel.Width, panel.Height);
            panel.DrawToBitmap(bitmap, panel.ClientRectangle);
            bitmap.Save(Path.Combine(root, "controller-save-drop.png"));
            host.Close();
        }
        Console.WriteLine("PASS: context-safe choices, invalid selection preserves bindings, pending-edit warning, general-save/fresh-instance pad round trip, explicit Drop repair preserves existing buttons and other contexts.");
        return 0;
    }
}
