using System.Buffers;
// ConsoleBgTool - configuration model and parser.
//
// The configuration is deliberately a plain key=value file with no scripting, no includes and
// no expression evaluation. Every value is length-limited, range-checked and validated by the
// native core before it can reach a console, so a malformed or hostile configuration results
// in a diagnostic rather than a changed console.
using System.Globalization;
using System.Text;
using ConsoleBgTool.Interop;

namespace ConsoleBgTool.Cli;

/// <summary>Thrown for configuration input the user needs to fix.</summary>
public sealed class ConfigurationException : Exception
{
    public ConfigurationException(string message) : base(message) { }
}

/// <summary>
/// A validated appearance configuration. Instances are created only through
/// <see cref="Parse"/> or the preset helpers, so every field is known good.
/// </summary>
public sealed class Appearance
{
    public const int ColorTableSize = NativeConfig.ColorTableSize;
    public const int MinOpacity = 10;
    public const int MaxOpacity = 100;

    public uint[] Palette { get; } = new uint[ColorTableSize];
    public int ForegroundSlot { get; set; } = 7;
    public int BackgroundSlot { get; set; }
    public int CanvasSlot { get; set; }
    public ushort BufferColumns { get; set; } = 120;
    public ushort BufferRows { get; set; } = 30;
    public ushort WindowColumns { get; set; } = 120;
    public ushort WindowRows { get; set; } = 30;
    public ushort FontSize { get; set; } = 16;
    public ushort FontWeight { get; set; } = 400;
    public uint FontFamily { get; set; } = 54;
    public string FaceName { get; set; } = string.Empty;
    public int OpacityPercent { get; set; } = 100;

    /// <summary>The unmodified Windows 11 default palette, used as the starting point.</summary>
    public static Appearance CreateDefault()
    {
        NativeConfig native = NativeMethods.DefaultConfig();
        var appearance = new Appearance();
        Array.Copy(native.Palette, appearance.Palette, ColorTableSize);
        appearance.ForegroundSlot = native.ForegroundSlot;
        appearance.BackgroundSlot = native.BackgroundSlot;
        appearance.CanvasSlot = native.CanvasSlot;
        appearance.BufferColumns = native.BufferColumns;
        appearance.BufferRows = native.BufferRows;
        appearance.WindowColumns = native.WindowColumns;
        appearance.WindowRows = native.WindowRows;
        appearance.FontSize = native.FontSize;
        appearance.FontWeight = native.FontWeight;
        appearance.FontFamily = native.FontFamily;
        appearance.FaceName = native.FaceName ?? string.Empty;
        appearance.OpacityPercent = native.OpacityPercent;
        return appearance;
    }

    /// <summary>
    /// Parses a key=value configuration.
    ///
    /// <para>
    /// Accepted keys: <c>background</c>, <c>foreground</c>, <c>colour.&lt;slot&gt;</c>,
    /// <c>palette.&lt;slot&gt;</c>, <c>opacity</c>, <c>columns</c>, <c>rows</c>,
    /// <c>window-columns</c>, <c>window-rows</c>, <c>font-size</c>, <c>font-weight</c>,
    /// <c>font-family</c>, <c>face-name</c>. Lines starting with <c>#</c> or <c>;</c> are
    /// comments.
    /// </para>
    /// </summary>
    public static Appearance Parse(string text, Appearance? basis = null)
    {
        Appearance appearance = basis is null ? CreateDefault() : basis.Clone();
        int lineNumber = 0;

        foreach (string rawLine in text.Split('\n'))
        {
            lineNumber++;
            string line = rawLine.Trim();
            if (line.Length == 0 || line[0] == '#' || line[0] == ';')
            {
                continue;
            }

            int separator = line.IndexOf('=', StringComparison.Ordinal);
            if (separator <= 0)
            {
                throw new ConfigurationException(
                    $"line {lineNumber}: expected 'key=value', found '{Sanitize(line)}'");
            }

            string key = line[..separator].Trim().ToLowerInvariant();
            string value = line[(separator + 1)..].Trim();

            try
            {
                appearance.Apply(key, value);
            }
            catch (Exception ex) when (ex is ConfigurationException or FormatException or OverflowException)
            {
                throw new ConfigurationException($"line {lineNumber}: {ex.Message}");
            }
        }

        return appearance;
    }

