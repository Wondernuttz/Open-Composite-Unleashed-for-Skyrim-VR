using System;
using System.IO;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            // Auto-detect game type from executable name
            string exeName = Path.GetFileNameWithoutExtension(Application.ExecutablePath);
            string gameType = "skyrim"; // default

            if (exeName.Contains("Fallout", StringComparison.OrdinalIgnoreCase))
                gameType = "fallout4";
            else if (exeName.Contains("Skyrim", StringComparison.OrdinalIgnoreCase))
                gameType = "skyrim";

            Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm(gameType));
        }
    }
}
