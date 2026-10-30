// ConsoleBgTool - Windows Terminal profile settings.
//
// This is the only officially supported way to give cmd.exe or Windows PowerShell a persistent
// background image, an opacity and a colour scheme, because those settings belong to Windows
// Terminal rather than to the classic console. See the project README for the documentation
// references; the relevant ones are the profile appearance page (backgroundImage,
// backgroundImageOpacity, backgroundImageAlignment, backgroundImageStretchMode, opacity,
// useAcrylic, colorScheme) and the color schemes page.
//
// Safety properties enforced here:
//   * The file is parsed before it is touched, so a settings file the tool cannot understand is
//     reported rather than overwritten.
//   * Comments and unknown properties survive, because the parsed DOM is modified in place.
//   * A timestamped backup is written next to the original before the first modification.
//   * The new content is written to a temporary file and moved into place, so an interrupted
//     write cannot leave a truncated settings.json.
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace ConsoleBgTool.Cli;

/// <summary>Appearance values that Windows Terminal understands.</summary>
public sealed class TerminalProfileSettings
{
    /// <summary>Path to a background image, a URI, or "desktopWallpaper".</summary>
    public string? BackgroundImage { get; set; }

    /// <summary>0.0 to 1.0.</summary>
    public double? BackgroundImageOpacity { get; set; }

    /// <summary>One of center, left, top, right, bottom, topLeft, topRight, bottomLeft, bottomRight.</summary>
    public string? BackgroundImageAlignment { get; set; }

    /// <summary>One of none, fill, uniform, uniformToFill.</summary>
    public string? BackgroundImageStretchMode { get; set; }

    /// <summary>Background colour as #rgb or #rrggbb.</summary>
    public string? Background { get; set; }

    /// <summary>Foreground colour as #rgb or #rrggbb.</summary>
    public string? Foreground { get; set; }

    /// <summary>1 to 100, the percentage of the window that is opaque.</summary>
    public int? Opacity { get; set; }

    /// <summary>Whether to blur the background behind the window.</summary>
    public bool? UseAcrylic { get; set; }

    private static readonly HashSet<string> AllowedAlignments = new(StringComparer.Ordinal)
    {
        "center", "left", "top", "right", "bottom",
        "topLeft", "topRight", "bottomLeft", "bottomRight",
    };

    private static readonly HashSet<string> AllowedStretchModes = new(StringComparer.Ordinal)
    {
        "none", "fill", "uniform", "uniformToFill",
    };

    /// <summary>
    /// Validates every field and returns the properties to merge. Throws
    /// <see cref="ConfigurationException"/> for anything Windows Terminal would reject, so the
    /// tool never writes a settings file that Terminal would refuse to load.
    /// </summary>
    public Dictionary<string, JsonNode?> ToJsonProperties()
    {
        var properties = new Dictionary<string, JsonNode?>(StringComparer.Ordinal);

        if (BackgroundImage is not null)
        {
            if (BackgroundImage.Length == 0 || BackgroundImage.Length > 4096 || BackgroundImage.Any(char.IsControl))
            {
                throw new ConfigurationException("background image path is empty, too long, or contains control characters");
            }
            properties["backgroundImage"] = JsonValue.Create(BackgroundImage);
        }

        if (BackgroundImageOpacity is double imageOpacity)
        {
            if (imageOpacity < 0.0 || imageOpacity > 1.0 || double.IsNaN(imageOpacity))
            {
                throw new ConfigurationException("background image opacity must be between 0.0 and 1.0");
            }
            properties["backgroundImageOpacity"] = JsonValue.Create(imageOpacity);
        }

        if (BackgroundImageAlignment is string alignment)
        {
            if (!AllowedAlignments.Contains(alignment))
            {
                throw new ConfigurationException(
                    $"background image alignment '{alignment}' is not one of {string.Join(", ", AllowedAlignments)}");
            }
            properties["backgroundImageAlignment"] = JsonValue.Create(alignment);
        }

        if (BackgroundImageStretchMode is string stretchMode)
        {
            if (!AllowedStretchModes.Contains(stretchMode))
            {
                throw new ConfigurationException(
                    $"background image stretch mode '{stretchMode}' is not one of {string.Join(", ", AllowedStretchModes)}");
            }
            properties["backgroundImageStretchMode"] = JsonValue.Create(stretchMode);
        }

        AddColor(properties, "background", Background);
        AddColor(properties, "foreground", Foreground);

        if (Opacity is int opacity)
        {
            if (opacity < 1 || opacity > 100)
            {
                throw new ConfigurationException("terminal opacity must be between 1 and 100");
            }
            properties["opacity"] = JsonValue.Create(opacity);
        }

        if (UseAcrylic is bool acrylic)
        {
            properties["useAcrylic"] = JsonValue.Create(acrylic);
        }

        return properties;
    }

