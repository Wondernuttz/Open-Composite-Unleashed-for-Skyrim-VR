using System;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    static int AuditFrameDefinitions()
    {
        string root = AppContext.BaseDirectory;
        Directory.CreateDirectory(Path.Combine(root, "root"));
        string controls = Path.Combine(root, "interface", "controls", "pc");
        Directory.CreateDirectory(controls);
        string path = Path.Combine(controls, "oculuscontroller.txt");
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        using var form = new MainForm();
        Call(form, "ApplyControllerModel", "frame");
        string stock = "// preserve this comment\r\ngrab 0x0002\r\nOCC_A 0x0007\r\nOCC_B 0x0001\r\nthumbstick 0x0020\r\ntrigger 0x0021\r\nCustom 0x0009\r\n";
        void Verify()
        {
            var rows = File.ReadAllLines(path).Where(l => l.Length > 0 && !l.StartsWith("//"))
                .Select(l => l.Split('\t', StringSplitOptions.RemoveEmptyEntries))
                .ToDictionary(f => Convert.ToInt32(f[1], 16), f => f[0]);
            foreach (int id in new[] { 1, 2, 3, 5, 6, 7, 32, 33, 35 })
                Check(rows.ContainsKey(id), "Unregistered button after save: " + id);
        }
        // These are the real shared save paths used by bindings/presets, combo
        // autosave, keyboard shortcuts, and the Settings/Video save buttons.
        foreach (string route in new[] { "GetControlmapSavePath", "SaveCombosToIniFiles", "GetOpenCompositeIniSavePaths" })
        {
            File.Delete(path);
            void Save() { if (route == "GetOpenCompositeIniSavePaths") Call(form, route, true); else Call(form, route); }
            Save(); Verify();
            File.WriteAllText(path, stock);
            Save(); Verify();
            Check(File.ReadAllText(path).StartsWith(stock.Replace(" 0x", "\t0x")), "Existing definitions/comments were lost during delimiter repair");
            string complete = File.ReadAllText(path);
            DateTime stamp = File.GetLastWriteTimeUtc(path);
            Save();
            Check(File.ReadAllText(path) == complete && File.GetLastWriteTimeUtc(path) == stamp, "Repeated save rewrites completed definitions");
            File.WriteAllText(path, stock + "Duplicate 0x0007\r\n");
            bool rejected = false;
            try { Save(); } catch (System.Reflection.TargetInvocationException e) when (e.InnerException is IOException) { rejected = true; }
            Check(rejected && File.ReadAllText(path) == stock + "Duplicate 0x0007\r\n", "Ambiguous definition silently accepted or overwritten");
            File.WriteAllText(path, complete);
        }
        Console.WriteLine("PASS: controller definitions recovered/merged on binding, combo and settings save paths; stock/custom entries preserved; repeated saves unchanged; duplicate definitions rejected.");
        return 0;
    }
}
