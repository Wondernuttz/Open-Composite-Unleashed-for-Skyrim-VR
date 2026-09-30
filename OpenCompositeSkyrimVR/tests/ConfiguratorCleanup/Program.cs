using System;
using System.IO;
using OpenCompositeConfigurator;

string path = Path.Combine(Path.GetTempPath(), "ocu-cleanup-test-" + Guid.NewGuid() + ".ini");
string[] retired = {
    "aswBufferEnabled", "aswExperimentalMode", "aswCaptureEnabled",
    "aswForceLegacy", "aswConcurrentFrameThread", "aswSpeculativeTrackingLead",
    "aswUpscalerReset", "aswUpscalerReactiveMask",
    "aswForceCustom", "aswFPControllerScale", "aswMVConfidence", "aswMVPixelScale"
};
try {
    foreach (string section in new[] { "", "[default]\n", "[asw]\n", "[general]\n" }) {
        string fixture = "; preserve my comment\n" + section;
        foreach (string key in retired) fixture += key + "=true\n";
        fixture += "[asw]\naswEnabled=true\naswWarpStrength=0.75\n"
            + "[vrs]\nvrsEyeTracked=true\nfoveatedBackend=rdm\n"
            + "[input]\nswapThumbsticks=true\n[third_party]\ncustomSetting=keep\n";
        File.WriteAllText(path, fixture);
        var ini = new IniFile();
        ini.Load(path);
        ini.Save();
        string output = File.ReadAllText(path);
        foreach (string key in retired)
            if (output.Contains(key, StringComparison.OrdinalIgnoreCase))
                throw new Exception("Retired setting survived: " + key);
        foreach (string expected in new[] { "; preserve my comment", "aswEnabled=true",
            "aswWarpStrength=0.75", "vrsEyeTracked=true", "foveatedBackend=rdm",
            "swapThumbsticks=true", "customSetting=keep" })
            if (!output.Contains(expected)) throw new Exception("Lost active setting: " + expected);
        ini.Load(path);
        ini.Save();
        if (output != File.ReadAllText(path)) throw new Exception("Save is not idempotent");
    }
    Console.WriteLine("Configurator retired-setting migration and preservation PASS (4 section layouts)");
    foreach (string section in new[] { "", "[default]\n", "[vrs]\n" }) {
        File.WriteAllText(path, section + "vrsInnerRadius=0.63\nvrsMidRadius=0.83\nkeepMe=yes\n");
        var ini = new IniFile();
        ini.Load(path);
        foreach (bool eye in new[] { false, true })
            if (FoveationProfiles.Read(ini, eye) != new FoveationRadii(0.63m, 0.83m))
                throw new Exception("Legacy profile migration changed coverage");
        FoveationProfiles.Write(ini, true, new(0.45m, 0.65m));
        FoveationProfiles.Write(ini, false, new(0.75m, 0.90m));
        ini.Remove("", "vrsInnerRadius");
        ini.Remove("", "vrsMidRadius");
        ini.Save();
        ini.Load(path);
        if (FoveationProfiles.Read(ini, true) != new FoveationRadii(0.45m, 0.65m) ||
            FoveationProfiles.Read(ini, false) != new FoveationRadii(0.75m, 0.90m))
            throw new Exception("Independent profile round-trip failed");
        if (!File.ReadAllText(path).Contains("keepMe=yes"))
            throw new Exception("Unrelated setting lost");
    }
    File.WriteAllText(path, "");
    var fresh = new IniFile();
    fresh.Load(path);
    if (FoveationProfiles.Read(fresh, true) != new FoveationRadii(0.50m, 0.70m) ||
        FoveationProfiles.Read(fresh, false) != new FoveationRadii(0.70m, 0.85m))
        throw new Exception("Fresh profile defaults mismatch");
    fresh.Set("", "vrsEyeInnerRadius", "NaN");
    fresh.Set("", "vrsEyeMidRadius", "-1");
    if (FoveationProfiles.Read(fresh, true) != new FoveationRadii(0.50m, 0.70m))
        throw new Exception("Invalid radii did not fall back to defaults");
    FoveationProfiles.Write(fresh, true, new(0.80m, 0.30m));
    if (FoveationProfiles.Read(fresh, true) != new FoveationRadii(0.80m, 0.80m))
        throw new Exception("Ring ordering was not repaired");
    Console.WriteLine("Independent foveation profiles: defaults, migration, overrides and ordering PASS");
    foreach (string section in new[] { "", "[default]\n", "[vrs]\n" }) {
        File.WriteAllText(path, section + "vrsEyeCustomRates=true\nvrsEyeOuterRate=4x2\nvrsFixedInnerRadius=0.77\n");
        fresh.Load(path);
        foreach (string rate in FoveationProfiles.RateChoices) {
            FoveationProfiles.WriteRate(fresh, "Mid", rate);
            fresh.Save(); fresh.Load(path);
            if (FoveationProfiles.ReadRate(fresh, "Mid", "1x1") != rate ||
                FoveationProfiles.ReadRate(fresh, "Outer", "1x1") != "4x2" ||
                fresh.Get("", "vrsFixedInnerRadius", "") != "0.77")
                throw new Exception("Ring rate round-trip changed another ring or fixed profile");
        }
    }
    foreach (string rate in FoveationProfiles.RateChoices) {
        if (FoveationProfiles.EffectiveRate(rate, false, true) != rate)
            throw new Exception("Uncapped rate changed");
        if (Array.IndexOf(new[] { "1x1", "1x2", "2x1" }, FoveationProfiles.EffectiveRate(rate, true, false)) < 0)
            throw new Exception("Compatibility cap failed");
    }
    if (FoveationProfiles.ValidRate("8x8") != "1x1" ||
        FoveationProfiles.EffectiveRate("4x2", true, false) != "2x1" ||
        FoveationProfiles.EffectiveRate("2x4", true, true) != "1x2")
        throw new Exception("Invalid input or anisotropic cap failed");
    Console.WriteLine("Eye ring rate persistence, fixed isolation and effective compatibility display PASS");
} finally {
    if (File.Exists(path)) File.Delete(path);
}