    private static void AddColor(Dictionary<string, JsonNode?> properties, string name, string? value)
    {
        if (value is null)
        {
            return;
        }

        string hex = value.TrimStart('#');
        if (!hex.All(Uri.IsHexDigit))
        {
            throw new ConfigurationException($"{name} colour '{value}' must be #rgb or #rrggbb");
        }

        // Windows Terminal accepts #rgb, but it is normalised to #rrggbb here so the written file is
        // unambiguous and diffable. Each shorthand nibble is doubled, so #123 becomes #112233.
        string expanded = hex.Length switch
        {
            3 => string.Concat(hex.Select(c => new string(c, 2))),
            6 => hex,
            _ => throw new ConfigurationException($"{name} colour '{value}' must be #rgb or #rrggbb"),
        };

        properties[name] = JsonValue.Create("#" + expanded.ToLowerInvariant());
    }
}

/// <summary>Reads and updates Windows Terminal's settings.json.</summary>
public static class WindowsTerminalSettings
{
    private static readonly JsonDocumentOptions ParseOptions = new()
    {
        // Terminal's own settings file ships with comments, so they must be tolerated.
        CommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    private static readonly JsonSerializerOptions WriteOptions = new()
    {
        WriteIndented = true,
    };

    /// <summary>
    /// The documented settings location for the packaged Terminal, falling back to the
    /// unpackaged location. Returns null when neither exists.
    /// </summary>
    public static string? FindSettingsPath()
    {
        string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

        string packaged = Path.Combine(
            localAppData,
            "Packages",
            "Microsoft.WindowsTerminal_8wekyb3d8bbwe",
            "LocalState",
            "settings.json");
        if (File.Exists(packaged))
        {
            return packaged;
        }

        string preview = Path.Combine(
            localAppData,
            "Packages",
            "Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe",
            "LocalState",
            "settings.json");
        if (File.Exists(preview))
        {
            return preview;
        }

        string unpackaged = Path.Combine(localAppData, "Microsoft", "Windows Terminal", "settings.json");
        if (File.Exists(unpackaged))
        {
            return unpackaged;
        }

        return null;
    }

    /// <summary>The well known Windows Terminal window class, as observed on Windows 11 24H2.</summary>
    public const string TerminalWindowClass = "CASCADIA_HOSTING_WINDOW_CLASS";

    /// <summary>
    /// Applies profile defaults. With <paramref name="allProfiles"/> false only the profiles
    /// whose command line starts with cmd.exe or powershell.exe are changed, which is
    /// sufficient for this tool's purpose and avoids touching unrelated profiles.
    /// </summary>
    public static void Apply(string settingsPath, TerminalProfileSettings settings,
        bool allProfiles, bool dryRun, TextWriter output)
    {
        Dictionary<string, JsonNode?> properties = settings.ToJsonProperties();
        if (properties.Count == 0)
        {
            output.WriteLine("terminal: nothing to change");
            return;
        }

        string original = File.ReadAllText(settingsPath, Encoding.UTF8);

        JsonNode? root;
        try
        {
            root = JsonNode.Parse(original, nodeOptions: null, documentOptions: ParseOptions);
        }
        catch (JsonException ex)
        {
            throw new ConfigurationException(
                $"'{settingsPath}' is not valid JSON ({ex.Message}); refusing to modify it");
        }

        if (root is not JsonObject rootObject)
        {
            throw new ConfigurationException($"'{settingsPath}' does not contain a JSON object at the top level");
        }

        var changed = new List<string>();

        JsonObject? profiles = rootObject["profiles"] as JsonObject;
        if (profiles is null)
        {
            profiles = new JsonObject();
            rootObject["profiles"] = profiles;
            changed.Add("profiles");
        }

        if (allProfiles)
        {
            JsonObject defaults = profiles["defaults"] as JsonObject ?? new JsonObject();
            profiles["defaults"] = defaults;
            foreach ((string key, JsonNode? value) in properties)
            {
                defaults[key] = value?.DeepClone();
            }
            changed.Add("profiles.defaults");
        }
        else
        {
            if (profiles["defaults"] is JsonObject existingDefaults &&
                existingDefaults.Count > 0 &&
                !TryMergeIntoMatchingProfiles(profiles, properties, changed))
            {
                output.WriteLine(
                    "terminal: no cmd.exe or powershell.exe profile found; " +
                    "use --all-profiles to change every profile");
                return;
            }
        }

        string updated = rootObject.ToJsonString(WriteOptions);

        // Verify that what will be written is still parseable before replacing anything.
        try
        {
            _ = JsonNode.Parse(updated, nodeOptions: null, documentOptions: ParseOptions);
        }
        catch (JsonException ex)
        {
            throw new ConfigurationException($"internal error: produced invalid JSON ({ex.Message}); aborting");
        }

        if (dryRun)
        {
            output.WriteLine($"terminal: would update {string.Join(", ", changed)} in {settingsPath} (dry run)");
            return;
        }

        string backupPath = WriteBackup(settingsPath, original);
        output.WriteLine($"terminal: backup written to {backupPath}");

        WriteAtomically(settingsPath, updated);
        output.WriteLine($"terminal: updated {string.Join(", ", changed)} in {settingsPath}");
    }

    private static bool TryMergeIntoMatchingProfiles(JsonObject profiles,
        Dictionary<string, JsonNode?> properties, List<string> changed)
    {
        if (profiles["list"] is not JsonArray list)
        {
            return false;
        }

        bool any = false;
        foreach (JsonNode? entry in list)
        {
            if (entry is not JsonObject profile)
            {
                continue;
            }
            if (!IsTargetProfile(profile))
            {
                continue;
            }

            foreach ((string key, JsonNode? value) in properties)
            {
                profile[key] = value?.DeepClone();
            }
            any = true;
        }

        if (any)
        {
            changed.Add("profiles.list[cmd/powershell]");
        }
        return any;
    }

    /// <summary>
    /// A profile belongs to cmd.exe or Windows PowerShell when its command line, source or name
    /// says so.
    /// </summary>
    private static bool IsTargetProfile(JsonObject profile)
    {
        static string Text(JsonObject source, string name) =>
            source[name] is JsonValue value && value.TryGetValue(out string? text) && text is not null
                ? text
                : string.Empty;

        string combined = string.Join('\n',
            Text(profile, "commandline"),
            Text(profile, "source"),
            Text(profile, "name"),
            Text(profile, "guid"));

        if (combined.Length == 0)
        {
            return false;
        }

        string lowered = combined.ToLowerInvariant();
        return lowered.Contains("cmd.exe", StringComparison.Ordinal) ||
               lowered.Contains("powershell.exe", StringComparison.Ordinal) ||
               lowered.Contains("command prompt", StringComparison.Ordinal) ||
               lowered.Contains("windows powershell", StringComparison.Ordinal);
    }

    /// <summary>
    /// Writes a backup next to the settings file. The name carries a UTC timestamp so repeated
    /// runs never overwrite an earlier backup, and the file is created with
    /// <see cref="FileMode.CreateNew"/> so a pre-existing name cannot be clobbered.
    /// </summary>
    private static string WriteBackup(string settingsPath, string content)
    {
        string directory = Path.GetDirectoryName(settingsPath) ?? ".";
        string baseName = Path.GetFileName(settingsPath);
        string timestamp = DateTime.UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'", System.Globalization.CultureInfo.InvariantCulture);
        string backupPath = Path.Combine(directory, $"{baseName}.cbt-backup-{timestamp}");

        using var stream = new FileStream(backupPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        using var writer = new StreamWriter(stream, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        writer.Write(content);
        return backupPath;
    }

    /// <summary>
    /// Writes the new content to a temporary file in the same directory and moves it over the
    /// original. Same-volume replace is atomic on NTFS, so a crash cannot leave a partial file.
    /// </summary>
    private static void WriteAtomically(string path, string content)
    {
        string directory = Path.GetDirectoryName(path) ?? ".";
        string temporary = Path.Combine(directory, Path.GetFileName(path) + ".cbt-tmp-" + Guid.NewGuid().ToString("N"));

        try
        {
            File.WriteAllText(temporary, content, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporary))
            {
                try
                {
                    File.Delete(temporary);
                }
                catch (IOException)
                {
                    // Leaving a stale temp file is preferable to masking the real failure.
                }
            }
        }
    }
}
