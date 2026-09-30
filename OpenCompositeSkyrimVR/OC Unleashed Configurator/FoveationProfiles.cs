using System;
using System.Globalization;

namespace OpenCompositeConfigurator;

internal readonly record struct FoveationRadii(decimal Inner, decimal Mid);

internal static class FoveationProfiles
{
    public const int DefaultEyePreset = 2;
    public const int AggressiveEyePreset = 4; // Keep existing preset and Custom indices stable.
    public static readonly string[] EyePresetNames = { "Quality", "Balanced", "Performance", "Custom", "Aggressive" };
    public static readonly string[] RateChoices = { "1x1", "1x2", "2x1", "2x2", "2x4", "4x2", "4x4" };
    public static string ValidRate(string value) => Array.IndexOf(RateChoices, value) >= 0 ? value : "1x1";
    public static string EffectiveRate(string value, bool compatibility, bool favorHorizontal)
    {
        string rate = ValidRate(value);
        if (!compatibility || rate is "1x1" or "1x2" or "2x1") return rate;
        if (rate == "2x4") return "1x2";
        if (rate == "4x2") return "2x1";
        return favorHorizontal ? "2x1" : "1x2";
    }
    public static string ReadRate(IniFile ini, string ring, string fallback) =>
        ValidRate(ini.Get("", "vrsEye" + ring + "Rate", fallback));
    public static void WriteRate(IniFile ini, string ring, string value) =>
        ini.Set("", "vrsEye" + ring + "Rate", ValidRate(value));
    // Keep defaults/range rules aligned with OpenOVR/Misc/FoveationProfiles.h.
    public static FoveationRadii Preset(bool eyeTracked, int index) => (eyeTracked, index) switch
    {
        (true, 0) => new(0.30m, 0.65m),
        (true, 1) => new(0.22m, 0.45m),
        (true, _) => new(0.20m, 0.40m),
        (false, 1) => new(0.60m, 0.80m),
        (false, 2) => new(0.50m, 0.70m),
        _ => new(0.70m, 0.85m)
    };

    public static int Detect(bool eyeTracked, FoveationRadii radii)
    {
        for (int i = 0; i < 3; ++i) if (Preset(eyeTracked, i) == radii) return i;
        return 3;
    }

    public static FoveationRadii Read(IniFile ini, bool eyeTracked)
    {
        string prefix = eyeTracked ? "vrsEye" : "vrsFixed";
        var defaults = Preset(eyeTracked, eyeTracked ? DefaultEyePreset : 0);
        decimal read(string key, string legacyKey, decimal fallback, decimal max)
        {
            foreach (string candidate in new[] { ini.Get("", key, ""), ini.Get("", legacyKey, "") })
            {
                if (decimal.TryParse(candidate.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out decimal value) && value >= 0)
                    return Math.Clamp(value, 0.10m, max);
            }
            return fallback;
        }
        decimal inner = read(prefix + "InnerRadius", "vrsInnerRadius", defaults.Inner, 1.0m);
        decimal mid = read(prefix + "MidRadius", "vrsMidRadius", defaults.Mid, 1.5m);
        return new(inner, Math.Max(inner, mid));
    }

    public static void Write(IniFile ini, bool eyeTracked, FoveationRadii radii)
    {
        string prefix = eyeTracked ? "vrsEye" : "vrsFixed";
        decimal inner = Math.Clamp(radii.Inner, 0.10m, 1.0m);
        decimal mid = Math.Clamp(Math.Max(inner, radii.Mid), 0.10m, 1.5m);
        ini.Set("", prefix + "InnerRadius", inner.ToString("0.00", CultureInfo.InvariantCulture));
        ini.Set("", prefix + "MidRadius", mid.ToString("0.00", CultureInfo.InvariantCulture));
    }
}