    private void Apply(string key, string value)
    {
        if (key is "background" or "bg")
        {
            CanvasSlot = ParseSlot(value);
            return;
        }
        if (key is "foreground" or "fg")
        {
            ForegroundSlot = ParseSlot(value);
            return;
        }
        if (key.StartsWith("colour.", StringComparison.Ordinal) ||
            key.StartsWith("color.", StringComparison.Ordinal) ||
            key.StartsWith("palette.", StringComparison.Ordinal))
        {
            int dot = key.IndexOf('.', StringComparison.Ordinal);
            string slotText = key[(dot + 1)..];
            int slot = ParseSlot(slotText);
            Palette[slot] = ParseColor(value);
            return;
        }
        if (key is "opacity")
        {
            OpacityPercent = ParseInt(value, MinOpacity, MaxOpacity, "opacity");
            return;
        }
        if (key is "columns")
        {
            BufferColumns = (ushort)ParseInt(value, 20, 512, "columns");
            return;
        }
        if (key is "rows")
        {
            BufferRows = (ushort)ParseInt(value, 5, 9999, "rows");
            return;
        }
        if (key is "window-columns")
        {
            WindowColumns = (ushort)ParseInt(value, 20, 512, "window-columns");
            return;
        }
        if (key is "window-rows")
        {
            WindowRows = (ushort)ParseInt(value, 5, 9999, "window-rows");
            return;
        }
        if (key is "font-size")
        {
            FontSize = (ushort)ParseInt(value, 1, 200, "font-size");
            return;
        }
        if (key is "font-weight")
        {
            FontWeight = (ushort)ParseInt(value, 0, 1000, "font-weight");
            return;
        }
        if (key is "font-family")
        {
            FontFamily = (uint)ParseInt(value, 0, int.MaxValue, "font-family");
            return;
        }
        if (key is "face-name")
        {
            /*
             * The value has to fit the console's LOGFONTW face-name field, which is 32 bytes
             * including the terminator. This is checked here as well as in the core because the
             * native structure declares a fixed 32-character buffer: a longer string is silently
             * truncated by the marshaller before the core could ever reject it, so the only place
             * the limit can be enforced honestly is on this side.
             */
            if (value.Length >= NativeConfig.MaxFaceName)
            {
                throw new ConfigurationException(
                    $"face-name is {value.Length} characters; the console allows at most " +
                    $"{NativeConfig.MaxFaceName - 1}");
            }
            FaceName = value;
            return;
        }

        throw new ConfigurationException($"unknown setting '{Sanitize(key)}'");
    }

    private static string Sanitize(string text)
    {
        // Strip control characters before echoing input back, so a crafted value cannot inject
        // terminal escape sequences into the diagnostic output.
        var builder = new StringBuilder(text.Length);
        foreach (char ch in text)
        {
            builder.Append(char.IsControl(ch) ? '?' : ch);
        }
        string result = builder.ToString();
        return result.Length > 64 ? result[..64] + "..." : result;
    }

    private static int ParseSlot(string value)
    {
        string trimmed = value.Trim().ToLowerInvariant();

        // A numeric slot is the natural form; a slot name is the readable one.
        if (int.TryParse(trimmed, NumberStyles.Integer, CultureInfo.InvariantCulture, out int numeric))
        {
            if (numeric < 0 || numeric >= ColorTableSize)
            {
                throw new ConfigurationException($"slot {numeric} is outside 0..{ColorTableSize - 1}");
            }
            return numeric;
        }

        return trimmed switch
        {
            "black" => 0,
            "dark-blue" or "darkblue" => 1,
            "dark-green" or "darkgreen" => 2,
            "dark-cyan" or "darkcyan" => 3,
            "dark-red" or "darkred" => 4,
            "dark-magenta" or "darkmagenta" => 5,
            "dark-yellow" or "darkyellow" => 6,
            "gray" or "grey" => 7,
            "dark-gray" or "darkgrey" or "dark-grey" => 8,
            "blue" => 9,
            "green" => 10,
            "cyan" => 11,
            "red" => 12,
            "magenta" => 13,
            "yellow" => 14,
            "white" => 15,
            _ => throw new ConfigurationException($"unknown colour slot '{Sanitize(trimmed)}'"),
        };
    }

    private static uint ParseColor(string value)
    {
        CbtStatus status = NativeMethods.ParseColor(value, out uint color);
        if (status != CbtStatus.Ok)
        {
            throw new ConfigurationException($"'{Sanitize(value)}' is not a colour ({status})");
        }
        return color;
    }

