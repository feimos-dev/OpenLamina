using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace RuntimeSwitcher.Core
{
    public enum SettingKind
    {
        /// <summary>"true" or "false".</summary>
        Bool,

        /// <summary>One of <see cref="Setting.Choices"/>.</summary>
        Choice,

        /// <summary>A decimal number.</summary>
        Number,
    }

    /// <summary>One option of openlamina.ini that the settings window shows.</summary>
    public sealed class Setting
    {
        /// <summary>The option's name in the file.</summary>
        public string Name { get; }

        public string Group { get; }
        public string Label { get; }
        public string Help { get; }
        public SettingKind Kind { get; }

        /// <summary>The value the library uses when the file doesn't set the option. Empty: one the file can't name.</summary>
        public string Default { get; }

        /// <summary>A choice's values as the file has them, each with the text shown for it.</summary>
        public IReadOnlyList<KeyValuePair<string, string>> Choices { get; }

        internal Setting(string group, string name, SettingKind kind, string @default, string label, string help,
            params string[] choices)
        {
            Group = group;
            Name = name;
            Kind = kind;
            Default = @default;
            Label = label;
            Help = help;
            var pairs = new List<KeyValuePair<string, string>>();
            for (int i = 0; i + 1 < choices.Length; i += 2)
                pairs.Add(new KeyValuePair<string, string>(choices[i], choices[i + 1]));
            Choices = pairs;
        }

        /// <summary>
        /// The value in effect for what the file has (null for nothing), as the library reads it: a value it can't
        /// read leaves the default.
        /// </summary>
        public string Read(string raw)
        {
            if (raw == null)
                return Default;
            switch (Kind)
            {
                case SettingKind.Bool:
                    switch (raw.ToLowerInvariant())
                    {
                        case "true":
                        case "on":
                        case "enabled":
                            return "true";
                        case "false":
                        case "off":
                        case "disabled":
                            return "false";
                        default:
                            return Default;
                    }
                case SettingKind.Choice:
                    string lower = raw.ToLowerInvariant();
                    return Choices.Any(c => c.Key == lower) ? lower : Default;
                default:
                    return decimal.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out decimal number)
                        ? Format(number)
                        : Default;
            }
        }

        /// <summary>The default as the window shows it: "on", "off", a choice's value in the file, or a number.</summary>
        public string DefaultText
        {
            get
            {
                switch (Kind)
                {
                    case SettingKind.Bool:
                        return Default == "true" ? "on" : "off";
                    case SettingKind.Choice:
                        return Default.Length > 0
                            ? Default
                            : Choices.First(c => c.Key == Default).Value.ToLowerInvariant();
                    default:
                        return Default;
                }
            }
        }

        /// <summary>A number as the file gets it: a point whatever the user's locale, at least one decimal.</summary>
        public static string Format(decimal number) => number.ToString("0.0###", CultureInfo.InvariantCulture);
    }

    /// <summary>
    /// The options of openlamina.ini a player would change, in the settings window's order. Their names, values and
    /// defaults are the library's (OpenOVR/Misc/Config.h and Config.cpp); the options left out are workarounds and
    /// logging, set in the file by hand.
    /// </summary>
    public static class Settings
    {
        private const string Tools = "Overlay tools";
        private const string Input = "Input";
        private const string Picture = "Picture";
        private const string Games = "Game behaviour";

        public static IReadOnlyList<Setting> All { get; } = new[]
        {
            Bool(Tools, "helperApps", true, "Run overlay tools beside the game",
                "Overlay and background applications (desktop viewers, playspace movers) get sessions of their own, " +
                "where the OpenXR runtime has overlay sessions."),
            new Setting(Tools, "dashboardButton", SettingKind.Choice, "system", "Dashboard opens with",
                "OpenLamina's dashboard shows the dashboard overlays of the tools running beside the game, and a " +
                "\"Quit game\" button. The same button closes it. The system button works on controllers that " +
                "have one (Index, Vive wands) where the OpenXR runtime passes it on; the left menu button is B on " +
                "Index controllers. With \"System button, pressed\", holding it for a second recentres instead.",
                "off", "Nothing (no dashboard)", "system", "System button, pressed", "system_long",
                "System button, held", "menu", "Left menu button, pressed", "menu_long", "Left menu button, held"),
            Bool(Tools, "laserMouse", true, "Point at overlays with the controllers",
                "Overlays that take mouse input get a laser pointer from the controllers; the trigger clicks."),
            Bool(Tools, "keyboard", true, "Show the keyboard when a game asks for one",
                "Typed on by pointing at it with the controllers. When off, the game is told at once that typing is " +
                "done."),
            new Setting(Tools, "keyboardLayout", SettingKind.Choice, "", "Keyboard layout",
                "Automatic goes by your locale: UK for British English, US otherwise.",
                "", "Automatic", "us", "US", "uk", "UK"),

            new Setting(Input, "inputEvaluator", SettingKind.Choice, "steamvr", "Controller bindings",
                "With SteamVR's binding modes, toggles, d-pads, chords and legacy input behave as in SteamVR. Try " +
                "plain OpenXR bindings when a game's controls don't work.",
                "direct", "As OpenXR bindings", "steamvr", "SteamVR's binding modes"),
            Bool(Input, "haptics", true, "Controller vibration", "Haptic feedback from games."),
            Bool(Input, "eyeTracking", true, "Give games the eye gaze",
                "For games with eye-tracked foveated rendering, on headsets with an eye tracker."),

            new Setting(Picture, "supersampleRatio", SettingKind.Number, "1.0", "Render scale",
                "As SteamVR's resolution setting: 1.45 is 145%. Higher is sharper and slower."),
            Bool(Picture, "renderCustomHands", true, "Draw OpenLamina's hand models",
                "Some games have models of their own, which show when this is off."),
            Bool(Picture, "steamvrRenderModels", true, "Use SteamVR's controller models",
                "From your SteamVR install, with their moving parts. When off, OpenLamina's built-in models."),
            Bool(Picture, "drawBounds", true, "Show the play area's edge when near it",
                "A grid wall, for OpenXR runtimes that show no boundary of their own."),
            Bool(Picture, "mirrorWindow", false, "Desktop mirror window for games that ask for one",
                "Shows the game's left eye. Costs some frame time on a headset of high resolution."),

            Bool(Games, "ownFrames", true, "Keep overlays and the skybox showing while a game loads",
                "While the game runs no frames, OpenLamina runs frames of its own: the game's last frame with its " +
                "overlays, or its skybox."),
            Bool(Games, "sessionEvents", true, "Tell games when focus is lost or the headset is taken off",
                "The events SteamVR sends. Some games pause on them."),
            Bool(Games, "applyZeroPoses", true, "Let recentring tools move the play space (Monado)",
                "A standing zero pose that a program sets moves Monado's stage, for every program, until Monado " +
                "restarts."),
            Bool(Games, "useSubmittedPose", true, "Use the head pose a game submits with its frames",
                "For games that submit the pose they rendered with."),
            Bool(Games, "submitDepth", true, "Pass a game's depth to the OpenXR runtime",
                "For games that submit depth, on runtimes that take it (for reprojection)."),
        };

        private static Setting Bool(string group, string name, bool @default, string label, string help) =>
            new Setting(group, name, SettingKind.Bool, @default ? "true" : "false", label, help);
    }
}
