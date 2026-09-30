using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    static int AuditFramePresets()
    {
        string root = AppContext.BaseDirectory;
        string definitionPath = Path.Combine(root, "Interface", "controls", "pc", "oculuscontroller.txt");
        Check(File.Exists(definitionPath), "Published Frame hold-time definition is missing");
        var definition = File.ReadAllLines(definitionPath)
            .Where(l => l.Trim().Length > 0 && !l.TrimStart().StartsWith("//"))
            .Select(l => l.Split('\t', StringSplitOptions.RemoveEmptyEntries))
            .ToDictionary(f => Convert.ToInt32(f[1][2..], 16), f => f[0]);
        foreach (var stock in new[] { (2, "grab"), (7, "OCC_A"), (1, "OCC_B"), (32, "thumbstick"), (33, "trigger") })
            Check(definition.TryGetValue(stock.Item1, out string? name) && name == stock.Item2,
                "Stock Touch button identity changed: " + stock.Item2);
        foreach (int extra in new[] {3, 5, 6, 35})
            Check(definition.ContainsKey(extra), "Frame extra has no native hold-time entry: " + extra);
        Directory.CreateDirectory(Path.Combine(root, "root"));
        Directory.CreateDirectory(Path.Combine(root, "interface", "controls", "pc"));
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        using var form = new MainForm();
        string path = (string)Call(form, "GetControlmapSavePath")!;
        var presets = (Dictionary<string,string>)typeof(MainForm).GetField("BindingPresetResources", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        List<string[]> Rows(string text) => text.Split('\n').Where(l => l.Trim().Length > 0 && !l.TrimStart().StartsWith("//"))
            .Select(l => l.Trim().Split('\t', StringSplitOptions.RemoveEmptyEntries).Select(f => f.Trim()).ToArray()).Where(f => f.Length >= 10).ToList();
        string Build(string? resource, string? custom = null)
        {
            object[] args = {resource!, custom!, "", ""};
            Check((bool)Call(form, "TryBuildControllerPresetMergedText", args)!, "Preset failed: " + args[3]);
            return (string)args[2];
        }
        void Apply(string? resource, string? custom = null)
        {
            object[] args = {resource!, custom!, ""};
            Check((bool)Call(form, "ApplyControllerPresetMergingKeyboard", args)!, "Apply failed: " + args[2]);
        }
        foreach (var preset in presets)
        {
            using var stream = typeof(MainForm).Assembly.GetManifestResourceStream(preset.Value)!;
            using var reader = new StreamReader(stream);
            string original = reader.ReadToEnd();
            File.WriteAllText(path, original);
            Call(form, "ApplyControllerModel", "touch");
            string baseline = Build(preset.Value);
            Call(form, "ApplyControllerModel", "frame");
            string translated = Build(preset.Value);
            Check(File.ReadAllText(path) == original, "Preview wrote live map");
            var before = Rows(baseline); var after = Rows(translated);
            Check(before.Count == after.Count, "Row count changed: " + preset.Key);
            for (int i = 0; i < before.Count; i++)
            {
                Check(before[i].Length == after[i].Length, "Column count changed");
                for (int col = 0; col < before[i].Length; col++)
                    if (col != 6 && col != 7) Check(before[i][col] == after[i][col], "Unrelated field changed: " + preset.Key);
                var left = after[i][7].Split(',', '+');
                var right = after[i][6].Split(',', '+');
                foreach (string key in left.Concat(right))
                    if (key.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                    {
                        int id = Convert.ToInt32(key[2..], 16);
                        // 0xb/0xc are Skyrim's analog thumbstick motion events,
                        // not digital buttons using the held-time table.
                        Check(id == 255 || id == 11 || id == 12 || definition.ContainsKey(id),
                            "Preset button has no native hold-time entry: " + preset.Key + "/" + after[i][0] + "/" + key);
                    }
                Check(!left.Any(k => new[]{"0x1","0x7","0x5","0x6","0x3","0x23","0x0"}.Contains(k)), "Frame extra assigned on left");
                Check(!right.Any(k => new[]{"0x3","0x23","0x0"}.Contains(k)), "Frame extra assigned on right");
                Check(!after[i][6].Contains('!') && !after[i][7].Contains('!'), "Unresolved cross-hand alias");
                // Verify simple alternatives independently; aliased rows are checked below.
                if (!before[i][7].Contains('!'))
                foreach (var binding in before[i][7].Split(','))
                {
                    string[] keys = binding.Split('+');
                    bool moved = keys.Contains("0x07") || keys.Contains("0x01");
                    if (!moved) continue;
                    string expected = string.Join('+', keys.Select(k => k == "0x07" ? "0x5" : k == "0x01" ? "0x6" : "0x" + Convert.ToInt32(k[2..],16).ToString("x")));
                    Check(after[i][6].Split(',').Contains(expected), "Lost face action/chord: " + preset.Key + "/" + before[i][0]);
                }
            }
            Apply(preset.Value);
            Check(File.ReadAllText(path) == translated, "Apply differs from preview");
            Call(form, "ValidateAndRepairControlmap");
            Check(File.ReadAllText(path) == translated, "Validation damaged Frame layout");
            // Already-translated custom maps must not be translated again.
            string custom = Path.Combine(root, "frame-custom.txt");
            File.WriteAllText(custom, translated);
            Check(Build(null, custom) == translated, "Custom preset was translated twice");
            Call(form, "ApplyControllerModel", "touch");
            Check(Build(preset.Value) == baseline, "Frame conversion leaked to Touch");
        }
        // A synthetic alias fixture catches a left face button inherited from
        // gameplay after moving it to another hand, alongside a same-hand grip.
        string Row(string name, string r, string l) => string.Join('\t', new[]{name,"0x11","0xff","0xffff","0xff","0xff",r,l,"0xff","0xff"});
        string fixture = "// Main Gameplay\n"+Row("Activate","0x07","0x07")+"\n"+Row("Left Attack/Block","0xff","0x21")+"\n\n// Crafting Menus\n"+Row("Accept","!0,Activate","!0,Activate,!0,Left Attack/Block")+"\n";
        var method = typeof(MainForm).GetMethod("AdaptBuiltinPresetForFrame", BindingFlags.Static|BindingFlags.NonPublic)!;
        var accept = Rows((string)method.Invoke(null,new object[]{fixture})!).Last();
        Check(accept[6] == "0x7,0x5" && accept[7] == "0x21", "Inherited crafting activation lost during hand transfer");
        Console.WriteLine("PASS: all 10 Frame presets; published native hold definitions, stock Touch identities, preview/apply equality, menu aliases, face chords, spare buttons, custom round trips, validation and Touch isolation");
        return 0;
    }
}
