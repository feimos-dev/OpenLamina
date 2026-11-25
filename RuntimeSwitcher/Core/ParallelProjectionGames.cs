using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace RuntimeSwitcher.Core
{
    /// <summary>
    /// The games the library turns parallel projections on for by itself, from the file the library is built with
    /// (OpenOVR/Misc/ParallelProjectionGames.inc, embedded here).
    /// </summary>
    public static class ParallelProjectionGames
    {
        public const string ResourceName = "OpenLamina.ParallelProjectionGames.inc";

        private static readonly Lazy<IReadOnlyDictionary<string, string>> Embedded =
            new Lazy<IReadOnlyDictionary<string, string>>(() =>
            {
                using (Stream stream = typeof(ParallelProjectionGames).Assembly.GetManifestResourceStream(ResourceName))
                {
                    if (stream == null)
                        return new Dictionary<string, string>();
                    using (var reader = new StreamReader(stream))
                        return Parse(reader.ReadToEnd());
                }
            });

        /// <summary>The executable file names (any case) and game names in the file's text.</summary>
        public static IReadOnlyDictionary<string, string> Parse(string text)
        {
            var games = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (Match match in Regex.Matches(text, "^\\s*\\{\\s*\"([^\"]+)\"\\s*,\\s*\"([^\"]+)\"\\s*\\}",
                         RegexOptions.Multiline))
                games[match.Groups[1].Value] = match.Groups[2].Value;
            return games;
        }

        /// <summary>The game a program is, by its path, if it's one the library turns parallel projections on for.</summary>
        public static string Find(string exePath) => Find(Embedded.Value, exePath);

        public static string Find(IReadOnlyDictionary<string, string> games, string exePath)
        {
            if (string.IsNullOrEmpty(exePath))
                return null;
            string file = exePath.Substring(exePath.LastIndexOfAny(new[] { '/', '\\' }) + 1);
            return games.TryGetValue(file, out string name) ? name : null;
        }
    }
}
