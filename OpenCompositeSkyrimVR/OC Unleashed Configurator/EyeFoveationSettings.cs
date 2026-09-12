using System;
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("ConfiguratorFoveation")]

namespace OpenCompositeConfigurator;

// A value snapshot shared by the page, popup, persistence and preview. The popup
// edits its own snapshot; only an accepted result is applied to the page.
internal sealed record EyeFoveationSettings
{
    public const string DefaultInnerRate = "1x1";
    public const string DefaultMidRate = "2x2";
    public const string DefaultOuterRate = "4x2";
    public bool Enabled { get; init; } = true;
    public int Backend { get; init; }
    public bool DebugRings { get; init; }
    public FoveationRadii Radii { get; init; } = FoveationProfiles.Preset(true, FoveationProfiles.DefaultEyePreset);
    public bool CustomRates { get; init; } = true;
    public string InnerRate { get; init; } = DefaultInnerRate;
    public string MidRate { get; init; } = DefaultMidRate;
    public string OuterRate { get; init; } = DefaultOuterRate;
    public bool Compatibility { get; init; }
    public bool FavorHorizontal { get; init; } = true;

    public static readonly string[] BackendNames = {
        "Auto (recommended)", "NVIDIA VRS", "Density Mask (AMD / Intel)", "Shader effects only (test)"
    };
    public string[] RequestedRates => CustomRates ? new[] { InnerRate, MidRate, OuterRate }
        : new[] { "1x1", FavorHorizontal ? "2x1" : "1x2", "2x2" };
    public string[] EffectiveRates => Array.ConvertAll(RequestedRates,
        rate => FoveationProfiles.EffectiveRate(rate, Compatibility, FavorHorizontal));
    private static (string Inner, string Mid, string Outer)? PresetRates(int index) => index switch {
        0 => ("1x1", "1x2", "2x2"),
        1 or 2 => (DefaultInnerRate, DefaultMidRate, DefaultOuterRate),
        FoveationProfiles.AggressiveEyePreset => ("1x1", "2x2", "4x4"),
        _ => null
    };
    public int PresetIndex {
        get {
            if (!CustomRates || Compatibility) return 3;
            for (int index = 0; index < FoveationProfiles.EyePresetNames.Length; ++index)
                if (PresetRates(index) is { } rates && Radii == FoveationProfiles.Preset(true, index)
                    && InnerRate == rates.Inner && MidRate == rates.Mid && OuterRate == rates.Outer) return index;
            return 3;
        }
    }
    public EyeFoveationSettings WithPreset(int index) => PresetRates(index) is { } rates
        ? this with { Radii = FoveationProfiles.Preset(true, index), CustomRates = true,
            InnerRate = rates.Inner, MidRate = rates.Mid, OuterRate = rates.Outer, Compatibility = false }
        : this;

    public static EyeFoveationSettings Read(IniFile ini)
    {
        static bool flag(string value) => value.Trim().ToLowerInvariant() is "true" or "1" or "yes" or "on" or "enabled";
        const string missing = "\u0001missing-eye-setting";
        // Old implicit rates use a different RDM layout and honor the shared
        // axis. Only an untuned eye profile adopts custom Performance rates.
        bool previouslyTuned = Array.Exists(new[] { "vrsInnerRadius", "vrsMidRadius", "vrsEyeInnerRadius", "vrsEyeMidRadius",
            "vrsCompatibilityMode", "vrsEyeCompatibilityMode", "vrsFavorHorizontal", "vrsEyeInnerRate", "vrsEyeMidRate", "vrsEyeOuterRate", "vrsEyeCustomRates" },
            key => ini.Get("", key, missing) != missing);
        return new() {
            Enabled = flag(ini.Get("", "vrsEyeTracked", "true")),
            Backend = ini.Get("", "foveatedBackend", "auto").Trim().ToLowerInvariant() switch {
                "vrs" => 1, "rdm" => 2, "effects" => 3, _ => 0 },
            DebugRings = flag(ini.Get("", "foveationDebugRings", "false")),
            Radii = FoveationProfiles.Read(ini, true),
            CustomRates = flag(ini.Get("", "vrsEyeCustomRates", previouslyTuned ? "false" : "true")),
            InnerRate = FoveationProfiles.ReadRate(ini, "Inner", DefaultInnerRate),
            // Partial older profiles retain their former unspecified rates.
            // Explicit saved values win; only an untuned profile starts Performance.
            MidRate = FoveationProfiles.ReadRate(ini, "Mid", previouslyTuned ? "2x1" : DefaultMidRate),
            OuterRate = FoveationProfiles.ReadRate(ini, "Outer", previouslyTuned ? "2x2" : DefaultOuterRate),
            // Old tuned profiles retain their implicit half-rate cap. Only an
            // untuned profile starts uncapped; selecting an eye preset writes false.
            Compatibility = flag(ini.Get("", "vrsEyeCompatibilityMode",
                ini.Get("", "vrsCompatibilityMode", previouslyTuned ? "true" : "false"))),
            FavorHorizontal = flag(ini.Get("", "vrsFavorHorizontal", "true"))
        };
    }

    public void Write(IniFile ini)
    {
        // Merge-save reloads the original file. Remove all recognized legacy
        // copies of these owned keys before writing one canonical value; Get's
        // root-first lookup and the runtime's parse order must agree afterwards.
        foreach (string key in new[] { "vrsEyeTracked", "foveatedBackend", "foveationDebugRings", "vrsEyeInnerRadius", "vrsEyeMidRadius",
            "vrsEyeCustomRates", "vrsEyeInnerRate", "vrsEyeMidRate", "vrsEyeOuterRate", "vrsEyeCompatibilityMode", "vrsFavorHorizontal" })
            ini.Remove("", key);
        void flag(string key, bool value) => ini.Set("vrs", key, value ? "true" : "false");
        flag("vrsEyeTracked", Enabled);
        flag("foveationDebugRings", DebugRings);
        flag("vrsEyeCustomRates", CustomRates);
        flag("vrsEyeCompatibilityMode", Compatibility);
        flag("vrsFavorHorizontal", FavorHorizontal);
        ini.Set("vrs", "foveatedBackend", Backend switch { 1 => "vrs", 2 => "rdm", 3 => "effects", _ => "auto" });
        FoveationProfiles.Write(ini, true, Radii);
        FoveationProfiles.WriteRate(ini, "Inner", InnerRate);
        FoveationProfiles.WriteRate(ini, "Mid", MidRate);
        FoveationProfiles.WriteRate(ini, "Outer", OuterRate);
    }
}
