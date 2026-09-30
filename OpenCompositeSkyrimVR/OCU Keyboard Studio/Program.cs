namespace OCUKeyboardStudio;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length >= 3 && args[0].Equals("--render-background", StringComparison.OrdinalIgnoreCase))
        {
            string layoutPath = Path.GetFullPath(args[1]);
            string outputPath = Path.GetFullPath(args[2]);
            string assetsDirectory = args.Length > 3
                ? Path.GetFullPath(args[3])
                : Path.Combine(AppContext.BaseDirectory, "Assets");
            string metadata = Path.Combine(assetsDirectory, "ParchmentMF-30.sfn");
            string texture = Path.Combine(assetsDirectory, "ParchmentMF-30-texture.png");
            var renderer = new KeyboardRenderer(assetsDirectory, new SudoFont(metadata, texture));
            KeyboardDocument document = KeyboardDocument.Load(layoutPath);
            using Bitmap background = renderer.RenderBackgroundExact(document);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
            background.Save(outputPath, System.Drawing.Imaging.ImageFormat.Png);
            return;
        }

        if (args.Length > 0 && args[0].Equals("--self-test", StringComparison.OrdinalIgnoreCase))
        {
            // Never write generated self-test ZIPs beside a published Studio
            // executable. Nexus rejects nested archives inside a mod upload, and
            // a no-argument self-test previously contaminated the publish folder
            // with two test-export ZIPs. Keep implicit output isolated in Temp;
            // CI may still provide an explicit artifact directory as arg 2.
            string outputDirectory = args.Length > 1
                ? Path.GetFullPath(args[1])
                : Path.Combine(
                    Path.GetTempPath(),
                    "OCUKeyboardStudio",
                    $"SelfTest-{DateTime.Now:yyyyMMdd-HHmmss}-{Environment.ProcessId}");
            Directory.CreateDirectory(outputDirectory);
            Environment.ExitCode = StudioSelfTest.Run(outputDirectory);
            return;
        }

        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new MainForm(
            GetOptionValue(args, "--ocu-root"),
            GetKeyboardFileArgument(args)));
    }

    internal static string? GetOptionValue(IReadOnlyList<string> args, string option)
    {
        for (int index = 0; index < args.Count; index++)
        {
            if (!args[index].Equals(option, StringComparison.OrdinalIgnoreCase))
                continue;
            return index + 1 < args.Count && !string.IsNullOrWhiteSpace(args[index + 1])
                ? args[index + 1]
                : null;
        }
        return null;
    }

    private static string? GetKeyboardFileArgument(IEnumerable<string> args)
        => args.FirstOrDefault(argument => File.Exists(argument)
            && (Path.GetExtension(argument).Equals(KeyboardPackage.Extension, StringComparison.OrdinalIgnoreCase)
                || Path.GetExtension(argument).Equals(".kb", StringComparison.OrdinalIgnoreCase)));
}
