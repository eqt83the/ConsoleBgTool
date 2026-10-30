// ConsoleBgTool - read-only report of HKCU\Console.
//
// This type exists for reference and backup only. The tool never writes these values, and the
// reason is worth recording: on Windows 11 (verified on build 26100) a newly created console
// ignores the HKCU\Console colour table, screen buffer size and window size, so writing them would
// change nothing about how a console looks while making the console's own Properties dialog
// disagree with reality. Reading them is still useful, for documenting the original state or
// diffing before and after a manual change.
//
// Everything here is read-only: the key is opened with read access and nothing is ever written.
using System.Globalization;
using Microsoft.Win32;

namespace ConsoleBgTool.Cli;

/// <summary>A snapshot of the values in <c>HKCU\Console</c>.</summary>
internal sealed class RegistryReport
{
    private const string ConsoleKeyPath = @"Console";

    /// <summary>
    /// The values that determine the stock appearance, in the order a console's own Properties
    /// dialog presents them.
    /// </summary>
    private static readonly string[] AppearanceValueNames =
    {
        "ScreenColors", "PopupColors", "WindowAlpha", "CursorSize", "CursorColor",
        "DefaultForeground", "DefaultBackground", "FaceName", "FontFamily", "FontSize",
        "FontWeight", "ScreenBufferSize", "WindowSize", "ForceV2",
    };

    public uint[] ColorTable { get; } = new uint[16];
    public List<(string Name, string Value)> OtherValues { get; } = new();
    public int ValueCount { get; private set; }
    public int SubKeyCount { get; private set; }
    public string[] SubKeys { get; private set; } = Array.Empty<string>();

    /// <summary>True when <c>HKCU\Console</c> exists.</summary>
    public static bool KeyExists()
    {
        using RegistryKey? key = Registry.CurrentUser.OpenSubKey(ConsoleKeyPath, writable: false);
        return key is not null;
    }

    /// <summary>Reads the key. A missing key yields an all-zero report rather than an error.</summary>
    public static RegistryReport Capture()
    {
        var report = new RegistryReport();

        using RegistryKey? key = Registry.CurrentUser.OpenSubKey(ConsoleKeyPath, writable: false);
        if (key is null)
        {
            return report;
        }

        report.ValueCount = key.ValueCount;
        report.SubKeys = key.GetSubKeyNames();
        report.SubKeyCount = report.SubKeys.Length;

        for (int i = 0; i < report.ColorTable.Length; i++)
        {
            object? raw = key.GetValue($"ColorTable{i:D2}");
            report.ColorTable[i] = raw switch
            {
                int value => unchecked((uint)value),
                long value => unchecked((uint)value),
                _ => 0u,
            };
        }

        foreach (string name in AppearanceValueNames)
        {
            object? raw = key.GetValue(name);
            report.OtherValues.Add((name, FormatValue(raw)));
        }

        return report;
    }

    /// <summary>
    /// Formats a registry value for display. Binary values are summarised by length rather than
    /// dumped, so a large value cannot flood the console.
    /// </summary>
    private static string FormatValue(object? raw) => raw switch
    {
        null => "(absent)",
        int value => value.ToString(CultureInfo.InvariantCulture),
        long value => value.ToString(CultureInfo.InvariantCulture),
        string text => text.Length == 0 ? "(empty)" : text,
        string[] parts => string.Join(", ", parts),
        byte[] bytes => $"(binary, {bytes.Length} bytes)",
        _ => raw.ToString() ?? "(unknown)",
    };

    /// <summary>
    /// A stable, indented JSON document.
    ///
    /// The text is assembled here rather than through a serialiser so the output ordering is fixed
    /// and the file is diffable between runs, which is the point of a backup report.
    /// </summary>
    public string ToJson()
    {
        var builder = new System.Text.StringBuilder();
        builder.AppendLine("{");
        builder.AppendLine($"  \"key\": \"HKEY_CURRENT_USER\\\\{ConsoleKeyPath}\",");
        builder.AppendLine($"  \"valueCount\": {ValueCount},");
        builder.AppendLine($"  \"subKeyCount\": {SubKeyCount},");

        builder.AppendLine("  \"subKeys\": [");
        for (int i = 0; i < SubKeys.Length; i++)
        {
            builder.Append("    \"").Append(EscapeJson(SubKeys[i])).Append('"');
            builder.AppendLine(i + 1 < SubKeys.Length ? "," : string.Empty);
        }
        builder.AppendLine("  ],");

        builder.AppendLine("  \"colorTable\": [");
        for (int i = 0; i < ColorTable.Length; i++)
        {
            uint stored = ColorTable[i];
            uint canonical = ((stored & 0xFF) << 16) | (stored & 0xFF00) | ((stored >> 16) & 0xFF);
            builder.Append($"    {{ \"name\": \"ColorTable{i:D2}\", \"stored\": {stored}, ");
            builder.Append($"\"canonical\": {canonical} }}");
            builder.AppendLine(i + 1 < ColorTable.Length ? "," : string.Empty);
        }
        builder.AppendLine("  ],");

        builder.AppendLine("  \"values\": {");
        for (int i = 0; i < OtherValues.Count; i++)
        {
            (string name, string value) = OtherValues[i];
            builder.Append($"    \"{EscapeJson(name)}\": \"{EscapeJson(value)}\"");
            builder.AppendLine(i + 1 < OtherValues.Count ? "," : string.Empty);
        }
        builder.AppendLine("  }");
        builder.AppendLine("}");
        return builder.ToString();
    }

    /// <summary>
    /// Escapes a string for JSON. Only the characters JSON requires are escaped, and every control
    /// character is emitted as a \u sequence, so a crafted registry value cannot break the document
    /// or inject a terminal escape sequence when the file is printed.
    /// </summary>
    private static string EscapeJson(string text)
    {
        var builder = new System.Text.StringBuilder(text.Length + 8);
        foreach (char ch in text)
        {
            switch (ch)
            {
                case '"': builder.Append("\\\""); break;
                case '\\': builder.Append("\\\\"); break;
                case '\b': builder.Append("\\b"); break;
                case '\f': builder.Append("\\f"); break;
                case '\n': builder.Append("\\n"); break;
                case '\r': builder.Append("\\r"); break;
                case '\t': builder.Append("\\t"); break;
                default:
                    if (char.IsControl(ch))
                    {
                        builder.Append("\\u").Append(((int)ch).ToString("X4", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        builder.Append(ch);
                    }
                    break;
            }
        }
        return builder.ToString();
    }
}
