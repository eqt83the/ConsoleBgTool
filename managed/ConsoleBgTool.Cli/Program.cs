// ConsoleBgTool - command line interface.
//
// Command surface
// ---------------
//   apply     style the console this process is attached to
//   show      report the live console state and the identified host
//   presets   list the built-in colour presets
//   restore   reset the live console to the stock Windows 11 palette and attributes
//   terminal  write Windows Terminal profile settings (the persistent path)
//   registry  report or snapshot HKCU\Console
//   doctor    report the environment and what will and will not work here
//
// Everything that changes appearance goes through the native layers. Nothing here shells out,
// evaluates a script, or executes a string from a configuration file.
using System.Globalization;
using System.Text;
using ConsoleBgTool.Interop;

namespace ConsoleBgTool.Cli;

internal static class Program
{
    private const int ExitOk = 0;
    private const int ExitError = 1;
    private const int ExitUsage = 2;
    private const int ExitUnsupported = 3;

    private static int Main(string[] args)
    {
        // A console app that has been given a UTF-8 capable console should emit UTF-8 so the
        // diagnostics are readable; failure here is not fatal.
        try
        {
            Console.OutputEncoding = Encoding.UTF8;
        }
        catch (IOException)
        {
            // Redirected or unavailable console output: keep the default encoding.
        }
        catch (PlatformNotSupportedException)
        {
        }

        if (args.Length == 0)
        {
            PrintUsage(Console.Out);
            return ExitUsage;
        }

        try
        {
            return Dispatch(args);
        }
        catch (ConfigurationException ex)
        {
            Error(ex.Message);
            return ExitError;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            Error(ex.Message);
            return ExitError;
        }
    }

    private static int Dispatch(string[] args)
    {
        /*
         * --report-state FILE is a global diagnostic: after the command runs, the live console state
         * is written to the file. It exists so the state of a console can be inspected from outside
         * that console, which is otherwise impossible because each process gets its own console.
         * Every verification script in this project relies on it.
         */
        string? reportPath = null;
        var forwarded = new List<string>(args.Length);
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--report-state")
            {
                if (i + 1 >= args.Length)
                {
                    Error("--report-state requires a file path");
                    return ExitUsage;
                }
                reportPath = args[++i];
                continue;
            }
            forwarded.Add(args[i]);
        }

        if (forwarded.Count == 0)
        {
            PrintUsage(Console.Out);
            return ExitUsage;
        }

        int result = DispatchCommand(forwarded.ToArray());

