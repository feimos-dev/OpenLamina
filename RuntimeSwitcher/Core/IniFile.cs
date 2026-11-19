using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;

namespace RuntimeSwitcher.Core
{
    /// <summary>
    /// An openlamina.ini, changed one value at a time: every other line (comments, sections, options this app doesn't
    /// know, blank lines) is written back byte for byte. Lines are read as the library reads them: a name and a value
    /// around "=" or ":", comments from ";" or "#" at a line's start or " ;" after a value, an indented line as the
    /// previous name's value again, and only the options outside any section or in [default]. Where a name is set
    /// twice, the last one counts.
    /// </summary>
    public sealed class IniFile
    {
        public const string FileName = "openlamina.ini";
        public const string LegacyFileName = "opencomposite.ini";

        // One char per byte both ways, so text in any encoding survives.
        private static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

        // UTF-8's byte order mark, as Latin-1 reads it.
        private const string Bom = "ï»¿";

        // Each line with its line end.
        private readonly List<string> _lines = new List<string>();

        private struct Assignment
        {
            public int Line;
            public string Name;

            // The value's place in the line.
            public int Start, End;
        }

        private IniFile()
        {
        }

        /// <summary>
        /// The file the library reads in a folder: openlamina.ini, else an opencomposite.ini that is there, else the
        /// openlamina.ini to create.
        /// </summary>
        public static string InFolder(string folder)
        {
            string file = Path.Combine(folder, FileName);
            string legacy = Path.Combine(folder, LegacyFileName);
            return !File.Exists(file) && File.Exists(legacy) ? legacy : file;
        }

        /// <summary>Reads the file; one that isn't there is an empty one.</summary>
        public static IniFile Load(string path) =>
            Parse(File.Exists(path) ? Latin1.GetString(File.ReadAllBytes(path)) : "");

        public static IniFile Parse(string text)
        {
            var ini = new IniFile();
            for (int at = 0; at < text.Length;)
            {
                int end = text.IndexOf('\n', at);
                end = end < 0 ? text.Length : end + 1;
                ini._lines.Add(text.Substring(at, end - at));
                at = end;
            }

            return ini;
        }

        public string ToText() => string.Concat(_lines);

        /// <summary>The value the library reads for the name, or null when the file doesn't set it.</summary>
        public string Get(string name)
        {
            List<Assignment> found = Scan(out _);
            int last = found.FindLastIndex(a => a.Name == name);
            if (last < 0)
                return null;
            Assignment a = found[last];
            return _lines[a.Line].Substring(a.Start, a.End - a.Start);
        }

        /// <summary>
        /// Sets the value where the file has the name (the line's spacing and comment stay), else in a new line after
        /// the last of the options the library reads.
        /// </summary>
        public void Set(string name, string value)
        {
            List<Assignment> found = Scan(out int insertAt);
            int last = found.FindLastIndex(a => a.Name == name);
            if (last >= 0)
            {
                Assignment a = found[last];
                _lines[a.Line] = _lines[a.Line].Substring(0, a.Start) + value + _lines[a.Line].Substring(a.End);
                return;
            }

            string newLine = _lines.Any(l => l.EndsWith("\r\n", StringComparison.Ordinal)) ||
                             !_lines.Any(l => l.EndsWith("\n", StringComparison.Ordinal))
                ? "\r\n"
                : "\n";
            if (insertAt > 0 && !_lines[insertAt - 1].EndsWith("\n", StringComparison.Ordinal))
                _lines[insertAt - 1] += newLine;
            _lines.Insert(insertAt, name + "=" + value + newLine);
            if (insertAt == 0)
                KeepBomFirst();
        }

        /// <summary>Takes out every line that sets the name, so the library uses its default.</summary>
        public void Remove(string name)
        {
            foreach (int line in Scan(out _).Where(a => a.Name == name).Select(a => a.Line).Distinct()
                         .OrderByDescending(l => l))
            {
                if (line == 0 && _lines.Count > 1 && _lines[0].StartsWith(Bom, StringComparison.Ordinal))
                    _lines[1] = Bom + _lines[1];
                _lines.RemoveAt(line);
            }
        }

