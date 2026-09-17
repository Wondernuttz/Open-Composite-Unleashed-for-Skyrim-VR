using System.Diagnostics;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

internal static class Program
{
    private static int checks;
    private static void Check(bool condition, string message)
    {
        ++checks;
        if (!condition) throw new Exception(message);
    }

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length >= 2 && args[0] == "--sdk")
        {
            // A harmless child-process fixture; it never loads the supplied DLL.
            File.WriteAllText(Path.ChangeExtension(args[1], ".pid"), Environment.ProcessId.ToString());
            for (int i = 0; i < 400; ++i)
            {
                Console.WriteLine("{\"connected\":true,\"velocity\":[0,0,0]}");
                Thread.Sleep(25);
            }
            return 0;
        }

        string root = Path.Combine(Path.GetTempPath(), "OCU-KatReader-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            RunPreferences(root);
            RunLifecycle(root);
            Console.WriteLine($"PASS: {checks} KAT preferences, startup, process-lifetime and calibration UI checks.");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
        finally { Directory.Delete(root, recursive: true); }
    }

    private static void RunPreferences(string root)
    {
        string state = Path.Combine(root, "Settings", "KatReader.json");
        string sdk = Path.Combine(root, "SDK choice ü", "KATNativeSDK.dll");
        string reader = Path.Combine(root, "KATReader.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(sdk)!);
        File.WriteAllText(sdk, "fixture");
        File.WriteAllText(reader, "fixture");
        var preferences = TreadmillReaderPreferences.Load(state);
        Check(preferences.SdkPath.Length == 0, "First launch must not guess a DLL");
        Check(!preferences.AutoStart(true, reader).Start, "No chosen SDK must not autostart");
        preferences = new(sdk);
        Check(preferences.TrySave(state, out _), "Chosen SDK must persist without saving the INI");
        Check(TreadmillReaderPreferences.Load(state) == preferences, "SDK path with spaces and Unicode must round-trip");
        Check(preferences.AutoStart(true, reader).Start, "Configured SDK should autostart when KAT is enabled");
        Check(!preferences.AutoStart(false, reader).Start, "Disabled KAT must not start a saved reader");
        preferences = preferences with { StartWithConfigurator = false };
        Check(preferences.TrySave(state, out _), "Opt-out must save");
        Check(!TreadmillReaderPreferences.Load(state).AutoStart(true, reader).Start, "Opt-out must survive restart");
        string previous = File.ReadAllText(state);
        Check(!new TreadmillReaderPreferences("KATNativeSDK.dll").TrySave(state, out _), "Relative DLL path must be rejected");
        Check(!new TreadmillReaderPreferences(reader).TrySave(state, out _), "Wrong file name must be rejected");
        Check(File.ReadAllText(state) == previous, "Rejected choice must preserve prior preferences");
        preferences = preferences with { StartWithConfigurator = true };
        File.Delete(sdk);
        Check(!preferences.AutoStart(true, reader).Start, "Uninstalled Gateway must not launch reader");
        Check(preferences.AutoStart(true, reader).Status.Contains("saved SDK"), "Missing SDK must explain recovery inline");
        Check(preferences.TrySave(state, out _), "Remembered missing path must remain editable");
        Check(TreadmillReaderPreferences.Load(state).SdkPath == sdk, "Missing path must remain visible in setup");
        File.WriteAllText(sdk, "fixture");
        File.Delete(reader);
        Check(!preferences.AutoStart(true, reader).Start, "Absent optional reader must not launch");
        File.WriteAllText(state, "{not JSON");
        Check(TreadmillReaderPreferences.Load(state).SdkPath == "", "Damaged settings must fail cleanly");
        File.WriteAllText(state, "{\"SdkPath\":null,\"StartWithConfigurator\":true}");
        Check(TreadmillReaderPreferences.Load(state).SdkPath == "", "Null SDK must fail cleanly");
        string blocked = Path.Combine(root, "blocked");
        File.WriteAllText(blocked, "not a directory");
        Check(!preferences.TrySave(Path.Combine(blocked, "KatReader.json"), out string error) && error.Length > 0,
            "Persistence failure must return an actionable error without throwing");
        Check(!Directory.EnumerateFiles(root, "*.tmp", SearchOption.AllDirectories).Any(), "Failed save must not leave temporary settings");
        Check(Path.IsPathFullyQualified(TreadmillReaderPreferences.StoragePath)
            && TreadmillReaderPreferences.StoragePath.StartsWith(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)),
            "SDK choice must be stored per user, outside redistributed mod settings");
    }

    private static void RunLifecycle(string root)
    {
        string sdk = Path.Combine(root, "KATNativeSDK.dll");
        File.WriteAllText(sdk, "fixture");
        string fakeReaderFolder = Path.Combine(AppContext.BaseDirectory, "Tools", "KATReader");
        Directory.CreateDirectory(fakeReaderFolder);
        string assembly = typeof(Program).Assembly.Location;
        string stem = Path.GetFileNameWithoutExtension(assembly);
        foreach (string suffix in new[] { ".dll", ".deps.json", ".runtimeconfig.json" })
            File.Copy(Path.Combine(AppContext.BaseDirectory, stem + suffix), Path.Combine(fakeReaderFolder, stem + suffix), true);
        File.Copy(Path.Combine(AppContext.BaseDirectory, stem + ".exe"), Path.Combine(fakeReaderFolder, "KATReader.exe"), true);
        var preferences = new TreadmillReaderPreferences(sdk);
        using (var form = new MainForm(preferences, false))
        {
            form.UnsavedEnabled = true;
            form.SimulateShown();
            Check(form.ReaderDialog == null, "An unsaved enabled checkbox must not override the loaded INI at startup");
            Check(form.CalibrationEnabled, "Controller calibration should default enabled");
            form.Values[("", "unrelated")] = "keep me";
            form.CalibrationEnabled = false;
            form.SaveControls();
            Check(form.Values[("", "treadmillControllerCalibration")] == "false", "Calibration opt-out must save");
            Check(form.Values[("", "unrelated")] == "keep me", "KAT controls must preserve unrelated settings");
        }
        using (var form = new MainForm(preferences with { StartWithConfigurator = false }, true))
        {
            form.SimulateShown();
            Check(form.ReaderDialog == null, "Autostart opt-out must suppress reader creation");
        }
        using (var form = new MainForm(new(Path.Combine(root, "missing", "KATNativeSDK.dll")), true))
        {
            form.SimulateShown();
            Check(form.ReaderDialog == null && form.Connection.Contains("saved SDK"), "Stale saved path must fail inline without opening setup");
        }
        var active = new MainForm(preferences, true);
        Process? process = null;
        try
        {
            active.UnsavedEnabled = false;
            active.SimulateShown();
            Check(active.ReaderDialog is { IsDisposed: false, Visible: false }, "Autostart must leave setup hidden");
            string pidPath = Path.ChangeExtension(sdk, ".pid");
            PumpUntil(() => File.Exists(pidPath), 4000);
            process = Process.GetProcessById(int.Parse(File.ReadAllText(pidPath)));
            Check(!process.HasExited, "Autostart must launch the owned reader");
            PumpUntil(() => active.Connection.Contains("connected"), 4000);
            Check(active.Connection.Contains("connected"), "Hidden setup must continue monitoring fresh reader output");
            var dialog = active.ReaderDialog!;
            _ = dialog.Handle;
            dialog.Close();
            Check(!dialog.IsDisposed && !process.HasExited, "Closing setup must keep an active owned reader running");
            active.Dispose();
            Check(process.WaitForExit(3000), "Closing Configurator must terminate its owned reader");
            Check(dialog.IsDisposed, "Closing Configurator must dispose hidden setup and its timer");
        }
        finally { active.Dispose(); process?.Dispose(); }
    }

    private static void PumpUntil(Func<bool> predicate, int timeoutMs)
    {
        var elapsed = Stopwatch.StartNew();
        while (!predicate() && elapsed.ElapsedMilliseconds < timeoutMs)
        {
            Application.DoEvents();
            Thread.Sleep(10);
        }
        if (!predicate()) throw new TimeoutException("Reader fixture did not reach expected state");
    }
}
