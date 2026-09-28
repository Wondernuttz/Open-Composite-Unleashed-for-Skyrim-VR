using System;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static class Program
{
    static int checks;
    static void Check(bool ok, string message) { ++checks; if (!ok) throw new Exception(message); }
    [STAThread]
    static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        string output=Path.GetFullPath(args[0]); Directory.CreateDirectory(output);
        using var control=new IndexGripControl();
        Check(control.Mode.SelectedIndex==0 && !control.Grab.Enabled && !control.Release.Enabled,"runtime default must disable tuning");
        control.Mode.SelectedIndex=1;
        Check(control.Grab.Enabled && control.Release.Enabled,"custom mode must enable both values");
        control.Grab.Value=.20m;
        Check(control.Release.Value==.19m && control.Release.Maximum==.19m,"lowering grab must clamp release below it");
        control.Release.Value=.08m;
        var ini=new IniFile();
        ini.Set("", "aswEnabled", "true");
        ini.Set("", "enableVRIKKnucklesTrackPadSupport", "true");
        CultureInfo.CurrentCulture=new CultureInfo("de-DE");
        control.SaveSettings(ini);
        var path=Path.Combine(output,"settings.ini"); ini.Save(path);
        var loaded=new IniFile(); loaded.Load(path);
        using var restored=new IndexGripControl(); restored.LoadSettings(loaded);
        Check(restored.Mode.SelectedIndex==1 && restored.Grab.Value==.20m && restored.Release.Value==.08m,"custom settings round trip under comma-decimal culture");
        Check(loaded.Get("", "aswEnabled")=="true" && loaded.Get("", "enableVRIKKnucklesTrackPadSupport")=="true","grip save preserves unrelated settings");
        Check(File.ReadAllText(path).Contains("indexGripGrabThreshold=0.20") || File.ReadAllText(path).Contains("indexGripGrabThreshold = 0.20"),"INI uses invariant numeric syntax");
        restored.Mode.SelectedIndex=0; restored.SaveSettings(loaded); restored.LoadSettings(loaded);
        Check(!restored.Grab.Enabled && restored.Grab.Value==.20m && restored.Release.Value==.08m,"runtime mode preserves custom values while disabled");
        loaded.Set("", "indexGripGrabThreshold", "0.02"); loaded.Set("", "indexGripReleaseThreshold", "Infinity");
        restored.LoadSettings(loaded);
        Check(restored.Grab.Value==.02m && restored.Release.Value==.01m,"malformed settings clamp without throwing");
        restored.ResetSettings();
        Check(restored.Mode.SelectedIndex==0 && restored.Grab.Value==.50m && restored.Release.Value==.25m,"reset returns to runtime default");
        // Construct but never show MainForm: no OnShown deployment or live-file writes.
        using var main=new MainForm();
        const BindingFlags flags=BindingFlags.Instance|BindingFlags.NonPublic;
        var field=(IndexGripControl)typeof(MainForm).GetField("_indexGrip",flags)!.GetValue(main)!;
        var model=typeof(MainForm).GetMethod("ApplyControllerModel",flags)!;
        model.Invoke(main,new object[]{"knuckles"});
        field.Mode.SelectedIndex=1; field.Grab.Value=.35m; field.Release.Value=.15m;
        model.Invoke(main,new object[]{"psvr2"});
        Check(!field.Mode.Enabled && !field.Grab.Enabled && !field.Release.Enabled,"non-Index layout disables grip controls");
        model.Invoke(main,new object[]{"knuckles"});
        Check(field.Grab.Value==.35m && field.Release.Value==.15m && field.Mode.SelectedIndex==1,"layout switching retains custom values");
        // Render the production control in both modes for visual review.
        using var host=new Form { ClientSize=new Size(405,191), BackColor=ModernUiTheme.Surface, ShowInTaskbar=false, Opacity=0, FormBorderStyle=FormBorderStyle.None };
        host.Controls.Add(control); control.Location=new Point(20,20);
        ModernUiTheme.Apply(host,windowBands:false);
        host.Show(); host.PerformLayout();
        foreach(int mode in new[]{0,1}) {
            control.Mode.SelectedIndex=mode;
            using var bitmap=new Bitmap(host.ClientSize.Width,host.ClientSize.Height);
            host.DrawToBitmap(bitmap,new Rectangle(Point.Empty,bitmap.Size));
            bitmap.Save(Path.Combine(output,$"grip-{mode}.png"));
        }
        host.Controls.Remove(control);
        var panel=(Panel)typeof(MainForm).GetField("_pnlSkyrimOnly",flags)!.GetValue(main)!;
        host.Controls.Add(panel); panel.Location=Point.Empty; panel.Visible=true; host.ClientSize=panel.Size;
        model.Invoke(main,new object[]{"knuckles"});
        Check(field.Visible,"Index layout shows grip controls when its Settings panel is visible");
        model.Invoke(main,new object[]{"psvr2"});
        Check(field.Visible && !field.Mode.Enabled && !field.Grab.Enabled && !field.Release.Enabled,"Sense layout retains visible disabled grip controls");
        model.Invoke(main,new object[]{"knuckles"});
        Check(field.Visible && field.Mode.Enabled && field.Grab.Enabled && field.Release.Enabled,"Index custom mode re-enables retained grip controls");
        var choicePath=Path.Combine(AppContext.BaseDirectory,"ConfiguratorUI.json");
        byte[]? savedChoice=File.Exists(choicePath)?File.ReadAllBytes(choicePath):null;
        try {
            Check((bool)typeof(MainForm).GetMethod("SaveUiModelChoice",flags)!.Invoke(main,null)!,"controller selection save succeeds");
            using var reopened=new MainForm();
            var reopenedGrip=(IndexGripControl)typeof(MainForm).GetField("_indexGrip",flags)!.GetValue(reopened)!;
            Check(reopenedGrip.Mode.Enabled,"reopening restores Index controller choice and grip availability");
        } finally {
            if(savedChoice!=null) File.WriteAllBytes(choicePath,savedChoice); else File.Delete(choicePath);
        }
        host.PerformLayout();
        using(var bitmap=new Bitmap(host.ClientSize.Width,host.ClientSize.Height)) {
            host.DrawToBitmap(bitmap,new Rectangle(Point.Empty,bitmap.Size));
            bitmap.Save(Path.Combine(output,"settings-panel.png"));
        }
        Console.WriteLine($"PASS: {checks} Index grip Configurator checks");
        return 0;
    }
}
