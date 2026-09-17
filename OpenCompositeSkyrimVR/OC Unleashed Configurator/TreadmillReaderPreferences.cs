using System;
using System.IO;
using System.Text.Json;

namespace OpenCompositeConfigurator
{
    // This is a machine-local SDK installation choice, independent of the mod INI.
    internal sealed record TreadmillReaderPreferences(string SdkPath = "", bool StartWithConfigurator = true)
    {
        internal static string StoragePath => Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "OpenCompositeConfigurator", "KatReader.json");

        internal static TreadmillReaderPreferences Load(string path)
        {
            try
            {
                if (!File.Exists(path)) return new();
                var preferences = JsonSerializer.Deserialize<TreadmillReaderPreferences>(File.ReadAllText(path));
                if (preferences == null || preferences.SdkPath == null) return new();
                return preferences;
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException || ex is JsonException)
            {
                return new();
            }
        }

        internal static bool IsSdkPath(string path) => !string.IsNullOrWhiteSpace(path)
            && Path.IsPathFullyQualified(path)
            && Path.GetFileName(path).Equals("KATNativeSDK.dll", StringComparison.OrdinalIgnoreCase);

        internal bool TrySave(string path, out string error)
        {
            error = "";
            if (SdkPath.Length > 0 && !IsSdkPath(SdkPath))
            {
                error = "Choose KATNativeSDK.dll from your installed Gateway.";
                return false;
            }
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(temporary, JsonSerializer.Serialize(this));
                File.Move(temporary, path, overwrite: true);
                return true;
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                error = "The SDK choice could not be remembered on this PC. " + ex.Message;
                return false;
            }
            finally
            {
                try { File.Delete(temporary); }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }

        internal (bool Start, string Status) AutoStart(bool treadmillEnabled, string readerExecutable)
        {
            if (!treadmillEnabled || !StartWithConfigurator) return (false, "");
            if (SdkPath.Length == 0) return (false, "KAT: choose your SDK in Reader setup");
            if (!IsSdkPath(SdkPath) || !File.Exists(SdkPath))
                return (false, "KAT: saved SDK is unavailable — choose it in Reader setup");
            if (!File.Exists(readerExecutable))
                return (false, "KAT: Reader is missing — check your OCU installation");
            return (true, "");
        }
    }
}
