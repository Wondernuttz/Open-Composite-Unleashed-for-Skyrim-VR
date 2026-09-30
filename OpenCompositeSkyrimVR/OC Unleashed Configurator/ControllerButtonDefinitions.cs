using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace OpenCompositeConfigurator
{
    internal static class ControllerButtonDefinitions
    {
        // The shipped file is also embedded so copying/updating the Configurator
        // cannot leave newly assigned Frame buttons without native hold tracking.
        internal static void Ensure(string controlsDirectory)
        {
            using var stream = typeof(ControllerButtonDefinitions).Assembly.GetManifestResourceStream(
                "OpenCompositeConfigurator.oculuscontroller.txt")
                ?? throw new InvalidOperationException("Controller button definitions are missing from the Configurator.");
            using var reader = new StreamReader(stream);
            string path = Path.Combine(controlsDirectory, "oculuscontroller.txt");
            string original = File.Exists(path) ? File.ReadAllText(path) : "";
            string merged = Merge(original, reader.ReadToEnd());
            if (merged == original) return;
            Directory.CreateDirectory(controlsDirectory);
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllText(temporary, merged, new UTF8Encoding(false));
                File.Move(temporary, path, true);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }

        internal static string Merge(string original, string required)
        {
            // Skyrim uses strtok_s(line, "\t\r", ...), not a whitespace parser.
            // Repair the space-separated rows written by earlier OCU builds.
            // Preserve valid tab-separated names (which may contain spaces),
            // optional Flash codes, comments and custom IDs.
            static string Normalize(string text)
            {
                var output = new StringBuilder();
                using var lines = new StringReader(text);
                string? line;
                while ((line = lines.ReadLine()) != null)
                {
                    if (string.IsNullOrWhiteSpace(line) || line.TrimStart().StartsWith("//"))
                    {
                        output.AppendLine(line.TrimStart());
                        continue;
                    }
                    string[] fields = line.Contains('\t')
                        ? line.Split('\t', StringSplitOptions.RemoveEmptyEntries)
                        : line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                    output.AppendLine(string.Join("\t", Array.ConvertAll(fields, f => f.Trim())));
                }
                return output.ToString();
            }

            static Dictionary<int, string> Parse(string text)
            {
                var result = new Dictionary<int, string>();
                var names = new HashSet<string>(StringComparer.Ordinal);
                foreach (string line in text.Split('\n'))
                {
                    if (string.IsNullOrWhiteSpace(line) || line.StartsWith("//")) continue;
                    string[] fields = line.TrimEnd('\r').Split('\t', StringSplitOptions.RemoveEmptyEntries);
                    if (fields.Length == 0) continue;
                    if ((fields.Length != 2 && fields.Length != 3) || !int.TryParse(
                        fields[1].StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? fields[1][2..] : fields[1],
                        NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int id) || id < 0 || id >= 64 ||
                        !result.TryAdd(id, fields[0]) || !names.Add(fields[0]))
                        throw new IOException("oculuscontroller.txt contains an invalid or duplicate button definition. Resolve that file before saving bindings.");
                }
                return result;
            }

            original = Normalize(original);
            var existing = Parse(original);
            var canonical = Parse(Normalize(required));
            var additions = new StringBuilder();
            foreach (var entry in canonical)
            {
                if (existing.TryGetValue(entry.Key, out string? name))
                {
                    // Extra button names may be supplied by another compatible
                    // definition file. Stock Skyrim names must retain their IDs.
                    if ((entry.Key is 1 or 2 or 7 or 32 or 33) && name != entry.Value)
                        throw new IOException($"oculuscontroller.txt has a conflicting definition for {entry.Value}. Resolve that file before saving bindings.");
                    continue;
                }
                if (existing.ContainsValue(entry.Value))
                    throw new IOException($"oculuscontroller.txt assigns {entry.Value} to the wrong button ID.");
                additions.Append(entry.Value).Append("\t0x").Append(entry.Key.ToString("X4")).Append("\r\n");
            }
            if (additions.Length == 0) return original;
            return original + (original.Length > 0 && !original.EndsWith('\n') ? "\r\n" : "") + additions;
        }
    }
}