    private static int ParseInt(string value, int minimum, int maximum, string what)
    {
        if (!int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsed))
        {
            throw new ConfigurationException($"{what}: '{Sanitize(value)}' is not a number");
        }
        if (parsed < minimum || parsed > maximum)
        {
            throw new ConfigurationException($"{what}: {parsed} is outside {minimum}..{maximum}");
        }
        return parsed;
    }

    public Appearance Clone()
    {
        var copy = new Appearance
        {
            ForegroundSlot = ForegroundSlot,
            BackgroundSlot = BackgroundSlot,
            CanvasSlot = CanvasSlot,
            BufferColumns = BufferColumns,
            BufferRows = BufferRows,
            WindowColumns = WindowColumns,
            WindowRows = WindowRows,
            FontSize = FontSize,
            FontWeight = FontWeight,
            FontFamily = FontFamily,
            FaceName = FaceName,
            OpacityPercent = OpacityPercent,
        };
        Array.Copy(Palette, copy.Palette, ColorTableSize);
        return copy;
    }

    /// <summary>Converts to the native structure, validating through the native core first.</summary>
    public NativeConfig ToNative()
    {
        var native = new NativeConfig
        {
            Palette = new uint[ColorTableSize],
            ForegroundSlot = ForegroundSlot,
            BackgroundSlot = BackgroundSlot,
            CanvasSlot = CanvasSlot,
            BufferColumns = BufferColumns,
            BufferRows = BufferRows,
            WindowColumns = WindowColumns,
            WindowRows = WindowRows,
            FontSize = FontSize,
            FontWeight = FontWeight,
            FontFamily = FontFamily,
            FaceName = FaceName,
            OpacityPercent = OpacityPercent,
        };
        Array.Copy(Palette, native.Palette, ColorTableSize);

        CbtStatus status = NativeMethods.Validate(ref native, out string field);
        if (status != CbtStatus.Ok)
        {
            throw new ConfigurationException($"invalid configuration: {status} in '{field}'");
        }
        return native;
    }

    public static Appearance FromNative(NativeConfig native)
    {
        var appearance = new Appearance
        {
            ForegroundSlot = native.ForegroundSlot,
            BackgroundSlot = native.BackgroundSlot,
            CanvasSlot = native.CanvasSlot,
            BufferColumns = native.BufferColumns,
            BufferRows = native.BufferRows,
            WindowColumns = native.WindowColumns,
            WindowRows = native.WindowRows,
            FontSize = native.FontSize,
            FontWeight = native.FontWeight,
            FontFamily = native.FontFamily,
            FaceName = native.FaceName ?? string.Empty,
            OpacityPercent = native.OpacityPercent,
        };
        Array.Copy(native.Palette, appearance.Palette, ColorTableSize);
        return appearance;
    }

    /// <summary>Contrast between the default text colour and the window background.</summary>
    public double ContrastRatio()
    {
        NativeConfig native = ToNative();
        CbtStatus status = NativeMethods.Contrast(ref native, out double ratio);
        return status == CbtStatus.Ok ? ratio : 0.0;
    }

    /// <summary>Relative luminance of the window background, 0 (black) to 1 (white).</summary>
    public double CanvasLuminance()
    {
        uint canvas = Palette[Math.Clamp(CanvasSlot, 0, ColorTableSize - 1)];
        static double Channel(byte value) => value / 255.0;
        static double Linear(double c) => c <= 0.03928 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4);
        double r = Linear(Channel((byte)((canvas >> 16) & 0xFF)));
        double g = Linear(Channel((byte)((canvas >> 8) & 0xFF)));
        double b = Linear(Channel((byte)(canvas & 0xFF)));
        return (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
    }

    public bool IsLightBackground => CanvasLuminance() >= 0.5;

    public static string ToHex(uint color) => $"#{color & 0xFFFFFF:X6}";

    /* --------------------------------------------------------------------- */
    /* Command line parsing helpers                                           */
    /* --------------------------------------------------------------------- */

    /// <summary>Parses a slot given either as a number 0-15 or as a slot name.</summary>
    public static int ParseSlotText(string text) => ParseSlot(text);

    /// <summary>
    /// Applies "<c>SLOT=COLOR</c>", as accepted by <c>--color</c>. Both halves are validated, so a
    /// malformed override is reported rather than partially applied.
    /// </summary>
    public static void ApplyColorOverride(Appearance appearance, string spec)
    {
        int separator = spec.IndexOf('=', StringComparison.Ordinal);
        if (separator <= 0 || separator == spec.Length - 1)
        {
            throw new ConfigurationException($"--color expects SLOT=COLOR, found '{Sanitize(spec)}'");
        }

        string slotText = spec[..separator].Trim();
        string colorText = spec[(separator + 1)..].Trim();

        int slot = ParseSlot(slotText);
        CbtStatus status = NativeMethods.ParseColor(colorText, out uint color);
        if (status != CbtStatus.Ok)
        {
            throw new ConfigurationException($"--color: '{Sanitize(colorText)}' is not a colour ({status})");
        }
        appearance.Palette[slot] = color;
    }

    /// <summary>Separators accepted in a "<c>COLSxROWS</c>" geometry specification.</summary>
    private static readonly SearchValues<char> GeometrySeparators =
        SearchValues.Create("xX:");

    /// <summary>Applies "<c>COLSxROWS</c>" or "<c>COLS:ROWS</c>", as accepted by <c>--geometry</c>.</summary>
    public static void ApplyGeometryOverride(Appearance appearance, string spec)
    {
        int separator = spec.AsSpan().IndexOfAny(GeometrySeparators);
        if (separator <= 0 || separator == spec.Length - 1)
        {
            throw new ConfigurationException($"--geometry expects COLSxROWS, found '{Sanitize(spec)}'");
        }

        int columns = ParseInt(spec[..separator].Trim(), 20, 512, "geometry columns");
        int rows = ParseInt(spec[(separator + 1)..].Trim(), 5, 9999, "geometry rows");

        appearance.BufferColumns = (ushort)columns;
        appearance.BufferRows = (ushort)rows;
        /*
         * The window is sized to the buffer. A larger window than buffer is refused by the console,
         * so keeping the two equal is the one combination that is always accepted.
         */
        appearance.WindowColumns = (ushort)columns;
        appearance.WindowRows = (ushort)rows;
    }

    /* --------------------------------------------------------------------- */
    /* Presets                                                                */
    /* --------------------------------------------------------------------- */

    private static readonly (string Name, string Description)[] PresetCatalog =
    {
        ("default", "the stock Windows 11 palette, unchanged"),
        ("black", "pure black background, light grey text"),
        ("charcoal", "soft charcoal background, easy on the eyes"),
        ("navy", "deep blue background"),
        ("forest", "dark green background"),
        ("plum", "dark purple background"),
        ("solarized-dark", "Solarized Dark: base03 background"),
        ("nord", "Nord: polar night background"),
        ("gruvbox-dark", "Gruvbox Dark: warm dark background"),
        ("paper", "light background, dark text (dark-on-light)"),
        ("high-contrast", "white on pure black, maximum contrast"),
    };

    public static IEnumerable<(string Name, string Description)> PresetNames() => PresetCatalog;

    /// <summary>
    /// Builds a preset by name. Names are matched case-insensitively; an unknown name is reported
    /// with the list of valid ones, so a typo is never silently treated as the default.
    /// </summary>
    public static Appearance FromPreset(string name)
    {
        string key = name.Trim().ToLowerInvariant();
        if (!PresetCatalog.Any(entry => entry.Name == key))
        {
            throw new ConfigurationException(
                $"unknown preset '{Sanitize(name)}'; available: " +
                string.Join(", ", PresetCatalog.Select(entry => entry.Name)));
        }

        Appearance appearance = CreateDefault();

        // Overrides are packed as (slot << 24) | RRGGBB so a preset reads as a list of choices.
        static uint At(int slot, uint color) => ((uint)slot << 24) | (color & 0xFFFFFF);

        void Set(int background, int foreground, uint[] overrides)
        {
            appearance.CanvasSlot = background;
            appearance.BackgroundSlot = background;
            appearance.ForegroundSlot = foreground;
            foreach (uint value in overrides)
            {
                appearance.Palette[(int)(value >> 24)] = value & 0xFFFFFF;
            }
        }

        switch (key)
        {
            case "default":
                break;

            case "black":
                Set(0, 7, new[] { At(0, 0x000000) });
                break;

            case "charcoal":
                Set(0, 7, new[] { At(0, 0x1B1B1B), At(8, 0x6E6E6E) });
                break;

            case "navy":
                Set(0, 7, new[] { At(0, 0x0A1A33), At(1, 0x2A4B8D), At(8, 0x4A6FA5) });
                break;

            case "forest":
                Set(0, 7, new[] { At(0, 0x0E2010), At(2, 0x2E7D32), At(8, 0x4F7F52) });
                break;

            case "plum":
                Set(0, 7, new[] { At(0, 0x1E1024), At(5, 0x9C27B0), At(8, 0x7E6A85) });
                break;

            case "solarized-dark":
                // Solarized: base03 #002b36, base0 #839496, base01 #586e75, plus the accent set.
                Set(0, 7, new[]
                {
                    At(0, 0x002B36), At(7, 0x839496), At(8, 0x586E75),
                    At(1, 0x268BD2), At(2, 0x859900), At(3, 0x2AA198),
                    At(4, 0xDC322F), At(5, 0xD33682), At(6, 0xB58900),
                });
                break;

            case "nord":
                // Nord: nord0 #2E3440, nord4 #D8DEE9, nord3 #4C566A, plus the accent set.
                Set(0, 7, new[]
                {
                    At(0, 0x2E3440), At(7, 0xD8DEE9), At(8, 0x4C566A),
                    At(1, 0x81A1C1), At(2, 0xA3BE8C), At(3, 0x88C0D0),
                    At(4, 0xBF616A), At(5, 0xB48EAD), At(6, 0xEBCB8B),
                });
                break;

            case "gruvbox-dark":
                // Gruvbox Dark: bg0 #282828, fg1 #EBDBB2, bg2 #504945, plus the accent set.
                Set(0, 7, new[]
                {
                    At(0, 0x282828), At(7, 0xEBDBB2), At(8, 0x504945),
                    At(1, 0x83A598), At(2, 0xB8BB26), At(3, 0x8EC07C),
                    At(4, 0xFB4934), At(5, 0xD3869B), At(6, 0xFABD2F),
                });
                break;

            case "paper":
                /*
                 * Dark text on a light canvas: the inverted theme.
                 *
                 * The slot assignment is the whole trick and is easy to get backwards. A console
                 * paints its background with the palette entry the DEFAULT ATTRIBUTE's background
                 * nibble selects, not with slot 0 of the palette. The light colour therefore goes
                 * into the slot named by background_slot (here 7 = gray) and the dark colour into the
                 * slot named by foreground_slot (here 0 = black). Putting the light colour in slot 0
                 * instead leaves the window dark with light text, which is the opposite of intent.
                 */
                Set(7, 0, new[]
                {
                    At(0, 0x2B2B2B), At(7, 0xF7F3E8), At(8, 0x8A8578),
                    At(1, 0x1F3F9F), At(2, 0x14631A), At(3, 0x11607A),
                    At(4, 0xA03028), At(5, 0x7A1F70), At(6, 0x7A5B00),
                });
                break;

            case "high-contrast":
                // White text (slot 15) on a pure black background (slot 0).
                Set(0, 15, new[] { At(0, 0x000000), At(7, 0xFFFFFF), At(15, 0xFFFFFF) });
                break;
        }

        return appearance;
    }

    /// <summary>
    /// Returns a copy whose text colour reaches at least <paramref name="minimumRatio"/> against the
    /// canvas, by choosing the palette entry with the highest contrast. Used by the tests to assert
    /// that every shipped preset is legible rather than merely pretty.
    /// </summary>
    public Appearance WithReadableText(double minimumRatio = 4.5)
    {
        if (ContrastRatio() >= minimumRatio)
        {
            return this;
        }

        Appearance copy = Clone();
        int bestSlot = copy.ForegroundSlot;
        double bestRatio = 0;

        for (int slot = 0; slot < ColorTableSize; slot++)
        {
            double ratio = ContrastRatioBetween(copy.Palette[slot], copy.Palette[copy.CanvasSlot]);
            if (ratio > bestRatio)
            {
                bestRatio = ratio;
                bestSlot = slot;
            }
        }

        copy.ForegroundSlot = bestSlot;
        return copy;
    }

    /// <summary>Contrast ratio between two colours, using the WCAG 2.x definition.</summary>
    public static double ContrastRatioBetween(uint a, uint b)
    {
        static double Channel(byte value) => value / 255.0;
        static double Linear(double c) => c <= 0.03928 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4);
        static double Luminance(uint color)
        {
            double r = Linear(Channel((byte)((color >> 16) & 0xFF)));
            double g = Linear(Channel((byte)((color >> 8) & 0xFF)));
            double b = Linear(Channel((byte)(color & 0xFF)));
            return (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
        }

        double la = Luminance(a);
        double lb = Luminance(b);
        double lighter = Math.Max(la, lb);
        double darker = Math.Min(la, lb);
        return (lighter + 0.05) / (darker + 0.05);
    }
}
