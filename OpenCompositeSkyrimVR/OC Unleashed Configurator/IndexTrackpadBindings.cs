using System;
using System.Globalization;
using System.Linq;

namespace OpenCompositeConfigurator
{
    public partial class MainForm
    {
        // Stored in the controlmap too so exported custom presets retain routing,
        // while applying a built-in/old preset restores its legacy Menu/A routing.
        private const string TrackpadMetadata = "// OCU IndexTrackpadCustomRegions=";
        private int _indexTrackpadCustomRegions;

        private static int TrackpadRegion(string? id) => id switch
        {
            "l_trackpad_upper" => 0, "l_trackpad_lower" => 1,
            "r_trackpad_upper" => 2, "r_trackpad_lower" => 3, _ => -1
        };

        private static int ReadTrackpadRegions(string text)
        {
            foreach (string raw in text.Split('\n'))
            {
                string line = raw.Trim();
                if (line.StartsWith(TrackpadMetadata, StringComparison.Ordinal)
                    && int.TryParse(line[TrackpadMetadata.Length..], NumberStyles.Integer,
                        CultureInfo.InvariantCulture, out int value) && value >= 0 && value <= 15)
                    return value;
            }
            return 0;
        }

        private bool TryGetControllerBindingHex(string id, out (string hexRight, string hexLeft) hex)
        {
            int region = TrackpadRegion(id);
            if (region < 0) return ControllerButtonHex.TryGetValue(id, out hex);
            bool upper = (region & 1) == 0;
            bool custom = (_indexTrackpadCustomRegions & (1 << region)) != 0;
            string value = custom ? (upper ? "0x05" : "0x06") : (upper ? "0x01" : "0x07");
            hex = region < 2 ? ("", value) : (value, "");
            return true;
        }

        private static string AppendTrackpadHex(string current, string value)
        {
            if (string.IsNullOrWhiteSpace(current) || current.Equals("0xff", StringComparison.OrdinalIgnoreCase)) return value;
            return current.Split(',').Any(part => part.Trim().Equals(value, StringComparison.OrdinalIgnoreCase))
                ? current : current + "," + value;
        }

        private void EnsureIndependentTrackpadRegion(string id)
        {
            int region = TrackpadRegion(id);
            if (region < 0 || (_indexTrackpadCustomRegions & (1 << region)) != 0) return;
            string legacy = (region & 1) == 0 ? "0x01" : "0x07";
            string independent = (region & 1) == 0 ? "0x05" : "0x06";
            int[] columns = region < 2 ? new[] { 5, 7, 9 } : new[] { 4, 6, 8 };
            // Preserve this half's previous behavior in every other context.
            // Face buttons retain their original ids and assignments.
            foreach (var context in _contextBindings)
            foreach (string[] fields in context.Value)
            {
                if (fields.Length <= 9) continue;
                foreach (int column in columns)
                {
                    if (TryRemoveControllerHex(fields[column], independent, out string cleared))
                    {
                        fields[column] = cleared;
                        RecordControllerChange(context.Key, fields[0], column, cleared);
                    }
                    if (!TryRemoveControllerHex(fields[column], legacy, out _)) continue;
                    fields[column] = AppendTrackpadHex(fields[column], independent);
                    RecordControllerChange(context.Key, fields[0], column, fields[column]);
                }
            }
            _indexTrackpadCustomRegions |= 1 << region;
        }

        private void PersistTrackpadRouting()
        {
            _ini.Set("", "indexTrackpadCustomRegions", _indexTrackpadCustomRegions.ToString(CultureInfo.InvariantCulture));
            foreach (string path in GetOpenCompositeIniSavePaths(createDirectories: true))
                _ini.Save(path);
        }
    }
}
