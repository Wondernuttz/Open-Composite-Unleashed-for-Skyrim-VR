using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace OpenCompositeConfigurator
{
    /// <summary>
    /// Reads and writes opencomposite.ini files, preserving comments and structure.
    /// </summary>
    public class IniFile
    {
        private readonly List<IniLine> _lines = new();
        private string _filePath = "";

        public string FilePath => _filePath;

        public void Load(string path)
        {
            _filePath = path;
            _lines.Clear();

            if (!File.Exists(path))
                return;

            string currentSection = "";
            foreach (string rawLine in File.ReadAllLines(path))
            {
                string trimmed = rawLine.Trim();

                if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith(";") || trimmed.StartsWith("#"))
                {
                    _lines.Add(new IniLine { Raw = rawLine, Type = IniLineType.Comment });
                    continue;
                }

                if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
                {
                    currentSection = trimmed.Substring(1, trimmed.Length - 2).Trim().ToLowerInvariant();
                    _lines.Add(new IniLine { Raw = rawLine, Type = IniLineType.Section, Section = currentSection });
                    continue;
                }

                int eq = trimmed.IndexOf('=');
                if (eq > 0)
                {
                    string key = trimmed.Substring(0, eq).Trim();
                    string val = trimmed.Substring(eq + 1).Trim();
                    _lines.Add(new IniLine
                    {
                        Raw = rawLine,
                        Type = IniLineType.KeyValue,
                        Section = currentSection,
                        Key = key,
                        Value = val
                    });
                }
                else
                {
                    _lines.Add(new IniLine { Raw = rawLine, Type = IniLineType.Comment });
                }
            }
        }

        public string Get(string section, string key, string defaultValue = "")
        {
            section = section.ToLowerInvariant();
            foreach (var line in _lines)
            {
                if (line.Type == IniLineType.KeyValue &&
                    line.Section == section &&
                    string.Equals(line.Key, key, StringComparison.OrdinalIgnoreCase))
                {
                    return line.Value;
                }
            }
            return defaultValue;
        }

        public void Set(string section, string key, string value)
        {
            section = section.ToLowerInvariant();

            // Try to update existing key
            foreach (var line in _lines)
            {
                if (line.Type == IniLineType.KeyValue &&
                    line.Section == section &&
                    string.Equals(line.Key, key, StringComparison.OrdinalIgnoreCase))
                {
                    line.Value = value;
                    line.Raw = $"{key}={value}";
                    return;
                }
            }

            // Key doesn't exist — find the section and append, or create section
            int sectionIdx = -1;
            int lastKeyInSection = -1;
            for (int i = 0; i < _lines.Count; i++)
            {
                if (_lines[i].Type == IniLineType.Section && _lines[i].Section == section)
                    sectionIdx = i;

                if (sectionIdx >= 0 && _lines[i].Section == section && _lines[i].Type == IniLineType.KeyValue)
                    lastKeyInSection = i;
            }

            var newLine = new IniLine
            {
                Raw = $"{key}={value}",
                Type = IniLineType.KeyValue,
                Section = section,
                Key = key,
                Value = value
            };

            if (sectionIdx >= 0)
            {
                int insertAt = lastKeyInSection >= 0 ? lastKeyInSection + 1 : sectionIdx + 1;
                _lines.Insert(insertAt, newLine);
            }
            else
            {
                // Create new section
                if (_lines.Count > 0)
                    _lines.Add(new IniLine { Raw = "", Type = IniLineType.Comment });

                _lines.Add(new IniLine
                {
                    Raw = $"[{section}]",
                    Type = IniLineType.Section,
                    Section = section
                });
                _lines.Add(newLine);
            }
        }

        public void Save()
        {
            Save(_filePath);
        }

        public void Save(string path)
        {
            _filePath = path;
            var sb = new StringBuilder();
            foreach (var line in _lines)
                sb.AppendLine(line.Raw);
            File.WriteAllText(path, sb.ToString());
        }

        private enum IniLineType { Comment, Section, KeyValue }

        private class IniLine
        {
            public string Raw = "";
            public IniLineType Type;
            public string Section = "";
            public string Key = "";
            public string Value = "";
        }
    }
}
