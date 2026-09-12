using System;
using System.Collections.Generic;
using System.Globalization;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static class Program
{
    const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    static T Field<T>(MainForm form, string name) => (T)typeof(MainForm).GetField(name, Private)!.GetValue(form)!;
    static void Call(MainForm form, string name, params object[] args) => typeof(MainForm).GetMethod(name, Private)!.Invoke(form, args);
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
    static readonly Dictionary<string, decimal> Defaults = new() {
        ["WarpStrength"] = 1m, ["RotationScale"] = 1m, ["TranslationScale"] = 0m,
        ["LocoScale"] = 1m, ["DepthScale"] = 1m, ["AutoEngageFps"] = 50m
    };
    static void CheckDefaults(MainForm form)
    {
        foreach (var item in Defaults)
            Check(Field<NumericUpDown>(form, "_nudAsw" + item.Key).Value == item.Value,
                "Wrong DAPA default: " + item.Key);
        Check(!Field<CheckBox>(form, "_chkAswAutoNative").Checked, "Default must select manual DAPA");
        Check(!Field<CheckBox>(form, "_chkAswDebugMode").Checked, "Synthetic tint must default off");
        Check(Field<float>(form, "_aswNearFadeDepth") == 0f && Field<float>(form, "_aswEdgeFadeWidth") == 3f,
            "Hidden DAPA controls did not reset");
    }

    [STAThread]
    static int Main(string[] args)
    {
        try {
            var output = Path.GetFullPath(args[0]);
            Directory.CreateDirectory(output);
            Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
            Application.EnableVisualStyles();
            // Never show the form: no startup deployment, dialogs or live files.
            using var form = new MainForm();
            var ini = Field<IniFile>(form, "_ini");
            var path = Path.Combine(output, "dapa-fixture.ini");
            foreach (var section in new[] { "", "[default]\n", "[general]\n", "[asw]\n" }) {
                // Opening a minimal file then enabling DAPA must use the tested
                // baseline without requiring the user to visit Advanced first.
                File.WriteAllText(path, section + "aswEnabled=true\n");
                ini.Load(path); Call(form, "ReadFromIni");
                CheckDefaults(form);
                Check(Field<CheckBox>(form, "_chkAswEnabled").Checked, "Explicit enable was lost");
                Call(form, "WriteToIni", false); ini.Save(path); ini.Load(path); Call(form, "ReadFromIni");
                CheckDefaults(form);

                // Existing explicit tuning is not mistaken for a missing key.
                File.WriteAllText(path, section + "aswEnabled=true\naswLocoScale=0.35\naswRotationScale=0.45\n" +
                    "aswTranslationScale=0.25\naswAutoNative=true\naswDebugMode=10\naswNearFadeDepth=0.4\naswEdgeFadeWidth=2\n");
                ini.Load(path); Call(form, "ReadFromIni");
                Call(form, "WriteToIni", false); ini.Save(path); ini.Load(path); Call(form, "ReadFromIni");
                Check(Field<NumericUpDown>(form, "_nudAswLocoScale").Value == 0.35m, "Custom LOCO lost on save");
                Check(Field<NumericUpDown>(form, "_nudAswRotationScale").Value == 0.45m, "Custom rotation lost");
                Check(Field<NumericUpDown>(form, "_nudAswTranslationScale").Value == 0.25m, "Custom translation lost");
                Check(Field<CheckBox>(form, "_chkAswAutoNative").Checked && Field<CheckBox>(form, "_chkAswDebugMode").Checked,
                    "Explicit mode/debug setting lost");

                // Master Reset clears the INI then reloads. Old control values
                // must not survive this operation.
                ini.Reset(); Call(form, "ReadFromIni"); CheckDefaults(form);
                Check(!Field<CheckBox>(form, "_chkAswEnabled").Checked, "Reset must preserve opt-in enable policy");
                foreach (string invalid in new[] { "", "garbage", "NaN", "Infinity", "1e100" }) {
                    File.WriteAllText(path, section + string.Join("\n", Array.ConvertAll(
                        new[] { "WarpStrength", "RotationScale", "TranslationScale", "LocoScale", "DepthScale", "AutoEngageFps", "NearFadeDepth", "EdgeFadeWidth" },
                        key => "asw" + key + "=" + invalid)) + "\n");
                    foreach (var item in Defaults) Field<NumericUpDown>(form, "_nudAsw" + item.Key).Value = 2m <
                        Field<NumericUpDown>(form, "_nudAsw" + item.Key).Minimum ? 40m : 2m;
                    ini.Load(path); Call(form, "ReadFromIni"); CheckDefaults(form);
                }
            }
            var backends = new[] { "auto", "vrs", "rdm", "effects" };
            for (int i = 0; i < backends.Length; ++i) {
                File.WriteAllText(path, "foveatedBackend=" + backends[i] + "\naswLocoScale=0.75\n");
                ini.Load(path); Call(form, "ReadFromIni");
                Check(Field<ComboBox>(form, "_cboFoveatedBackend").SelectedIndex == i, "Backend selection lost: " + backends[i]);
                Call(form, "WriteToIni", false); ini.Save(path); ini.Load(path); Call(form, "ReadFromIni");
                Check(ini.Get("", "foveatedBackend", "") == backends[i], "Backend changed on save: " + backends[i]);
                Check(Field<NumericUpDown>(form, "_nudAswLocoScale").Value == 0.75m, "Backend save changed DAPA tuning");
            }
            var rings = Field<CheckBox>(form, "_chkFoveationDebugRings");
            Check(!rings.Checked, "Missing ring setting must default off");
            foreach (bool enabled in new[] { true, false }) {
                rings.Checked = enabled;
                Call(form, "WriteToIni", false); ini.Save(path);
                Check(ini.Get("vrs", "foveationDebugRings", "") == (enabled ? "true" : "false"),
                    "Ring toggle must save in the runtime's vrs section");
                rings.Checked = !enabled;
                ini.Load(path); Call(form, "ReadFromIni");
                Check(rings.Checked == enabled, "Ring setting did not survive reload");
                Check(Field<NumericUpDown>(form, "_nudAswLocoScale").Value == 0.75m,
                    "Ring toggle changed existing DAPA tuning");
            }
            var previousCulture = CultureInfo.CurrentCulture;
            try {
                CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("de-DE");
                File.WriteAllText(path, "aswLocoScale=0,75\naswTranslationScale=99\naswRotationScale=-1\n");
                ini.Load(path); Call(form, "ReadFromIni");
                Check(Field<NumericUpDown>(form, "_nudAswLocoScale").Value == 0.75m, "Runtime-supported comma decimal lost");
                Check(Field<NumericUpDown>(form, "_nudAswTranslationScale").Value == 3m &&
                    Field<NumericUpDown>(form, "_nudAswRotationScale").Value == 0m, "DAPA bounds not applied");
                Call(form, "WriteToIni", false); ini.Save(path);
                Check(ini.Get("", "aswLocoScale", "") == "0.75", "Saved decimal is locale-dependent");
            } finally { CultureInfo.CurrentCulture = previousCulture; }
            ini.Reset(); Call(form, "ReadFromIni"); CheckDefaults(form);
            var panel = (Panel)Field<NumericUpDown>(form, "_nudAswLocoScale").Parent!;
            panel.Parent = null; panel.Visible = true; panel.CreateControl();
            using (var preview = new Bitmap(panel.Width, panel.Height)) {
                panel.DrawToBitmap(preview, new Rectangle(Point.Empty, preview.Size));
                preview.Save(Path.Combine(output, "dapa-defaults.png"));
            }
            Console.WriteLine("PASS: actual Configurator DAPA defaults, enable/save/reload, eye-debug ring toggle roundtrip, four INI layouts, explicit tuning, reset, invalid/non-finite values, decimal locale and bounds.");
            return 0;
        } catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