        /// <summary>Writes the file through a temporary file next to it, so a starting game never reads half of it.</summary>
        public void Save(string path)
        {
            string temp;
            using (Process self = Process.GetCurrentProcess())
                temp = $"{path}.{self.Id}.tmp";
            try
            {
                File.WriteAllBytes(temp, Latin1.GetBytes(ToText()));
                if (File.Exists(path))
                    File.Replace(temp, path, null);
                else
                    File.Move(temp, path);
            }
            finally
            {
                if (File.Exists(temp))
                    File.Delete(temp);
            }
        }

        private void KeepBomFirst()
        {
            if (_lines.Count > 1 && _lines[1].StartsWith(Bom, StringComparison.Ordinal))
            {
                _lines[1] = _lines[1].Substring(Bom.Length);
                _lines[0] = Bom + _lines[0];
            }
        }

        // C's isspace.
        private static bool IsSpace(char c) => c == ' ' || (c >= '\t' && c <= '\r');

        // The first of the chars from start, or the " ;" that begins a comment, or end.
        private static int Find(string line, int start, int end, string chars)
        {
            bool wasSpace = false;
            int at = start;
            while (at < end && (chars == null || chars.IndexOf(line[at]) < 0) && !(wasSpace && line[at] == ';'))
            {
                wasSpace = IsSpace(line[at]);
                at++;
            }

            return at;
        }

        /// <summary>
        /// Every line that sets an option the library reads, in the file's order. insertAt is the line a new option
        /// goes before: after the last line, blank ones aside, that is outside any section or in [default].
        /// </summary>
        private List<Assignment> Scan(out int insertAt)
        {
            var found = new List<Assignment>();
            insertAt = 0;
            string section = "", previous = null;
            for (int i = 0; i < _lines.Count; i++)
            {
                string line = _lines[i];
                int end = line.Length;
                while (end > 0 && IsSpace(line[end - 1]))
                    end--;
                int start = i == 0 && line.StartsWith(Bom, StringComparison.Ordinal) ? Bom.Length : 0;
                while (start < end && IsSpace(line[start]))
                    start++;
                if (start >= end)
                    continue;

                bool read = section == "" || section == "default";
                char first = line[start];
                if (first == ';' || first == '#')
                {
                }
                else if (!string.IsNullOrEmpty(previous) && start > 0)
                {
                    if (read)
                        found.Add(new Assignment { Line = i, Name = previous, Start = start, End = end });
                }
                else if (first == '[')
                {
                    int close = Find(line, start + 1, end, "]");
                    if (close < end && line[close] == ']')
                    {
                        section = line.Substring(start + 1, close - start - 1);
                        previous = null;
                        read = section == "" || section == "default";
                    }
                }
                else
                {
                    int separator = Find(line, start, end, "=:");
                    if (separator < end && line[separator] != ';')
                    {
                        previous = line.Substring(start, separator - start).TrimEnd(' ', '\t', '\v', '\f');
                        int comment = Find(line, separator + 1, end, null);
                        int valueStart = separator + 1, valueEnd = comment;
                        while (valueStart < comment && IsSpace(line[valueStart]))
                            valueStart++;
                        while (valueEnd > valueStart && IsSpace(line[valueEnd - 1]))
                            valueEnd--;
                        // No value yet: a new one goes right after the separator, before the space a comment needs.
                        if (valueStart == valueEnd)
                            valueStart = valueEnd = separator + 1;
                        if (read)
                            found.Add(new Assignment { Line = i, Name = previous, Start = valueStart, End = valueEnd });
                    }
                }

                if (read)
                    insertAt = i + 1;
            }

            return found;
        }
    }
}