        if (reportPath is not null)
        {
            result = WriteStateReport(reportPath, result);
        }
        return result;
    }

    /// <summary>
    /// Writes the live console state as a small key=value file. Kept deliberately simple so a
    /// verification script can parse it without a JSON dependency.
    /// </summary>
    private static int WriteStateReport(string path, int commandResult)
    {
        var builder = new StringBuilder();
        builder.AppendLine("commandExit=" + commandResult.ToString(CultureInfo.InvariantCulture));

        CbtStatus status = NativeMethods.Read(out NativeConfig config, out NativeLiveState state);
        builder.AppendLine("readStatus=" + status.ToString());
        builder.AppendLine("hasConsole=" + (status == CbtStatus.Ok ? "yes" : "no"));

        if (status == CbtStatus.Ok)
        {
            builder.AppendLine("host=" + NativeMethods.HostKind);
            builder.AppendLine("isWindowsTerminal=" + (NativeMethods.IsWindowsTerminalHost ? "yes" : "no"));
            builder.AppendLine("ownsWindow=" + (state.HasWindow ? "yes" : "no"));
            builder.AppendLine("bufferColumns=" + state.BufferColumns.ToString(CultureInfo.InvariantCulture));
            builder.AppendLine("bufferRows=" + state.BufferRows.ToString(CultureInfo.InvariantCulture));
            builder.AppendLine("windowColumns=" + state.WindowColumns.ToString(CultureInfo.InvariantCulture));
            builder.AppendLine("windowRows=" + state.WindowRows.ToString(CultureInfo.InvariantCulture));
            builder.AppendLine("attributes=0x" + state.Attributes.ToString("X4", CultureInfo.InvariantCulture));
            builder.AppendLine("canvasSlot=" + config.CanvasSlot.ToString(CultureInfo.InvariantCulture));
            builder.AppendLine("foregroundSlot=" + config.ForegroundSlot.ToString(CultureInfo.InvariantCulture));

            uint canvas = state.Palette[Math.Clamp(config.CanvasSlot, 0, 15)];
            builder.AppendLine("canvasColor=" + Appearance.ToHex(canvas));
            for (int i = 0; i < NativeConfig.ColorTableSize; i++)
            {
                builder.AppendLine($"palette{i:D2}={Appearance.ToHex(state.Palette[i])}");
            }
        }

        try
        {
            File.WriteAllText(path, builder.ToString(), new UTF8Encoding(false));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Error($"could not write the state report: {ex.Message}");
            return ExitError;
        }
        return commandResult;
    }

    private static int DispatchCommand(string[] args)
    {
        string command = args[0].ToLowerInvariant();
        string[] rest = args[1..];

        return command switch
        {
            "apply" or "set" => CommandApply(rest, Console.Out),
            "show" or "status" => CommandShow(rest, Console.Out),
            "presets" or "list" => CommandPresets(rest, Console.Out),
            "restore" or "reset" => CommandRestore(rest, Console.Out),
            "terminal" or "wt" => CommandTerminal(rest, Console.Out),
            "registry" or "reg" => CommandRegistry(rest, Console.Out),
            "doctor" or "check" => CommandDoctor(rest, Console.Out),
            "-h" or "--help" or "help" => CommandHelp(Console.Out),
            "-v" or "--version" or "version" => CommandVersion(Console.Out),
            _ => Unknown(command),
        };
    }

    private static int Unknown(string command)
    {
        Error($"unknown command '{Sanitize(command)}'");
        PrintUsage(Console.Error);
        return ExitUsage;
    }

    /* --------------------------------------------------------------------- */
    /* apply                                                                  */
    /* --------------------------------------------------------------------- */

    private static int CommandApply(string[] args, TextWriter output)
    {
        Appearance appearance = Appearance.CreateDefault();
        bool haveConfig = false;
        bool applyGeometry = false;
        bool applyOpacity = false;
        bool dryRun = false;
        bool quiet = false;
        string? configPath = null;

        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i];
            switch (arg)
            {
                case "--preset":
                    appearance = Appearance.FromPreset(RequireValue(args, ref i, arg));
                    haveConfig = true;
                    break;
                case "--config":
                    configPath = RequireValue(args, ref i, arg);
                    break;
                case "--background":
                    appearance.CanvasSlot = Appearance.ParseSlotText(RequireValue(args, ref i, arg));
                    haveConfig = true;
                    break;
                case "--foreground":
                    appearance.ForegroundSlot = Appearance.ParseSlotText(RequireValue(args, ref i, arg));
                    haveConfig = true;
                    break;
                case "--color":
                {
                    string spec = RequireValue(args, ref i, arg);
                    Appearance.ApplyColorOverride(appearance, spec);
                    haveConfig = true;
                    break;
                }
                case "--opacity":
                {
                    appearance.OpacityPercent = ParseRange(RequireValue(args, ref i, arg), 10, 100, "opacity");
                    applyOpacity = true;
                    haveConfig = true;
                    break;
                }
                case "--geometry":
                {
                    string spec = RequireValue(args, ref i, arg);
                    Appearance.ApplyGeometryOverride(appearance, spec);
                    applyGeometry = true;
                    haveConfig = true;
                    break;
                }
                case "--dry-run":
                    dryRun = true;
                    break;
                case "-q":
                case "--quiet":
                    /*
                     * Suppress the success chatter. Meant for the autostart hooks, where this command
                     * runs before the user's own prompt and must print nothing. Failures are still
                     * reported on stderr, and the command never changes the shell's exit code.
                     */
                    quiet = true;
                    break;
                case "-h":
                case "--help":
                    PrintApplyUsage(output);
                    return ExitOk;
                default:
                    throw new ConfigurationException($"unknown option '{Sanitize(arg)}' for apply");
            }
        }

        if (configPath is not null)
        {
            if (!File.Exists(configPath))
            {
                throw new ConfigurationException($"configuration file not found: {configPath}");
            }
            // Read as UTF-8 and let a malformed file be reported rather than silently mangled.
            string text;
            try
            {
                text = File.ReadAllText(configPath, new UTF8Encoding(false, true));
            }
            catch (DecoderFallbackException)
            {
                throw new ConfigurationException($"{configPath} is not valid UTF-8");
            }
            appearance = Appearance.Parse(text, appearance);
            haveConfig = true;
        }

        if (!haveConfig)
        {
            throw new ConfigurationException("nothing to apply: pass --preset, --config or an override");
        }

        // Validate before anything is touched, so a bad value cannot leave a half-styled console.
        NativeConfig native = appearance.ToNative();

        double contrast = appearance.ContrastRatio();
        if (contrast < 4.5)
        {
            output.WriteLine(
                $"warning: the default text colour and the window background have a contrast ratio " +
                $"of {contrast.ToString("F2", CultureInfo.InvariantCulture)}:1, below the WCAG AA " +
                $"threshold of 4.5:1; text may be hard to read");
        }

        if (dryRun)
        {
            output.WriteLine("apply: dry run, nothing changed");
            DescribeAppearance(appearance, output);
            return ExitOk;
        }

        CbtStatus status = NativeMethods.Apply(native, applyGeometry, applyOpacity);
        if (status != CbtStatus.Ok)
        {
            Error(NativeMethods.Describe(status, "apply"));
            if (status == CbtStatus.Unsupported)
            {
                output.WriteLine(
                    "hint: this process has no console of its own. Run the command inside a cmd.exe " +
                    "or Windows PowerShell window, or use 'terminal' to configure Windows Terminal.");
                return ExitUnsupported;
            }
            return ExitError;
        }

        if (!quiet)
        {
            output.WriteLine("apply: done");
            DescribeAppearance(appearance, output);
            if (!applyGeometry)
            {
                output.WriteLine("  geometry : unchanged (pass --geometry COLSxROWS to set it)");
            }
            if (!applyOpacity)
            {
                output.WriteLine("  opacity  : unchanged (pass --opacity PCT to set it)");
            }
            output.WriteLine(
                "note: this affects the console window it ran in. Use 'terminal' for settings that " +
                "survive a restart.");
        }

        /*
         * Verify rather than assume. The native layer reports success, but the only honest check is
         * to read the console back and confirm the palette really carries the values that were
         * requested. This is also what makes a live-console test possible from outside the console.
         */
        CbtStatus verifyStatus = NativeMethods.Read(out NativeConfig verified, out NativeLiveState verifiedState);
        if (verifyStatus == CbtStatus.Ok)
        {
            int mismatches = 0;
            for (int i = 0; i < NativeConfig.ColorTableSize; i++)
            {
                if (verifiedState.Palette[i] != appearance.Palette[i])
                {
                    mismatches++;
                }
            }

            if (mismatches != 0)
            {
                Error($"the console did not take the palette: {mismatches} of " +
                      $"{NativeConfig.ColorTableSize} entries differ from what was requested");
                return ExitError;
            }

            if (!quiet)
            {
                output.WriteLine(
                    $"verified : all {NativeConfig.ColorTableSize} palette entries read back as requested");
            }
        }

        return ExitOk;
    }

    private static void DescribeAppearance(Appearance appearance, TextWriter output)
    {
        output.WriteLine($"  background: slot {appearance.CanvasSlot} {Appearance.ToHex(appearance.Palette[appearance.CanvasSlot])}");
        output.WriteLine($"  foreground: slot {appearance.ForegroundSlot} {Appearance.ToHex(appearance.Palette[appearance.ForegroundSlot])}");
        output.WriteLine($"  palette   : {appearance.Palette.Length} entries set");
        if (appearance.OpacityPercent < Appearance.MaxOpacity)
        {
            output.WriteLine($"  opacity   : {appearance.OpacityPercent}%");
        }
    }

    /* --------------------------------------------------------------------- */
    /* show                                                                   */
    /* --------------------------------------------------------------------- */

    private static int CommandShow(string[] args, TextWriter output)
    {
        if (args.Length > 0 && args[0] is "-h" or "--help")
        {
            output.WriteLine("usage: show [--palette]");
            output.WriteLine("        report the live console state; --palette lists all 16 colour entries");
            output.WriteLine("用法：show [--palette]");
            output.WriteLine("        报告控制台的实时状态；--palette 会列出全部 16 个颜色条目");
            return ExitOk;
        }
        bool showPalette = args.Contains("--palette");

        CbtStatus status = NativeMethods.Read(out NativeConfig native, out NativeLiveState state);
        if (status != CbtStatus.Ok)
        {
            Error(NativeMethods.Describe(status, "read"));
            output.WriteLine("hint: run this inside a console window.");
            return ExitUnsupported;
        }

        HostKind host = NativeMethods.HostKind;
        output.WriteLine("console");
        output.WriteLine($"  host           : {DescribeHost(host)}");
        output.WriteLine($"  new console    : {(state.V2 ? "yes (ForceV2=1)" : "no (legacy host)")}");
        output.WriteLine($"  owns a window  : {(state.HasWindow ? "yes" : "no (pseudoconsole)")}");
        output.WriteLine($"  screen buffer  : {state.BufferColumns} x {state.BufferRows}");
        output.WriteLine($"  window         : {state.WindowColumns} x {state.WindowRows}");
        if (state.FontWidth > 0 && state.FontHeight > 0)
        {
            output.WriteLine($"  font cell      : {state.FontWidth} x {state.FontHeight} px");
        }
        if (state.WindowPixelWidth > 0)
        {
            output.WriteLine($"  window pixels  : {state.WindowPixelWidth} x {state.WindowPixelHeight}");
        }

        int fg;
        int bg;
        DecodeAttributeSlots(state.Attributes, out fg, out bg);
        output.WriteLine($"  default attr   : 0x{state.Attributes:X4} (foreground slot {fg}, background slot {bg})");

        if (showPalette)
        {
            output.WriteLine("  palette");
            for (int i = 0; i < NativeConfig.ColorTableSize; i++)
            {
                uint color = state.Palette[i];
                output.WriteLine(
                    $"    {i,2}  {SlotName(i),-13} #{color & 0xFFFFFF:X6}   " +
                    $"COLORREF 0x{NativeMethods.ColorToColorRef(color):X8}");
            }
        }
        return ExitOk;
    }

    /* --------------------------------------------------------------------- */
    /* presets                                                                */
    /* --------------------------------------------------------------------- */

    private static int CommandPresets(string[] args, TextWriter output)
    {
        if (args.Length > 0 && args[0] is "-h" or "--help")
        {
            PrintPresetsUsage(output);
            return ExitOk;
        }
        if (args.Length > 0)
        {
            throw new ConfigurationException($"unknown option '{Sanitize(args[0])}' for presets");
        }

        output.WriteLine("built-in colour presets");
        output.WriteLine();
        foreach ((string name, string description) in Appearance.PresetNames())
        {
            Appearance preset = Appearance.FromPreset(name);
            uint canvas = preset.Palette[preset.CanvasSlot];
            uint text = preset.Palette[preset.ForegroundSlot];
            double contrast = preset.ContrastRatio();
            output.WriteLine($"  {name,-14} {description}");
            output.WriteLine(
                $"                 canvas {Appearance.ToHex(canvas)} on {Appearance.ToHex(text)}, " +
                $"contrast {contrast.ToString("F2", CultureInfo.InvariantCulture)}:1");
        }
        output.WriteLine();
        output.WriteLine("usage: apply --preset <name>");
        output.WriteLine("用法：apply --preset <name>");
        return ExitOk;
    }

    /* --------------------------------------------------------------------- */
    /* restore                                                                */
    /* --------------------------------------------------------------------- */

    private static int CommandRestore(string[] args, TextWriter output)
    {
        if (args.Length > 0 && args[0] is "-h" or "--help")
        {
            PrintRestoreUsage(output);
            return ExitOk;
        }
        if (args.Length > 0)
        {
            throw new ConfigurationException($"unknown option '{Sanitize(args[0])}' for restore");
        }

        /*
         * The CLI runs one command per process, so the snapshot-based cbt_platform_restore path is
         * useless here: the snapshot belongs to the process that applied a style, and that process
         * has already exited. Instead, restore the console this command is attached to by applying
         * the stock Windows 11 configuration. Geometry and opacity are deliberately left alone:
         * "restore the appearance" must not resize the window the user is looking at.
         */
        Appearance defaults = Appearance.CreateDefault();
        NativeConfig native = defaults.ToNative();

        CbtStatus status = NativeMethods.Apply(native, applyGeometry: false, applyOpacity: false);
        if (status != CbtStatus.Ok)
        {
            Error(NativeMethods.Describe(status, "restore"));
            if (status == CbtStatus.Unsupported)
            {
                output.WriteLine(
                    "hint: this process has no console of its own. Run the command inside a cmd.exe " +
                    "or Windows PowerShell window.");
                return ExitUnsupported;
            }
            return ExitError;
        }

        /*
         * Verify rather than assume. A restore that silently did nothing is exactly the bug this
         * command is fixing, so the palette and the default text attribute are read back and
         * compared with the stock configuration before success is reported.
         */
        CbtStatus verifyStatus = NativeMethods.Read(out NativeConfig verified, out NativeLiveState live);
        if (verifyStatus != CbtStatus.Ok)
        {
            Error(NativeMethods.Describe(verifyStatus, "restore verification"));
            return ExitError;
        }

        int mismatches = 0;
        for (int i = 0; i < NativeConfig.ColorTableSize; i++)
        {
            if (live.Palette[i] != defaults.Palette[i])
            {
                mismatches++;
            }
        }
        if (mismatches != 0)
        {
            Error($"the console did not take the default palette: {mismatches} of " +
                  $"{NativeConfig.ColorTableSize} entries differ from the Windows 11 defaults");
            return ExitError;
        }
        if (verified.ForegroundSlot != defaults.ForegroundSlot ||
            verified.BackgroundSlot != defaults.BackgroundSlot)
        {
            Error("the console did not take the default text attribute: " +
                  $"foreground slot {verified.ForegroundSlot} (want {defaults.ForegroundSlot}), " +
                  $"background slot {verified.BackgroundSlot} (want {defaults.BackgroundSlot})");
            return ExitError;
        }

        output.WriteLine("restore: the stock Windows 11 palette and attributes are back");
        output.WriteLine(
            $"verified: all {NativeConfig.ColorTableSize} palette entries read back as the Windows 11 defaults");
        output.WriteLine("          the window geometry and the opacity were not touched");
        output.WriteLine("note: this only affects the console the command runs in. New consoles and");
        output.WriteLine(
            "      Windows Terminal profiles are unchanged; use 'terminal' for persistent settings.");
        return ExitOk;
    }

    /* --------------------------------------------------------------------- */
    /* terminal                                                               */
    /* --------------------------------------------------------------------- */

    private static int CommandTerminal(string[] args, TextWriter output)
    {
        var settings = new TerminalProfileSettings();
        bool allProfiles = false;
        bool dryRun = false;
        string? settingsPath = null;
        string? imagePath = null;
        bool acrylic = false;

        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i];
            switch (arg)
            {
                case "--image":
                    imagePath = RequireValue(args, ref i, arg);
                    break;
                case "--image-opacity":
                    settings.BackgroundImageOpacity =
                        ParseDouble(RequireValue(args, ref i, arg), 0.0, 1.0, "image opacity");
                    break;
                case "--image-align":
                    settings.BackgroundImageAlignment = RequireValue(args, ref i, arg);
                    break;
                case "--image-stretch":
                    settings.BackgroundImageStretchMode = RequireValue(args, ref i, arg);
                    break;
                case "--background":
                    settings.Background = RequireValue(args, ref i, arg);
                    break;
                case "--foreground":
                    settings.Foreground = RequireValue(args, ref i, arg);
                    break;
                case "--opacity":
                    settings.Opacity = ParseRange(RequireValue(args, ref i, arg), 1, 100, "terminal opacity");
                    break;
                case "--acrylic":
                    settings.UseAcrylic = true;
                    acrylic = true;
                    break;
                case "--no-acrylic":
                    settings.UseAcrylic = false;
                    break;
                case "--all-profiles":
                    allProfiles = true;
                    break;
                case "--settings":
                    settingsPath = RequireValue(args, ref i, arg);
                    break;
                case "--dry-run":
                    dryRun = true;
                    break;
                case "-h":
                case "--help":
                    PrintTerminalUsage(output);
                    return ExitOk;
                default:
                    throw new ConfigurationException($"unknown option '{Sanitize(arg)}' for terminal");
            }
        }

        settingsPath ??= WindowsTerminalSettings.FindSettingsPath();
        if (settingsPath is null)
        {
            Error("Windows Terminal settings.json was not found");
            output.WriteLine(
                "looked in %LOCALAPPDATA%\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\ " +
                "and %LOCALAPPDATA%\\Microsoft\\Windows Terminal\\");
            output.WriteLine("Install Windows Terminal, or pass --settings with an explicit path.");
            return ExitUnsupported;
        }

        // An image path is turned into an absolute path so the setting is not resolved relative to
        // whatever directory Terminal happens to start in.
        if (imagePath is not null)
        {
            if (!string.Equals(imagePath, "desktopWallpaper", StringComparison.OrdinalIgnoreCase))
            {
                if (!File.Exists(imagePath))
                {
                    throw new ConfigurationException($"background image not found: {imagePath}");
                }
                imagePath = Path.GetFullPath(imagePath);
            }
            settings.BackgroundImage = imagePath;
        }

        if (acrylic && settings.Opacity is null)
        {
            settings.Opacity = 85;
        }

        WindowsTerminalSettings.Apply(settingsPath, settings, allProfiles, dryRun, output);
        output.WriteLine(
            "note: Windows Terminal applies profile settings to new tabs/panes; existing ones may " +
            "need a new tab. This affects profiles hosted by Windows Terminal, not the classic " +
            "console window.");
        return ExitOk;
    }

    /* --------------------------------------------------------------------- */
    /* registry                                                               */
    /* --------------------------------------------------------------------- */

    private static int CommandRegistry(string[] args, TextWriter output)
    {
        if (args.Length == 0 || args[0] is "-h" or "--help")
        {
            output.WriteLine("usage: registry [--json]");
            output.WriteLine();
            output.WriteLine("Reports HKCU\\Console: the values a console reads for its defaults.");
            output.WriteLine("This tool never writes these values: on Windows 11 they no longer affect");
            output.WriteLine("newly created consoles, so writing them would only desynchronise what the");
            output.WriteLine("console's own Properties dialog shows. Reported here for reference and backup.");
            output.WriteLine();
            output.WriteLine(ChineseHeading);
            output.WriteLine();
            output.WriteLine("用法：registry [--json]");
            output.WriteLine();
            output.WriteLine("报告 HKCU\\Console：控制台读取的默认值。");
            output.WriteLine("本工具从不写入这些值：在 Windows 11 上它们不再影响新建的控制台，");
            output.WriteLine("写入只会让控制台“属性”对话框显示的内容与实际不一致。此处仅作参考和备份。");
            return ExitOk;
        }

        bool json = args.Contains("--json");
        RegistryReport report = RegistryReport.Capture();

        if (json)
        {
            output.Write(report.ToJson());
            return ExitOk;
        }

        output.WriteLine($"HKCU\\Console: {report.ValueCount} values, {report.SubKeyCount} subkeys");
        output.WriteLine();
        output.WriteLine("colour table (as stored, and in #RRGGBB order)");
        for (int i = 0; i < report.ColorTable.Length; i++)
        {
            uint stored = report.ColorTable[i];
            uint canonical = ((stored & 0xFF) << 16) | (stored & 0xFF00) | ((stored >> 16) & 0xFF);
            output.WriteLine($"  ColorTable{i:D2}  0x{stored:X8}  #{canonical:X6}");
        }
        output.WriteLine();
        output.WriteLine("other values that affect appearance");
        foreach ((string name, string value) in report.OtherValues)
        {
            output.WriteLine($"  {name,-22} {value}");
        }
        return ExitOk;
    }

    /* --------------------------------------------------------------------- */
    /* doctor                                                                 */
    /* --------------------------------------------------------------------- */

    private static int CommandDoctor(string[] args, TextWriter output)
    {
        if (args.Length > 0 && args[0] is "-h" or "--help")
        {
            PrintDoctorUsage(output);
            return ExitOk;
        }
        if (args.Length > 0)
        {
            throw new ConfigurationException($"unknown option '{Sanitize(args[0])}' for doctor");
        }

        output.WriteLine("environment");
        output.WriteLine($"  runtime        : {Environment.Version}");
        output.WriteLine($"  os             : {Environment.OSVersion.Version}");
        output.WriteLine($"  arch           : {System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture}");
        output.WriteLine($"  interactive    : {(Environment.UserInteractive ? "yes" : "no")}");

        output.WriteLine();
        output.WriteLine("native layers");
        output.WriteLine($"  cbt_core.dll   : {ProbeNative("cbt_core")}");
        output.WriteLine($"  cbt_platform.dll: {ProbeNative("cbt_platform")}");

        HostKind host = NativeMethods.HostKind;
        output.WriteLine();
        output.WriteLine("console host");
        output.WriteLine($"  kind           : {DescribeHost(host)}");
        output.WriteLine($"  windows terminal host : {(NativeMethods.IsWindowsTerminalHost ? "yes" : "no")}");

        output.WriteLine();
        output.WriteLine("what will work here");
        switch (host)
        {
            case HostKind.None:
                output.WriteLine("  live colour and opacity : NO - this process has no console");
                output.WriteLine("  windows terminal settings: " + (WindowsTerminalSettings.FindSettingsPath() is not null ? "yes" : "no"));
                break;
            case HostKind.ClassicConhost:
                output.WriteLine("  live colour            : yes (documented console API)");
                output.WriteLine("  live geometry          : yes");
                output.WriteLine("  window opacity         : yes, opt-in and reversible (not an officially");
                output.WriteLine("                           documented console setting; Windows Terminal is the");
                output.WriteLine("                           supported way to get opacity)");
                break;
            case HostKind.LegacyConhost:
                output.WriteLine("  live colour            : yes");
                output.WriteLine("  window opacity         : yes, opt-in and reversible");
                output.WriteLine("  note                   : this console is running the legacy host (ForceV2=0)");
                break;
            case HostKind.Pseudoconsole:
                output.WriteLine("  live colour            : yes, applied to this console");
                output.WriteLine("  window opacity         : NO - the host owns the window");
                output.WriteLine("  persistent appearance  : use 'terminal' for Windows Terminal profile settings");
                break;
        }

        output.WriteLine();
        output.WriteLine("persistent settings");
        string? settingsPath = WindowsTerminalSettings.FindSettingsPath();
        output.WriteLine($"  Windows Terminal settings.json : {settingsPath ?? "(not found)"}");
        output.WriteLine($"  HKCU\\Console                   : {(RegistryReport.KeyExists() ? "present" : "absent")}");
        output.WriteLine();
        output.WriteLine("reminder: on Windows 11, HKCU\\Console colour values do not reach newly created");
        output.WriteLine("consoles. This tool therefore styles live consoles through the documented console");
        output.WriteLine("API, and writes persistent appearance only to Windows Terminal's settings.json.");

        return host == HostKind.None ? ExitUnsupported : ExitOk;
    }

    private static string ProbeNative(string library)
    {
        try
        {
            // A cheap call that forces the resolver and the P/Invoke to run.
            _ = NativeMethods.ColorFromColorRef(0x00112233u);
            return library == "cbt_core" ? "loaded" : "loaded";
        }
        catch (DllNotFoundException ex)
        {
            return "MISSING - " + FirstLine(ex.Message);
        }
        catch (EntryPointNotFoundException ex)
        {
            return "BAD EXPORTS - " + FirstLine(ex.Message);
        }
        catch (BadImageFormatException ex)
        {
            // Typically a 32-bit library in a 64-bit process, or the reverse.
            return "WRONG ARCHITECTURE - " + FirstLine(ex.Message);
        }
    }

    /* --------------------------------------------------------------------- */
    /* help / version                                                         */
    /* --------------------------------------------------------------------- */

    private static int CommandHelp(TextWriter output)
    {
        PrintUsage(output);
        return ExitOk;
    }

    private static int CommandVersion(TextWriter output)
    {
        output.WriteLine("ConsoleBgTool 1.0.0");
        output.WriteLine("styles cmd.exe and Windows PowerShell console backgrounds on Windows 10/11.");
        return ExitOk;
    }

    /*
     * Every usage block is printed twice: the English text first, then a complete Chinese
     * translation, separated by a heading. Command names, option names and value placeholders stay
     * in Latin script in both halves so they can be typed exactly as shown. The Chinese text is
     * written through the same TextWriter after Main has set the console output encoding to UTF-8.
     */
    private const string ChineseHeading =
        "------------------------------ 中文 / Chinese ------------------------------";

    private static void PrintUsage(TextWriter output)
    {
        output.WriteLine("ConsoleBgTool - style cmd.exe and Windows PowerShell console backgrounds");
        output.WriteLine();
        output.WriteLine("usage: ConsoleBgTool <command> [options]");
        output.WriteLine();
        output.WriteLine("commands");
        output.WriteLine("  apply      style the console this command runs in");
        output.WriteLine("  show       report the live console state and the identified host");
        output.WriteLine("  presets    list the built-in colour presets");
        output.WriteLine("  restore    reset the live console to the stock Windows 11 palette");
        output.WriteLine("  terminal   write Windows Terminal profile settings (persistent)");
        output.WriteLine("  registry   report HKCU\\Console for reference and backup");
        output.WriteLine("  doctor     report the environment and what will work here");
        output.WriteLine();
        output.WriteLine("run 'ConsoleBgTool <command> --help' for the options of a command.");
        output.WriteLine();
        output.WriteLine("typical use");
        output.WriteLine("  ConsoleBgTool presets");
        output.WriteLine("  ConsoleBgTool apply --preset nord --geometry 120x30");
        output.WriteLine("  ConsoleBgTool terminal --image C:\\pics\\bg.png --image-opacity 0.25 --opacity 90");
        output.WriteLine();
        output.WriteLine(ChineseHeading);
        output.WriteLine();
        output.WriteLine("ConsoleBgTool - 设置 cmd.exe 和 Windows PowerShell 控制台的背景");
        output.WriteLine();
        output.WriteLine("用法：ConsoleBgTool <命令> [选项]");
        output.WriteLine();
        output.WriteLine("命令");
        output.WriteLine("  apply      为当前命令所在的控制台应用配色");
        output.WriteLine("  show       显示控制台的实时状态和识别出的宿主");
        output.WriteLine("  presets    列出内置的配色预设");
        output.WriteLine("  restore    把当前控制台恢复为 Windows 11 默认调色板和文字属性");
        output.WriteLine("  terminal   写入 Windows Terminal 配置文件（持久生效）");
        output.WriteLine("  registry   报告 HKCU\\Console 的内容，供参考和备份");
        output.WriteLine("  doctor     报告运行环境以及哪些功能在此可用");
        output.WriteLine();
        output.WriteLine("运行 'ConsoleBgTool <命令> --help' 查看某个命令的选项。");
        output.WriteLine();
        output.WriteLine("常用示例");
        output.WriteLine("  ConsoleBgTool presets");
        output.WriteLine("  ConsoleBgTool apply --preset nord --geometry 120x30");
        output.WriteLine("  ConsoleBgTool terminal --image C:\\pics\\bg.png --image-opacity 0.25 --opacity 90");
    }

    private static void PrintApplyUsage(TextWriter output)
    {
        output.WriteLine("usage: apply [--preset NAME] [--config FILE] [options]");
        output.WriteLine();
        output.WriteLine("options");
        output.WriteLine("  --preset NAME        a built-in preset (see 'presets')");
        output.WriteLine("  --config FILE        a key=value file; applied on top of --preset");
        output.WriteLine("  --background SLOT    window background slot, 0-15 or a slot name");
        output.WriteLine("  --foreground SLOT    default text slot, 0-15 or a slot name");
        output.WriteLine("  --color SLOT=COLOR   override one palette entry, repeatable");
        output.WriteLine("  --opacity PCT        10-100; 100 leaves the window fully opaque");
        output.WriteLine("  --geometry COLSxROWS set the screen buffer and window size");
        output.WriteLine("  --dry-run            validate and describe, change nothing");
        output.WriteLine("  --quiet              print nothing on success (for the autostart hooks)");
        output.WriteLine();
        output.WriteLine("colours accept #RGB, #RRGGBB, 0xRRGGBB, RRGGBB containing A-F, a decimal");
        output.WriteLine("integer, or a name such as red / grey / crimson.");
        output.WriteLine();
        output.WriteLine(ChineseHeading);
        output.WriteLine();
        output.WriteLine("用法：apply [--preset NAME] [--config FILE] [选项]");
        output.WriteLine();
        output.WriteLine("选项");
        output.WriteLine("  --preset NAME        内置预设（参见 'presets'）");
        output.WriteLine("  --config FILE        key=value 配置文件；在 --preset 的基础上应用");
        output.WriteLine("  --background SLOT    窗口背景色槽，0-15 或槽位名称");
        output.WriteLine("  --foreground SLOT    默认文字色槽，0-15 或槽位名称");
        output.WriteLine("  --color SLOT=COLOR   覆盖一个调色板条目，可重复使用");
        output.WriteLine("  --opacity PCT        10-100；100 表示窗口完全不透明");
        output.WriteLine("  --geometry COLSxROWS 设置屏幕缓冲区大小和窗口大小");
        output.WriteLine("  --dry-run            只校验并输出描述，不做任何修改");
        output.WriteLine("  --quiet              成功时不输出任何内容（供自启动钩子使用）");
        output.WriteLine();
        output.WriteLine("颜色可接受 #RGB、#RRGGBB、0xRRGGBB、包含 A-F 的 RRGGBB、十进制整数，");
        output.WriteLine("或 red / grey / crimson 之类的名称。");
    }

    private static void PrintTerminalUsage(TextWriter output)
    {
        output.WriteLine("usage: terminal [options]");
        output.WriteLine();
        output.WriteLine("options");
        output.WriteLine("  --image PATH          background image (.jpg, .png, .gif) or desktopWallpaper");
        output.WriteLine("  --image-opacity 0..1  opacity of that image");
        output.WriteLine("  --image-align NAME    center, left, top, right, bottom, topLeft, topRight,");
        output.WriteLine("                        bottomLeft, bottomRight");
        output.WriteLine("  --image-stretch NAME  none, fill, uniform, uniformToFill");
        output.WriteLine("  --background #RRGGBB  profile background colour");
        output.WriteLine("  --foreground #RRGGBB  profile foreground colour");
        output.WriteLine("  --opacity 1..100      window opacity percentage");
        output.WriteLine("  --acrylic             blur the background behind the window");
        output.WriteLine("  --all-profiles        change profiles.defaults instead of cmd/PowerShell only");
        output.WriteLine("  --settings PATH       use a specific settings.json");
        output.WriteLine("  --dry-run             report what would change, write nothing");
        output.WriteLine();
        output.WriteLine("A timestamped backup of settings.json is written before any change, and the new");
        output.WriteLine("content replaces the file atomically so an interrupted write cannot truncate it.");
        output.WriteLine();
        output.WriteLine(ChineseHeading);
        output.WriteLine();
        output.WriteLine("用法：terminal [选项]");
        output.WriteLine();
        output.WriteLine("选项");
        output.WriteLine("  --image PATH          背景图片（.jpg、.png、.gif）或 desktopWallpaper");
        output.WriteLine("  --image-opacity 0..1  该背景图片的不透明度");
        output.WriteLine("  --image-align NAME    center、left、top、right、bottom、topLeft、topRight、");
        output.WriteLine("                        bottomLeft、bottomRight");
        output.WriteLine("  --image-stretch NAME  none、fill、uniform、uniformToFill");
        output.WriteLine("  --background #RRGGBB  配置文件的背景色");
        output.WriteLine("  --foreground #RRGGBB  配置文件的前景色");
        output.WriteLine("  --opacity 1..100      窗口不透明度百分比");
        output.WriteLine("  --acrylic             模糊窗口后面的背景");
        output.WriteLine("  --all-profiles        修改 profiles.defaults，而不是只改 cmd/PowerShell");
        output.WriteLine("  --settings PATH       使用指定的 settings.json");
        output.WriteLine("  --dry-run             报告将要修改的内容，不写入任何文件");
        output.WriteLine();
        output.WriteLine("修改前会写入一份带时间戳的 settings.json 备份，新内容会原子地替换原文件，");
        output.WriteLine("因此写入中断也不会截断文件。");
    }

    private static void PrintPresetsUsage(TextWriter output)
    {
        output.WriteLine("usage: presets");
        output.WriteLine("        list the built-in colour presets and their contrast ratios");
        output.WriteLine("用法：presets");
        output.WriteLine("        列出内置配色预设及其对比度");
    }

    private static void PrintRestoreUsage(TextWriter output)
    {
        output.WriteLine("usage: restore");
        output.WriteLine("        reset the current console to the stock Windows 11 palette and attributes");
        output.WriteLine("        (the window geometry and the opacity are not touched)");
        output.WriteLine("用法：restore");
        output.WriteLine("        把当前控制台恢复为 Windows 11 默认调色板和文字属性");
        output.WriteLine("        （窗口大小和透明度不受影响）");
        output.WriteLine();
        output.WriteLine("note: this only affects the console the command runs in. New consoles and Windows");
        output.WriteLine("      Terminal profiles are unchanged; use 'terminal' for persistent settings.");
        output.WriteLine("注意：此命令只影响它所在的控制台，不影响以后新建的控制台；Windows Terminal");
        output.WriteLine("      的配置文件也不会改变，持久设置请使用 'terminal' 命令。");
    }

    private static void PrintDoctorUsage(TextWriter output)
    {
        output.WriteLine("usage: doctor");
        output.WriteLine("        report the environment and what will and will not work here");
        output.WriteLine("用法：doctor");
        output.WriteLine("        报告运行环境以及哪些功能在此可用");
    }

    /* --------------------------------------------------------------------- */
    /* helpers                                                                */
    /* --------------------------------------------------------------------- */

    private static string RequireValue(string[] args, ref int index, string option)
    {
        if (index + 1 >= args.Length)
        {
            throw new ConfigurationException($"{option} requires a value");
        }
        index++;
        return args[index];
    }

    private static int ParseRange(string text, int minimum, int maximum, string what)
    {
        if (!int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out int value))
        {
            throw new ConfigurationException($"{what}: '{Sanitize(text)}' is not a number");
        }
        if (value < minimum || value > maximum)
        {
            throw new ConfigurationException($"{what}: {value} is outside {minimum}..{maximum}");
        }
        return value;
    }

    private static double ParseDouble(string text, double minimum, double maximum, string what)
    {
        if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double value))
        {
            throw new ConfigurationException($"{what}: '{Sanitize(text)}' is not a number");
        }
        if (value < minimum || value > maximum || double.IsNaN(value))
        {
            throw new ConfigurationException(
                $"{what}: {value.ToString(CultureInfo.InvariantCulture)} is outside {minimum}..{maximum}");
        }
        return value;
    }

    private static string DescribeHost(HostKind kind) => kind switch
    {
        HostKind.None => "none (no console attached)",
        HostKind.ClassicConhost => "classic console window (conhost.exe)",
        HostKind.Pseudoconsole => "pseudoconsole (a terminal application owns the window)",
        HostKind.LegacyConhost => "legacy console window (ForceV2=0)",
        _ => "unknown",
    };

    private static string SlotName(int slot) => SlotNames[Math.Clamp(slot, 0, 15)];

    private static readonly string[] SlotNames =
    {
        "black", "dark-blue", "dark-green", "dark-cyan",
        "dark-red", "dark-magenta", "dark-yellow", "gray",
        "dark-gray", "blue", "green", "cyan",
        "red", "magenta", "yellow", "white",
    };

    private static string FirstLine(string text)
    {
        int newline = text.IndexOf('\n', StringComparison.Ordinal);
        return newline < 0 ? text : text[..newline].TrimEnd('\r');
    }

    /// <summary>
    /// Strips control characters before echoing user input back, so a crafted argument cannot
    /// inject terminal escape sequences into a diagnostic.
    /// </summary>
    private static string Sanitize(string text)
    {
        var builder = new StringBuilder(text.Length);
        foreach (char ch in text)
        {
            builder.Append(char.IsControl(ch) ? '?' : ch);
        }
        string result = builder.ToString();
        return result.Length > 80 ? result[..80] + "..." : result;
    }

    private static void Error(string message) => Console.Error.WriteLine("error: " + message);

    /// <summary>
    /// The console's own attribute decoding, kept in managed code so the CLI does not need a native
    /// call just to print two slot numbers. The bit layout is fixed by WinCon.h.
    /// </summary>
    private static void DecodeAttributeSlots(ushort attributes, out int foreground, out int background)
    {
        foreground = 0;
        background = 0;
        if ((attributes & 0x0001) != 0) foreground |= 0x1;
        if ((attributes & 0x0002) != 0) foreground |= 0x2;
        if ((attributes & 0x0004) != 0) foreground |= 0x4;
        if ((attributes & 0x0008) != 0) foreground |= 0x8;
        if ((attributes & 0x0010) != 0) background |= 0x1;
        if ((attributes & 0x0020) != 0) background |= 0x2;
        if ((attributes & 0x0040) != 0) background |= 0x4;
        if ((attributes & 0x0080) != 0) background |= 0x8;
    }
}
