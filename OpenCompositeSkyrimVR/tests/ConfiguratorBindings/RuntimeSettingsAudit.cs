using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    static int AuditRuntimeSettings()
    {
        string root = Path.Combine(AppContext.BaseDirectory, "runtime-settings-" + Guid.NewGuid());
        string payload = Path.Combine(root, "payload");
        string game = Path.Combine(root, "game");
        Directory.CreateDirectory(payload);
        Directory.CreateDirectory(game);
        Type installer = typeof(MainForm).Assembly.GetType("OpenCompositeConfigurator.RuntimeInstaller")!;
        const BindingFlags flags = BindingFlags.Static | BindingFlags.NonPublic;
        List<(string src, string dst)> Plan() =>
            (List<(string src, string dst)>)installer.GetMethod("BuildSyncPlan", flags)!.Invoke(null, new object[] { payload, game })!;
        int Apply(List<(string src, string dst)> plan) =>
            (int)installer.GetMethod("ApplyPlan", flags)!.Invoke(null, new object[] { plan, game })!;
        string[] configNames = { "opencomposite.ini", "menu_quad_settings.ini", "Gestures/custom.wav" };
        foreach (string name in configNames)
        {
            string source = Path.Combine(payload, name);
            Directory.CreateDirectory(Path.GetDirectoryName(source)!);
            File.WriteAllText(source, "packaged defaults");
        }
        string binary = Path.Combine(payload, "runtime-test.dll");
        File.WriteAllText(binary, "new runtime");
        var fresh = Plan();
        Check(fresh.Count == 4 && Apply(fresh) == 4, "Fresh install must copy every missing runtime and settings file");
        Check(Plan().Count == 0, "Identical installed files must not be rewritten");
        foreach (string name in configNames)
            File.WriteAllText(Path.Combine(game, name), "custom user settings and calibration");
        File.WriteAllText(Path.Combine(game, "runtime-test.dll"), "old runtime");
        var update = Plan();
        Check(update.Count == 1 && Path.GetFileName(update[0].dst) == "runtime-test.dll",
            "Runtime update must preserve existing settings and custom content");
        Check(Apply(update) == 1 && File.ReadAllText(Path.Combine(game, "runtime-test.dll")) == "new runtime",
            "Binary updates must still replace the installed runtime");
        foreach (string name in configNames)
        {
            string destination = Path.Combine(game, name);
            Check(File.ReadAllText(destination) == "custom user settings and calibration", "Existing user file was overwritten: " + name);
            File.Delete(destination);
        }
        var pending = Plan();
        Check(pending.Count == 3, "Missing config files must be included before confirmation");
        foreach (string name in configNames)
            File.WriteAllText(Path.Combine(game, name), "settings saved while confirmation was open");
        Check(Apply(pending) == 0, "New user files created during confirmation must not be overwritten");
        foreach (string name in configNames)
            Check(File.ReadAllText(Path.Combine(game, name)) == "settings saved while confirmation was open",
                "Confirmation race lost user settings: " + name);
        File.Delete(Path.Combine(game, "menu_quad_settings.ini"));
        var invalid = Plan();
        File.Delete(Path.Combine(payload, "menu_quad_settings.ini"));
        bool failedCopyReported = false;
        try { Apply(invalid); }
        catch (TargetInvocationException error) when (error.InnerException is IOException)
        {
            failedCopyReported = true;
        }
        Check(failedCopyReported, "A failed copy must propagate unrelated IO errors");
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        using var form = new MainForm();
        var ini = Field<IniFile>(form, "_ini");
        ini.Reset();
        Call(form, "ReadFromIni");
        var enabled = Field<CheckBox>(form, "_chkMenuLaserEnabled");
        Check(enabled.Checked, "Fresh Configurator must enable its native menu-laser gate");
        ini.Set("", "menuLaserEnabled", "false");
        Call(form, "ReadFromIni");
        Check(!enabled.Checked, "Existing explicit menu-laser opt-out must remain respected");
        Call(form, "WriteToIni", false);
        Check(ini.Get("", "menuLaserEnabled", "true") == "false", "Saving must retain the explicit menu-laser opt-out");
        Console.WriteLine("PASS: actual runtime install/update, calibration preservation, confirmation handoff and menu-laser defaults/opt-out");
        return 0;
    }
}
