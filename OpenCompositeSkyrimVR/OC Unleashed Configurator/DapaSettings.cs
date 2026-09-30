using System;
using System.Globalization;

namespace OpenCompositeConfigurator
{
    public partial class MainForm
    {
        // Shared by control construction and missing/invalid INI values. Keep
        // these aligned with the runtime defaults and opencomposite.ini.example.
        private const decimal DapaWarpDefault = 1.00m;
        private const decimal DapaRotationDefault = 1.00m;
        private const decimal DapaTranslationDefault = 0.00m;
        private const decimal DapaLocoDefault = 1.00m;
        private const decimal DapaDepthDefault = 1.00m;
        private const decimal DapaEngageFpsDefault = 50m;
        private const decimal DapaNearFadeDefault = 0m;
        private const decimal DapaEdgeFadeDefault = 3m;

        private decimal ReadDapaNumber(string key, decimal fallback, decimal minimum, decimal maximum)
        {
            // Decimal rejects NaN/infinity. Never retain the prior file's UI
            // value after a missing/invalid key or a reset. Match the runtime's
            // acceptance of comma decimal separators, then save invariantly.
            string raw = _ini.Get("", key, "").Replace(',', '.');
            return decimal.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out decimal value)
                ? Math.Clamp(value, minimum, maximum) : fallback;
        }

        private void ReadDapaSettings()
        {
            _chkAswEnabled.Checked = ParseBool(_ini.Get("", "aswEnabled", "false"));
            _nudAswWarpStrength.Value = ReadDapaNumber("aswWarpStrength", DapaWarpDefault, 0m, 3m);
            _nudAswRotationScale.Value = ReadDapaNumber("aswRotationScale", DapaRotationDefault, 0m, 2m);
            _nudAswTranslationScale.Value = ReadDapaNumber("aswTranslationScale", DapaTranslationDefault, 0m, 3m);
            _nudAswLocoScale.Value = ReadDapaNumber("aswLocoScale", DapaLocoDefault, 0m, 3m);
            _nudAswDepthScale.Value = ReadDapaNumber("aswDepthScale", DapaDepthDefault, 0m, 2m);
            _aswNearFadeDepth = (float)ReadDapaNumber("aswNearFadeDepth", DapaNearFadeDefault, 0m, 10m);
            _aswEdgeFadeWidth = (float)ReadDapaNumber("aswEdgeFadeWidth", DapaEdgeFadeDefault, 0m, 10m);
            _chkAswAutoNative.Checked = ParseBool(_ini.Get("", "aswAutoNative", "false"));
            _nudAswAutoEngageFps.Value = ReadDapaNumber("aswAutoEngageFps", DapaEngageFpsDefault, 20m, 90m);
            _chkAswDebugMode.Checked = _ini.Get("", "aswDebugMode", "0").Trim() == "10";
        }
    }
}
