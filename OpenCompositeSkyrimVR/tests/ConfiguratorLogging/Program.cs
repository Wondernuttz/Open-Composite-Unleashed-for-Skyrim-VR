using System;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;
using OpenCompositeConfigurator.BodyTracking;

static class Program
{
    const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    static T Field<T>(MainForm form, string name) => (T)typeof(MainForm).GetField(name, Private)!.GetValue(form)!;
    static void Call(MainForm form, string name, params object[] args) => typeof(MainForm).GetMethod(name, Private)!.Invoke(form, args);
    static void Check(bool ok, string why) { if (!ok) throw new Exception(why); }

    [STAThread]
    static int Main(string[] args)
    {
        try {
            string output = Path.GetFullPath(args[0]);
            var bodyFailures = ContinuousBodyPoseSelfTests.RunAll();
            Check(bodyFailures.Count == 0, string.Join("; ", bodyFailures));
            World3DLegMotionClassifierSelfTests.AssertAll();
            World3DCalibrationRecorderSelfTests.AssertAll();
            Console.WriteLine("PASS: retained camera pose, gait and calibration-recorder self-tests.");
            Directory.CreateDirectory(output);
            Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
            Application.EnableVisualStyles();
            // Never Show(): avoids runtime installation, user prompts and game-root writes.
            using var form = new MainForm();
            var ini = Field<IniFile>(form, "_ini");
            var checkbox = Field<CheckBox>(form, "_chkDiagnosticLogging");
            var panel = Field<Panel>(form, "_tabSteamHelp");
            Check(checkbox.Parent == panel && checkbox.Text == "Turn on logging", "Wrong tab/label");
            Check(checkbox.GetType() == Field<CheckBox>(form, "_chkAudioSwitch").GetType(), "Checkbox theme differs");
            foreach (string section in new[] { "", "[]\n", "[default]\n", "[debug]\n", "[general]\n" }) {
                foreach (string level in new[] { "normal", "DEBUG", "invalid" }) {
                    string path = Path.Combine(output, "logging-fixture.ini");
                    File.WriteAllText(path, section + "logLevel=" + level + "\n");
                    ini.Load(path);
                    Call(form, "ReadFromIni");
                    Check(checkbox.Checked == (level == "DEBUG"), "Logging read mismatch");
                    foreach (bool enabled in new[] { true, false }) {
                        checkbox.Checked = enabled;
                        Call(form, "WriteToIni", false);
                        Check(ini.Get("", "logLevel", "") == (enabled ? "debug" : "normal"), "UI write mismatch");
                        ini.Save(path); ini.Load(path);
                        Call(form, "ReadFromIni");
                        Check(checkbox.Checked == enabled, "Round-trip mismatch");
                    }
                }
            }
            ini.Reset(); Call(form, "ReadFromIni");
            Check(!checkbox.Checked, "Missing key must default off");
            Call(form, "SwitchTab", 4);
            Check(checkbox.Bottom <= panel.Height, "Logging checkbox clipped");
            // Detach from the never-shown form so DrawToBitmap paints children.
            panel.Parent = null;
            panel.Visible = true;
            panel.CreateControl();
            foreach (bool enabled in new[] { false, true }) {
                checkbox.Checked = enabled;
                using var bitmap = new Bitmap(panel.Width, panel.Height);
                panel.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
                bitmap.Save(Path.Combine(output, enabled ? "logging-on.png" : "logging-off.png"));
            }
            Console.WriteLine("PASS: actual themed Help checkbox, 5 INI layouts, on/off saves, invalid/missing defaults; screenshots rendered without opening UI or installing files.");
            return 0;
        } catch (Exception e) { Console.Error.WriteLine(e); return 1; }
    }
}
