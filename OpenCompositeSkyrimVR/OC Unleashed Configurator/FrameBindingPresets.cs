using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace OpenCompositeConfigurator
{
    public partial class MainForm
    {
        // Frame reports Oculus-compatible controlmap columns. Translate from the
        // original Touch layout once, only when reading a built-in preset.
        // Custom presets already contain the user's chosen physical assignments.
        private static string AdaptBuiltinPresetForFrame(string text)
        {
            var lines = text.Split('\n');
            var gameplay = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
            string context = "";
            foreach (string line in lines)
            {
                if (string.IsNullOrWhiteSpace(line)) { context = ""; continue; }
                if (TryReadControlmapContextHeader(line, out var header)) { context = header; continue; }
                if (line.TrimStart().StartsWith("//")) continue;
                var fields = line.Split('\t', StringSplitOptions.RemoveEmptyEntries).Select(s => s.Trim()).ToArray();
                if (context == "Main Gameplay" && fields.Length >= 10) gameplay[fields[0]] = fields;
            }

            string Resolve(string value, int column, HashSet<string> visiting)
            {
                // Resolve before moving buttons between hands: a left-hand alias
                // must still inherit any X/Y actions that move to the right hand.
                return Regex.Replace(value, @"!([0-9]+),([^,]+)", match =>
                {
                    string action = match.Groups[2].Value.Trim();
                    if (match.Groups[1].Value != "0" || !gameplay.TryGetValue(action, out var source)
                        || !visiting.Add(action))
                        throw new InvalidOperationException("Cannot translate Frame preset reference: " + match.Value);
                    string resolved = Resolve(source[column], column, visiting);
                    visiting.Remove(action);
                    return resolved;
                });
            }

            for (int i = 0; i < lines.Length; i++)
            {
                string line = lines[i];
                if (string.IsNullOrWhiteSpace(line) || line.TrimStart().StartsWith("//")) continue;
                var fields = line.TrimEnd('\r').Split('\t', StringSplitOptions.RemoveEmptyEntries).Select(s => s.Trim()).ToArray();
                if (fields.Length < 10) continue;
                var right = new List<string>();
                var left = new List<string>();
                for (int column = 6; column <= 7; column++)
                foreach (string alternative in Resolve(fields[column], column, new()).Split(',', StringSplitOptions.RemoveEmptyEntries))
                {
                    var keys = alternative.Split('+').Select(NormalizeControllerHex).ToArray();
                    if (keys.Contains("0xff")) continue;
                    // Old pad/system codes must not acquire unintended Frame actions.
                    if (keys.Any(k => k is "0x0" or "0x3" or "0x5" or "0x6" or "0x23")) continue;
                    bool moveFace = column == 7 && keys.Any(k => k is "0x7" or "0x1");
                    if (moveFace)
                    {
                        // Whole face-button chords follow the face button to the
                        // right hand; controlmap chords cannot span two devices.
                        keys = keys.Select(k => k == "0x7" ? "0x5" : k == "0x1" ? "0x6" : k).ToArray();
                    }
                    (column == 6 || moveFace ? right : left).Add(string.Join('+', keys));
                }
                fields[6] = right.Count == 0 ? "0xff" : string.Join(',', right.Distinct());
                fields[7] = left.Count == 0 ? "0xff" : string.Join(',', left.Distinct());
                lines[i] = string.Join('\t', fields);
            }
            return string.Join('\n', lines);
        }

        private string BindingPresetDisplayName(string name) =>
            _controllerModelKey == "frame" && BindingPresetResources.ContainsKey(name) ? name + " (Steam Frame)" : name;
    }
}
