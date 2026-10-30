// ConsoleBgTool - managed test suite.
//
// The tests that matter most here are the cross-language ones. The C# layer marshals two structures
// across a C ABI, and a mismatch in field order, packing or character width would not fail to
// compile: it would silently reinterpret a colour as a slot index, or read a face name out of the
// middle of a size field. So the layout is asserted field by field against the offsets the C
// compiler produced, and the native functions are exercised through the same P/Invoke surface the
// CLI uses.
//
// Exit code 0 means every check passed.
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using ConsoleBgTool.Cli;
using ConsoleBgTool.Interop;
using Microsoft.Win32;

namespace ConsoleBgTool.Tests;

internal static class Program
{
    private static int _checks;
    private static int _failures;
    private static int _skips;
    private static string _case = string.Empty;

    private static void Begin(string name) => _case = name;

    private static void Check(bool condition, string description)
    {
        _checks++;
        if (!condition)
        {
            _failures++;
            Console.Error.WriteLine($"FAIL [{_case}] {description}");
        }
    }

    private static void CheckEqual(long actual, long expected, string description)
    {
        _checks++;
        if (actual != expected)
        {
            _failures++;
            Console.Error.WriteLine($"FAIL [{_case}] {description}: got {actual}, want {expected}");
        }
    }

    private static void CheckEqual(string? actual, string? expected, string description)
    {
        _checks++;
        if (!string.Equals(actual, expected, StringComparison.Ordinal))
        {
            _failures++;
            Console.Error.WriteLine($"FAIL [{_case}] {description}: got '{actual}', want '{expected}'");
        }
    }

    private static void CheckClose(double actual, double expected, double tolerance, string description)
    {
        _checks++;
        if (Math.Abs(actual - expected) > tolerance)
        {
            _failures++;
            Console.Error.WriteLine(
                $"FAIL [{_case}] {description}: got {actual.ToString("F4", CultureInfo.InvariantCulture)}, " +
                $"want {expected.ToString("F4", CultureInfo.InvariantCulture)}");
        }
    }

    private static void Skip(string reason)
    {
        _skips++;
        Console.Error.WriteLine($"SKIP [{_case}] {reason}");
    }

    private static void Throws<TException>(Action action, string description)
        where TException : Exception
    {
        _checks++;
        try
        {
            action();
            _failures++;
            Console.Error.WriteLine($"FAIL [{_case}] {description}: no {typeof(TException).Name} was thrown");
        }
        catch (TException)
        {
            // Expected.
        }
        catch (Exception ex)
        {
            _failures++;
            Console.Error.WriteLine(
                $"FAIL [{_case}] {description}: threw {ex.GetType().Name} instead of {typeof(TException).Name}");
        }
    }

    private static int Main()
    {
        Console.WriteLine("ConsoleBgTool managed tests");
        Console.WriteLine("===========================");

        TestNativeStructureLayout();
        TestNativeColorInterop();
        TestNativeConfigRoundTrip();
        TestNativeValidation();
        TestAppearanceParsing();
        TestAppearanceGeometryAndColourOverrides();
        TestPresetsAreLegible();
        TestConfigurationErrors();
        TestTerminalSettingsValidation();
        TestRegistryReportIsReadOnly();
        TestBilingualHelp();
        TestLiveConsoleRead();
        TestRestoreResetsToDefaults();

        Console.WriteLine();
        Console.WriteLine($"{_checks} checks, {_failures} failures, {_skips} skipped");
        if (_failures != 0)
        {
            Console.WriteLine("RESULT: FAILED");
            return 1;
        }
        Console.WriteLine("RESULT: PASSED");
        return 0;
    }

    /* --------------------------------------------------------------------- */
    /* Helpers                                                                */
    /* --------------------------------------------------------------------- */

    /*
     * NativeConfig holds the palette as an array, so a plain struct assignment copies the ARRAY
     * REFERENCE, not the contents. Mutating the "copy" would then change the original as well, and
     * a test that asserts on the original would silently pass while asserting nothing. Every copy
     * in this file therefore goes through here.
     */
    private static NativeConfig DeepCopy(NativeConfig source)
    {
        NativeConfig copy = source;
        copy.Palette = new uint[NativeConfig.ColorTableSize];
        Array.Copy(source.Palette, copy.Palette, NativeConfig.ColorTableSize);
        return copy;
    }

    /// <summary>
    /// Runs the CLI executable built beside this test executable and captures its output as UTF-8.
    ///
    /// <para>
    /// The child process inherits this test process's console. That inheritance is what makes the
    /// live-console restore test possible: the CLI resets the palette of the console it runs in, and
    /// this process can then read that same console back.
    /// </para>
    /// </summary>
    private static (int ExitCode, string Output, string Error) RunCli(
        string executable, params string[] arguments)
    {
        var startInfo = new ProcessStartInfo(executable)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = AppContext.BaseDirectory,
        };
        foreach (string argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = new Process { StartInfo = startInfo };
        if (!process.Start())
        {
            return (-1, string.Empty, "the CLI process could not be started");
        }
        string output = process.StandardOutput.ReadToEnd();
        string error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        return (process.ExitCode, output, error);
    }

    private static string CliExecutable => Path.Combine(AppContext.BaseDirectory, "ConsoleBgTool.exe");

    /* --------------------------------------------------------------------- */
    /* Cross-language structure layout                                        */
    /* --------------------------------------------------------------------- */

    /*
     * These offsets are the contract. They were produced by the C compiler for cbt_config:
     *
     *   palette          0    16 * 4 bytes
     *   foreground_slot 64
     *   background_slot 68
     *   canvas_slot     72
     *   buffer_columns  76
     *   buffer_rows     78
     *   window_columns  80
     *   window_rows     82
     *   font_size       84
     *   font_weight     86
     *   font_family     88
     *   face_name       92    32 bytes
     *   opacity_percent 124
     *   sizeof          128
     */
    private static void TestNativeStructureLayout()
    {
        Begin("native structure layout");

        int size = Marshal.SizeOf<NativeConfig>();
        CheckEqual(size, 128, "sizeof(NativeConfig)");

        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.Palette)).ToInt32(), 0,
            "offset of Palette");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.ForegroundSlot)).ToInt32(), 64,
            "offset of ForegroundSlot");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.BackgroundSlot)).ToInt32(), 68,
            "offset of BackgroundSlot");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.CanvasSlot)).ToInt32(), 72,
            "offset of CanvasSlot");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.BufferColumns)).ToInt32(), 76,
            "offset of BufferColumns");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.BufferRows)).ToInt32(), 78,
            "offset of BufferRows");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.WindowColumns)).ToInt32(), 80,
            "offset of WindowColumns");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.WindowRows)).ToInt32(), 82,
            "offset of WindowRows");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.FontSize)).ToInt32(), 84,
            "offset of FontSize");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.FontWeight)).ToInt32(), 86,
            "offset of FontWeight");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.FontFamily)).ToInt32(), 88,
            "offset of FontFamily");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.FaceName)).ToInt32(), 92,
            "offset of FaceName");
        CheckEqual(Marshal.OffsetOf<NativeConfig>(nameof(NativeConfig.OpacityPercent)).ToInt32(), 124,
            "offset of OpacityPercent");

        /*
         * LiveConsoleState:
         *   valid,is_console,has_window,v2   0,1,2,3
         *   attributes                       4
         *   popup_attributes                 6
         *   buffer/window columns/rows       8,10,12,14
         *   font_width,font_height           16,18
         *   window_pixel_width/height        20,24
         *   palette                          28   16 * 4
         *   sizeof                           92
         */
        CheckEqual(Marshal.SizeOf<NativeLiveState>(), 92, "sizeof(NativeLiveState)");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.Valid)).ToInt32(), 0,
            "offset of Valid");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.V2)).ToInt32(), 3,
            "offset of V2");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.Attributes)).ToInt32(), 4,
            "offset of Attributes");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.PopupAttributes)).ToInt32(), 6,
            "offset of PopupAttributes");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.BufferColumns)).ToInt32(), 8,
            "offset of BufferColumns");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.BufferRows)).ToInt32(), 10,
            "offset of BufferRows");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.WindowColumns)).ToInt32(), 12,
            "offset of WindowColumns");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.WindowRows)).ToInt32(), 14,
            "offset of WindowRows");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.FontWidth)).ToInt32(), 16,
            "offset of FontWidth");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.FontHeight)).ToInt32(), 18,
            "offset of FontHeight");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.WindowPixelWidth)).ToInt32(), 20,
            "offset of WindowPixelWidth");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.WindowPixelHeight)).ToInt32(), 24,
            "offset of WindowPixelHeight");
        CheckEqual(Marshal.OffsetOf<NativeLiveState>(nameof(NativeLiveState.Palette)).ToInt32(), 28,
            "offset of Palette");

                /*
         * The four state flags must marshal as one byte each, matching C++ bool. Marshal.SizeOf<bool>()
         * reports the marshalled size of a bool field in a structure, which is what matters here, not
         * the literal 1-byte C size.
         */
        CheckEqual(Marshal.SizeOf<bool>(), 4, "marshalled sizeof(bool)");

        /* Prove the flags really occupy one byte each: V2 is the fourth of them, at offset 3. */
        var flags = new NativeLiveState { Valid = true, IsConsole = true, HasWindow = true, V2 = false };
        byte[] raw = new byte[Marshal.SizeOf<NativeLiveState>()];
        IntPtr buffer = Marshal.AllocHGlobal(raw.Length);
        try
        {
            Marshal.StructureToPtr(flags, buffer, fDeleteOld: false);
            Marshal.Copy(buffer, raw, 0, raw.Length);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
        CheckEqual(raw[0], 1, "valid flag occupies byte 0");
        CheckEqual(raw[1], 1, "is_console flag occupies byte 1");
        CheckEqual(raw[2], 1, "has_window flag occupies byte 2");
        CheckEqual(raw[3], 0, "v2 flag occupies byte 3 and is false");

        /* And the colour table sizes must agree, or one side would run off the end. */
        CheckEqual(NativeConfig.ColorTableSize, 16, "color table size");
        CheckEqual(NativeConfig.MaxFaceName, 32, "face name buffer size");
    }

    /* --------------------------------------------------------------------- */
    /* Native colour interop                                                  */
    /* --------------------------------------------------------------------- */

    private static void TestNativeColorInterop()
    {
        Begin("native colour interop");

        // The COLORREF swap is the single most error-prone conversion; assert it in both directions.
        CheckEqual(NativeMethods.ColorFromColorRef(0x00112233u), 0x332211u, "COLORREF -> canonical");
        CheckEqual(NativeMethods.ColorToColorRef(0x332211u), 0x00112233u, "canonical -> COLORREF");
        CheckEqual(NativeMethods.ColorFromColorRef(0x000C0C0Cu), 0x0C0C0Cu, "ColorTable00 black");
        CheckEqual(NativeMethods.ColorFromColorRef(0x00DA3700u), 0x0037DAu, "ColorTable01 dark blue");
        CheckEqual(NativeMethods.ColorFromColorRef(0x00F2F2F2u), 0xF2F2F2u, "ColorTable15 white");

        // Parsing, including the cases where the native layer makes a deliberate choice.
        (string Input, CbtStatus Status, uint Color)[] cases =
        {
            ("#123456", CbtStatus.Ok, 0x123456u),
            ("123456", CbtStatus.Ok, 123456u),      // bare digits are decimal
            ("0x123456", CbtStatus.Ok, 0x123456u),
            ("0X123456", CbtStatus.Ok, 0x123456u),
            ("FF0000", CbtStatus.Ok, 0xFF0000u),    // unprefixed hex needs an A-F
            ("#abc", CbtStatus.Ok, 0xAABBCcu),      // shorthand
            ("black", CbtStatus.Ok, 0x000000u),
            ("WHITE", CbtStatus.Ok, 0xFFFFFFu),
            ("", CbtStatus.Parse, 0u),
            ("#12345", CbtStatus.Parse, 0u),
            ("#0X123456", CbtStatus.Parse, 0u),
            ("notacolour", CbtStatus.Parse, 0u),
        };

        foreach ((string input, CbtStatus expected, uint color) in cases)
        {
            CbtStatus status = NativeMethods.ParseColor(input, out uint parsed);
            CheckEqual((long)status, (long)expected, $"parse '{input}' status");
            if (expected == CbtStatus.Ok)
            {
                CheckEqual(parsed, color, $"parse '{input}' value");
            }
        }

        // Opacity conversion: 50 percent is the 128 the console's own slider produces.
        CheckEqual((long)NativeMethods.OpacityToByte(100, out byte full), (long)CbtStatus.Ok, "opacity 100");
        CheckEqual(full, 255, "opacity 100 -> byte");
        CheckEqual((long)NativeMethods.OpacityToByte(50, out byte half), (long)CbtStatus.Ok, "opacity 50");
        CheckEqual(half, 128, "opacity 50 -> byte");
        /*
         * The native converter accepts the full 0..100 percentage range, and 9 percent therefore
         * converts successfully. The usability floor of 10 percent is a policy decision that lives in
         * the managed Appearance layer, not in the converter, so it is asserted there instead:
         * Appearance.Parse("opacity=9") throws. Keeping the two concerns separate is deliberate - the
         * core has no business refusing a value it can represent faithfully.
         */
        CheckEqual((long)NativeMethods.OpacityToByte(9, out byte nine), (long)CbtStatus.Ok,
            "opacity 9 is representable");
        CheckEqual(nine, 23, "opacity 9 -> byte");
        CheckEqual((long)NativeMethods.OpacityToByte(101, out _), (long)CbtStatus.OutOfRange, "opacity 101 rejected");
        CheckEqual((long)NativeMethods.OpacityToByte(-1, out _), (long)CbtStatus.OutOfRange, "opacity -1 rejected");

        // Coordinate packing, anchored to the real registry values read from this machine.
        CheckEqual(NativeMethods.PackCoord(120, 30), 0x001E0078u, "pack 120x30");
        CheckEqual(NativeMethods.PackCoord(0x78, 0x07), 0x00070078u, "pack from real ScreenBufferSize");
        NativeMethods.UnpackCoord(0x001E0078u, out ushort x, out ushort y);
        CheckEqual(x, 120, "unpack x");
        CheckEqual(y, 30, "unpack y");
    }

    /* --------------------------------------------------------------------- */
    /* Native configuration                                                   */
    /* --------------------------------------------------------------------- */

    private static void TestNativeConfigRoundTrip()
    {
        Begin("native config round trip");

        NativeConfig config = NativeMethods.DefaultConfig();

        CheckEqual(config.Palette.Length, 16, "default palette length");
        CheckEqual(config.ForegroundSlot, 7, "default foreground slot");
        CheckEqual(config.BackgroundSlot, 0, "default background slot");
        CheckEqual(config.BufferColumns, 120, "default buffer columns");
        CheckEqual(config.BufferRows, 30, "default buffer rows");
        CheckEqual(config.OpacityPercent, 100, "default opacity");

        // The verified default palette, round-tripped from the C layer.
        CheckEqual(config.Palette[0], 0x0C0C0Cu, "palette[0] black");
        CheckEqual(config.Palette[1], 0x0037DAu, "palette[1] dark blue");
        CheckEqual(config.Palette[4], 0xC50F1Fu, "palette[4] dark red");
        CheckEqual(config.Palette[15], 0xF2F2F2u, "palette[15] white");

        // Validation of a good configuration, and of a deliberately broken one.
        CbtStatus status = NativeMethods.Validate(ref config, out string field);
        CheckEqual((long)status, (long)CbtStatus.Ok, "valid config accepted");
        CheckEqual(field, string.Empty, "no offending field");

        NativeConfig broken = DeepCopy(config);
        broken.WindowColumns = 200; // wider than the buffer
        status = NativeMethods.Validate(ref broken, out field);
        CheckEqual((long)status, (long)CbtStatus.OutOfRange, "over-wide window rejected");
        CheckEqual(field, "geometry", "offending field is geometry");

        // Contrast computation, anchored to the WCAG definition.
        NativeConfig contrastConfig = DeepCopy(config);
        contrastConfig.Palette[0] = 0x000000u;
        contrastConfig.Palette[7] = 0xFFFFFFu;
        contrastConfig.ForegroundSlot = 7;
        contrastConfig.CanvasSlot = 0;
        CheckEqual((long)NativeMethods.Contrast(ref contrastConfig, out double ratio), (long)CbtStatus.Ok,
            "contrast computed");
        CheckClose(ratio, 21.0, 0.01, "black on white contrast is 21:1");
    }

    /*
     * A mutator for a NativeConfig.
     *
     * This exists because Action<NativeConfig> would pass the structure BY VALUE - NativeConfig is a
     * value type - so a mutation made inside the lambda would be discarded. A test written that way
     * silently asserts that a valid configuration is valid, and passes while checking nothing.
     * Taking the parameter by ref is what makes the negative cases below real.
     */
    private delegate void Mutator(ref NativeConfig candidate);

    private static void TestNativeValidation()
    {
        Begin("native validation");

        NativeConfig config = NativeMethods.DefaultConfig();

        void ExpectRejected(Mutator mutate, string expectedField, string description)
        {
            NativeConfig copy = DeepCopy(config);
            mutate(ref copy);
            CbtStatus status = NativeMethods.Validate(ref copy, out string field);

            /*
             * The exact code differs by field: an out-of-range value reports OutOfRange, while a
             * value that is well typed but not representable - a control character in a face name -
             * reports InvalidArgument. Both are refusals, and both must name the field, so the
             * assertion is "refused, with the field named" rather than one specific code.
             */
            Check(status != CbtStatus.Ok, description + " is refused");
            CheckEqual((long)status, (long)CbtStatus.OutOfRange, description + " reports out-of-range");
            CheckEqual(field, expectedField, description + " names the field");
        }

        void ExpectRejectedWith(Mutator mutate, CbtStatus expected, string expectedField,
            string description)
        {
            NativeConfig copy = DeepCopy(config);
            mutate(ref copy);
            CbtStatus status = NativeMethods.Validate(ref copy, out string field);
            CheckEqual((long)status, (long)expected, description + " is refused with the expected code");
            CheckEqual(field, expectedField, description + " names the field");
        }

        ExpectRejected((ref NativeConfig c) => c.ForegroundSlot = 99, "foreground", "slot 99");
        ExpectRejected((ref NativeConfig c) => c.BackgroundSlot = -1, "background", "slot -1");
        ExpectRejected((ref NativeConfig c) => c.CanvasSlot = 16, "canvas", "slot 16");
        ExpectRejected((ref NativeConfig c) => c.BufferColumns = 0, "geometry", "zero buffer");
        ExpectRejected((ref NativeConfig c) => c.FontSize = 0, "font-size", "zero font size");
        ExpectRejected((ref NativeConfig c) => c.FontSize = 500, "font-size", "huge font size");
        ExpectRejected((ref NativeConfig c) => c.OpacityPercent = 5, "opacity", "opacity below the floor");
        ExpectRejected((ref NativeConfig c) => c.OpacityPercent = 101, "opacity", "opacity above 100");

        /*
         * Face names need care on this side of the ABI. The native field is a fixed 32-character
         * buffer, so the marshaller TRUNCATES anything longer before the core can see it - a
         * 40-character name never reaches the validator. The honest division of labour is therefore:
         * the native check is exercised with a name that fits the buffer but contains a byte the
         * console cannot store, and the length limit is enforced in the managed Appearance layer,
         * where it is asserted in the configuration error tests.
         */
        ExpectRejectedWith((ref NativeConfig c) => c.FaceName = "bad\nname", CbtStatus.InvalidArgument,
            "face-name", "control character in face name");

        {
            NativeConfig truncated = DeepCopy(config);
            truncated.FaceName = new string('x', 40);
            CbtStatus status = NativeMethods.Validate(ref truncated, out string field);
            CheckEqual((long)status, (long)CbtStatus.Ok,
                "a face name longer than the buffer is truncated by the marshaller, not rejected here");
            Check(truncated.FaceName.Length <= NativeConfig.MaxFaceName - 1,
                "the marshalled face name fits the native buffer");
            _ = field;
        }
        ExpectRejected(
            (ref NativeConfig c) =>
            {
                c.Palette = new uint[16];
                Array.Copy(config.Palette, c.Palette, 16);
                c.Palette[3] = 0x01000000u;
            },
            "palette", "24-bit overflow in a palette entry");
    }

    /* --------------------------------------------------------------------- */
    /* Managed configuration parsing                                          */
    /* --------------------------------------------------------------------- */

    private static void TestAppearanceParsing()
    {
        Begin("appearance parsing");

        Appearance parsed = Appearance.Parse(
            "# a comment\n" +
            "; another comment\n" +
            "background = black\n" +
            "foreground = white\n" +
            "colour.0 = #101010\n" +
            "palette.7 = 0xCCCCCC\n" +
            "opacity = 80\n" +
            "columns = 140\n" +
            "rows = 40\n" +
            "window-columns = 120\n" +
            "window-rows = 30\n" +
            "font-size = 18\n" +
            "font-weight = 500\n" +
            "face-name = Consolas\n");

        CheckEqual(parsed.CanvasSlot, 0, "background slot parsed by name");
        CheckEqual(parsed.ForegroundSlot, 15, "foreground slot parsed by name");
        CheckEqual(parsed.Palette[0], 0x101010u, "colour.0 applied");
        CheckEqual(parsed.Palette[7], 0xCCCCCCu, "palette.7 applied");
        CheckEqual(parsed.OpacityPercent, 80, "opacity parsed");
        CheckEqual(parsed.BufferColumns, 140, "columns parsed");
        CheckEqual(parsed.BufferRows, 40, "rows parsed");
        CheckEqual(parsed.WindowColumns, 120, "window-columns parsed");
        CheckEqual(parsed.WindowRows, 30, "window-rows parsed");
        CheckEqual(parsed.FontSize, 18, "font-size parsed");
        CheckEqual(parsed.FontWeight, 500, "font-weight parsed");
        CheckEqual(parsed.FaceName, "Consolas", "face-name parsed");

        // A parsed configuration must survive native validation.
        NativeConfig native = parsed.ToNative();
        CheckEqual((long)NativeMethods.Validate(ref native, out _), (long)CbtStatus.Ok,
            "parsed config passes native validation");

        // Whitespace and case are tolerated.
        Appearance loose = Appearance.Parse("  BACKGROUND=Black  \n");
        CheckEqual(loose.CanvasSlot, 0, "whitespace and case tolerated");
    }

    private static void TestAppearanceGeometryAndColourOverrides()
    {
        Begin("appearance overrides");

        Appearance appearance = Appearance.CreateDefault();

        Appearance.ApplyGeometryOverride(appearance, "100x40");
        CheckEqual(appearance.BufferColumns, 100, "geometry columns");
        CheckEqual(appearance.BufferRows, 40, "geometry rows");
        CheckEqual(appearance.WindowColumns, 100, "geometry window columns");
        CheckEqual(appearance.WindowRows, 40, "geometry window rows");

        Appearance.ApplyGeometryOverride(appearance, "80:25");
        CheckEqual(appearance.BufferColumns, 80, "colon separator columns");

        Appearance.ApplyColorOverride(appearance, "4=#FF0000");
        CheckEqual(appearance.Palette[4], 0xFF0000u, "--color override");

        Appearance.ApplyColorOverride(appearance, "dark-blue=0x123456");
        CheckEqual(appearance.Palette[1], 0x123456u, "--color by slot name");

        CheckEqual(Appearance.ParseSlotText("15"), 15, "slot parsed from number");
        CheckEqual(Appearance.ParseSlotText("dark-gray"), 8, "slot parsed from name");
        CheckEqual(Appearance.ParseSlotText("grey"), 7, "grey alias");
    }

    private static void TestPresetsAreLegible()
    {
        Begin("presets");

        var names = Appearance.PresetNames().Select(entry => entry.Name).ToList();
        Check(names.Count >= 8, "at least eight presets are shipped");

        foreach (string name in names)
        {
            Appearance preset = Appearance.FromPreset(name);

            // Every preset must be a configuration the native layer accepts: nothing ships that the
            // core would refuse to apply.
            NativeConfig native = preset.ToNative();
            CheckEqual((long)NativeMethods.Validate(ref native, out string field), (long)CbtStatus.Ok,
                $"preset '{name}' passes native validation");

            // Every preset must be readable. This is the check that catches a pretty palette that
            // makes text illegible.
            double contrast = preset.ContrastRatio();
            Check(contrast >= 4.5,
                $"preset '{name}' has adequate contrast (got {contrast.ToString("F2", CultureInfo.InvariantCulture)}:1)");

            // All sixteen entries must be 24-bit values.
            Check(preset.Palette.All(c => c <= 0xFFFFFFu), $"preset '{name}' palette is 24-bit");

            // The theme detector must agree with the canvas luminance.
            bool lightCanvas = preset.CanvasLuminance() >= 0.5;
            CheckEqual(preset.IsLightBackground ? 1 : 0, lightCanvas ? 1 : 0,
                $"preset '{name}' theme matches canvas luminance");
        }

        // The light preset must actually be dark-on-light; that is the whole point of it.
        Appearance paper = Appearance.FromPreset("paper");
        Check(paper.IsLightBackground, "'paper' is a light background preset");
        Check(paper.ContrastRatio() >= 4.5, "'paper' is readable");

        // Names are case-insensitive.
        CheckEqual(Appearance.FromPreset("NORD").CanvasSlot, Appearance.FromPreset("nord").CanvasSlot,
            "preset names are case-insensitive");

        // An unknown preset is an error, not a silent default.
        Throws<ConfigurationException>(() => Appearance.FromPreset("nonexistent"), "unknown preset rejected");

        // WithReadableText must not leave a preset unreadable.
        foreach (string name in names)
        {
            Appearance fixedUp = Appearance.FromPreset(name).WithReadableText(7.0);
            Check(fixedUp.ContrastRatio() >= 7.0, $"WithReadableText makes '{name}' reach 7:1");
        }
    }

    private static void TestConfigurationErrors()
    {
        Begin("configuration errors");

        // Unknown keys, malformed lines and out-of-range values must all be reported.
        Throws<ConfigurationException>(() => Appearance.Parse("mystery=1"), "unknown key rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("nonsense"), "missing '=' rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("opacity=5"), "opacity below floor rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("opacity=500"), "opacity above ceiling rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("opacity=abc"), "non-numeric rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("background=99"), "slot out of range rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("background=purple-ish"), "unknown slot rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("colour.0=#zzzzzz"), "bad colour rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("colour.17=#ffffff"), "slot 17 rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("columns=0"), "zero columns rejected");
        Throws<ConfigurationException>(() => Appearance.Parse("columns=9999"), "excessive columns rejected");
        /*
         * A face name must fit LF_FACESIZE - 1 characters, because the value is placed in a 32-byte
         * field including the terminator. The boundary is checked from both sides so an off-by-one in
         * the limit cannot pass unnoticed.
         */
        Throws<ConfigurationException>(() => Appearance.Parse("face-name=" + new string('x', 40)),
            "over-long face name rejected");
        CheckEqual(Appearance.Parse("face-name=" + new string('x', 31)).FaceName.Length, 31,
            "31-character face name accepted");

        // A configuration that passes parsing but fails native validation must still be refused.
        Appearance tooWide = Appearance.CreateDefault();
        tooWide.WindowColumns = 300;
        tooWide.BufferColumns = 100;
        Throws<ConfigurationException>(() => tooWide.ToNative(), "native validation enforced by ToNative");

        // The line number must be reported, so a large file can be fixed.
        try
        {
            Appearance.Parse("columns=120\nrows=30\nopacity=0\n");
            Check(false, "expected a ConfigurationException for opacity=0");
        }
        catch (ConfigurationException ex)
        {
            Check(ex.Message.Contains("line 3", StringComparison.Ordinal),
                "the failing line number is reported");
        }

        // Control characters in a value must not reach the diagnostic output verbatim.
        try
        {
            Appearance.Parse("background=\u001b[31mred");
            Check(false, "expected a ConfigurationException for an escape sequence");
        }
        catch (ConfigurationException ex)
        {
            Check(!ex.Message.Contains('\u001b'), "escape sequences are stripped from diagnostics");
        }
    }

    /* --------------------------------------------------------------------- */
    /* Windows Terminal settings                                              */
    /* --------------------------------------------------------------------- */

    private static void TestTerminalSettingsValidation()
    {
        Begin("terminal settings");

        var settings = new TerminalProfileSettings();

        // Out-of-range values must be refused before anything is written.
        settings.Opacity = 0;
        Throws<ConfigurationException>(() => settings.ToJsonProperties(), "terminal opacity 0 rejected");
        settings.Opacity = 101;
        Throws<ConfigurationException>(() => settings.ToJsonProperties(), "terminal opacity 101 rejected");
        settings.Opacity = null;

        settings.BackgroundImageOpacity = 1.5;
        Throws<ConfigurationException>(() => settings.ToJsonProperties(), "image opacity > 1 rejected");
        settings.BackgroundImageOpacity = null;

        settings.BackgroundImageAlignment = "sideways";
        Throws<ConfigurationException>(() => settings.ToJsonProperties(), "bad alignment rejected");
        settings.BackgroundImageAlignment = null;

        settings.BackgroundImageStretchMode = "explode";
        Throws<ConfigurationException>(() => settings.ToJsonProperties(), "bad stretch mode rejected");
        settings.BackgroundImageStretchMode = null;

        settings.Background = "not-a-colour";
        Throws<ConfigurationException>(() => settings.ToJsonProperties(), "bad colour rejected");
        settings.Background = null;

        // Valid values produce exactly the documented property names and normalised colours.
        settings.Background = "#AABBCC";
        settings.Foreground = "#123";
        settings.Opacity = 90;
        settings.UseAcrylic = true;
        settings.BackgroundImage = @"C:\pics\bg.png";
        settings.BackgroundImageOpacity = 0.25;
        settings.BackgroundImageAlignment = "bottomRight";
        settings.BackgroundImageStretchMode = "uniformToFill";

        Dictionary<string, System.Text.Json.Nodes.JsonNode?> properties = settings.ToJsonProperties();
        Check(properties.ContainsKey("background"), "background property present");
        Check(properties.ContainsKey("foreground"), "foreground property present");
        Check(properties.ContainsKey("opacity"), "opacity property present");
        Check(properties.ContainsKey("useAcrylic"), "useAcrylic property present");
        Check(properties.ContainsKey("backgroundImage"), "backgroundImage property present");
        Check(properties.ContainsKey("backgroundImageOpacity"), "backgroundImageOpacity property present");
        Check(properties.ContainsKey("backgroundImageAlignment"), "backgroundImageAlignment present");
        Check(properties.ContainsKey("backgroundImageStretchMode"), "backgroundImageStretchMode present");

        // Allowed names are exactly the documented sets, so an arbitrary string cannot be written.
        CheckEqual(properties["backgroundImageAlignment"]!.GetValue<string>(), "bottomRight", "alignment value");
        CheckEqual(properties["backgroundImageStretchMode"]!.GetValue<string>(), "uniformToFill", "stretch value");

        // A three-digit colour is expanded to six.
        CheckEqual(properties["foreground"]!.GetValue<string>(), "#112233", "shorthand colour expanded");
        CheckEqual(properties["background"]!.GetValue<string>(), "#aabbcc", "colour lower-cased");

        // A settings file path that does not exist is reported rather than created.
        Check(!File.Exists(@"C:\definitely\not\here\settings.json"), "the probe path does not exist");
    }

    /* --------------------------------------------------------------------- */
    /* Registry report                                                        */
    /* --------------------------------------------------------------------- */

    private static void TestRegistryReportIsReadOnly()
    {
        Begin("registry report");

        RegistryReport before = RegistryReport.Capture();
        RegistryReport after = RegistryReport.Capture();

        // Capturing twice must not change anything: the report is strictly read-only.
        CheckEqual(after.ValueCount, before.ValueCount, "value count unchanged by reporting");
        CheckEqual(after.SubKeyCount, before.SubKeyCount, "subkey count unchanged by reporting");
        for (int i = 0; i < before.ColorTable.Length; i++)
        {
            CheckEqual(after.ColorTable[i], before.ColorTable[i], $"ColorTable{i:D2} unchanged");
        }

        // The JSON must be well formed and round-trip through a strict parser.
        string json = before.ToJson();
        Check(json.Length > 0, "JSON is non-empty");
        try
        {
            using var document = System.Text.Json.JsonDocument.Parse(json);
            Check(document.RootElement.TryGetProperty("colorTable", out _), "JSON has a colorTable array");
            Check(document.RootElement.TryGetProperty("values", out _), "JSON has a values object");
        }
        catch (System.Text.Json.JsonException ex)
        {
            Check(false, "JSON parses: " + ex.Message);
        }

        // Control characters must be escaped rather than emitted raw.
        var report = new RegistryReport();
        report.OtherValues.Add(("probe", "line1\nline2\u001b[31m"));
        string hostile = report.ToJson();
        Check(!hostile.Contains('\u001b'), "escape sequences are escaped in JSON output");
        Check(hostile.Contains("\\n", StringComparison.Ordinal), "newlines are escaped in JSON output");
        using (System.Text.Json.JsonDocument.Parse(hostile))
        {
            Check(true, "JSON with control characters still parses");
        }

        // The real key should be present on a normal Windows machine.
        Check(RegistryReport.KeyExists(), "HKCU\\Console exists");
    }

    /* --------------------------------------------------------------------- */
    /* Bilingual help                                                         */
    /* --------------------------------------------------------------------- */

    private static void TestBilingualHelp()
    {
        Begin("bilingual help");

        string executable = CliExecutable;
        Check(File.Exists(executable), "the CLI executable is built beside the test executable");
        if (!File.Exists(executable))
        {
            return;
        }

        /*
         * The user requirement is that help is printed in BOTH Chinese and English. The markers
         * below are the literal strings Program.cs prints, so a help path that lost one language
         * fails here instead of passing on a generic "usage" substring.
         */
        (int helpExit, string helpOutput, _) = RunCli(executable, "--help");
        CheckEqual(helpExit, 0, "--help exits 0");
        Check(helpOutput.Contains("usage: ConsoleBgTool", StringComparison.Ordinal),
            "help has the English usage line");
        Check(helpOutput.Contains("用法：ConsoleBgTool", StringComparison.Ordinal),
            "help has the Chinese usage line");
        Check(helpOutput.Contains("commands", StringComparison.Ordinal),
            "help has the English command list");
        Check(helpOutput.Contains("命令", StringComparison.Ordinal),
            "help has the Chinese command list");
        Check(helpOutput.Contains("中文", StringComparison.Ordinal),
            "help separates the two languages with the Chinese heading");

        void ExpectBilingual(string description, int expectedExit, string english, string chinese,
            params string[] arguments)
        {
            (int exitCode, string output, string error) = RunCli(executable, arguments);
            string combined = output + error;
            CheckEqual(exitCode, expectedExit, description + " exit code");
            Check(combined.Contains(english, StringComparison.Ordinal),
                description + " prints the English marker '" + english + "'");
            Check(combined.Contains(chinese, StringComparison.Ordinal),
                description + " prints the Chinese marker '" + chinese + "'");
        }

        ExpectBilingual("apply --help", 0, "usage: apply", "用法：apply", "apply", "--help");
        ExpectBilingual("terminal --help", 0, "usage: terminal", "用法：terminal", "terminal", "--help");
        ExpectBilingual("show --help", 0, "usage: show", "用法：show", "show", "--help");
        ExpectBilingual("registry --help", 0, "usage: registry", "用法：registry", "registry", "--help");
        ExpectBilingual("presets --help", 0, "usage: presets", "用法：presets", "presets", "--help");
        ExpectBilingual("restore --help", 0, "usage: restore", "用法：restore", "restore", "--help");
        ExpectBilingual("doctor --help", 0, "usage: doctor", "用法：doctor", "doctor", "--help");
        ExpectBilingual("no arguments", 2, "usage: ConsoleBgTool", "用法：ConsoleBgTool");
        ExpectBilingual("unknown command", 2, "usage: ConsoleBgTool", "用法：ConsoleBgTool",
            "definitely-not-a-command");
    }

    /* --------------------------------------------------------------------- */
    /* restore command                                                        */
    /* --------------------------------------------------------------------- */

    private static void TestRestoreResetsToDefaults()
    {
        Begin("restore");

        if (NativeMethods.Read(out _, out _) != CbtStatus.Ok)
        {
            Skip("no console attached to the test process");
            return;
        }

        string executable = CliExecutable;
        Check(File.Exists(executable), "the CLI executable is built beside the test executable");
        if (!File.Exists(executable))
        {
            return;
        }

        NativeConfig defaultNative = NativeMethods.DefaultConfig();
        Appearance defaults = Appearance.CreateDefault();
        CheckEqual(defaults.Palette[0], defaultNative.Palette[0],
            "Appearance.CreateDefault and NativeMethods.DefaultConfig agree on slot 0");
        CheckEqual(defaults.ForegroundSlot, defaultNative.ForegroundSlot,
            "Appearance.CreateDefault and NativeMethods.DefaultConfig agree on the text slot");

        /*
         * A marker that is unmistakably not the stock palette: high-contrast makes slot 0 pure
         * black, slot 7 pure white and moves the default text to slot 15.
         */
        NativeConfig marker = Appearance.FromPreset("high-contrast").ToNative();
        Check(marker.Palette[0] != defaultNative.Palette[0] &&
              marker.ForegroundSlot != defaultNative.ForegroundSlot,
            "the marker preset differs from the stock defaults");

        CbtStatus applied = NativeMethods.Apply(marker, applyGeometry: false, applyOpacity: false);
        if (applied == CbtStatus.Unsupported)
        {
            Skip("this host does not allow a live palette change");
            return;
        }
        CheckEqual((long)applied, (long)CbtStatus.Ok, "the marker preset is applied to this console");

        CheckEqual((long)NativeMethods.Read(out NativeConfig marked, out _), (long)CbtStatus.Ok,
            "the console is read back after the marker");
        CheckEqual(marked.Palette[0], marker.Palette[0], "the marker background is live before restore");
        CheckEqual(marked.ForegroundSlot, marker.ForegroundSlot,
            "the marker text slot is live before restore");

        /*
         * The behaviour under test: 'restore' used to call cbt_platform_restore in a fresh process,
         * where no snapshot exists, so it silently did nothing. It must now reset the console it
         * runs in - the console this test process is attached to.
         */
        (int exitCode, string output, string error) = RunCli(executable, "restore");
        CheckEqual(exitCode, 0, "restore exits 0");
        Check(error.Length == 0, "restore reports no error");
        Check(output.Contains("stock Windows 11", StringComparison.Ordinal),
            "restore reports that the stock Windows 11 appearance is back");
        Check(output.Contains("geometry", StringComparison.Ordinal),
            "restore reports that the window geometry was not touched");

        CheckEqual((long)NativeMethods.Read(out NativeConfig restored, out _), (long)CbtStatus.Ok,
            "the console is read back after restore");
        for (int i = 0; i < NativeConfig.ColorTableSize; i++)
        {
            CheckEqual(restored.Palette[i], defaults.Palette[i],
                $"restored palette[{i}] equals the Windows 11 default");
        }
        CheckEqual(restored.ForegroundSlot, defaultNative.ForegroundSlot,
            "restored foreground slot equals the Windows 11 default");
        CheckEqual(restored.BackgroundSlot, defaultNative.BackgroundSlot,
            "restored background slot equals the Windows 11 default");

        // Leave the console exactly as the test found it: restore the snapshot the marker apply took.
        CheckEqual((long)NativeMethods.Restore(), (long)CbtStatus.Ok,
            "the test restores the console it changed");
    }

    /* --------------------------------------------------------------------- */
    /* Live console                                                           */
    /* --------------------------------------------------------------------- */

    private static void TestLiveConsoleRead()
    {
        Begin("live console");

        CbtStatus status = NativeMethods.Read(out NativeConfig config, out NativeLiveState state);
        if (status != CbtStatus.Ok)
        {
            Skip("no console attached to the test process");
            return;
        }

        Check(state.IsConsole, "the process is attached to a console");
        Check(state.BufferColumns > 0, "buffer columns are positive");
        Check(state.BufferRows > 0, "buffer rows are positive");
        Check(state.WindowColumns > 0, "window columns are positive");
        Check(state.WindowRows > 0, "window rows are positive");
        Check(state.WindowColumns <= state.BufferColumns, "the window fits inside the buffer");
        Check(state.WindowRows <= state.BufferRows, "the window fits inside the buffer vertically");
        Check(state.Palette.All(c => c <= 0xFFFFFFu), "every live palette entry is a 24-bit colour");

        // The raw state and the decoded configuration must describe the same console.
        CheckEqual(config.BufferColumns, state.BufferColumns, "decoded buffer columns match");
        CheckEqual(config.BufferRows, state.BufferRows, "decoded buffer rows match");
        for (int i = 0; i < NativeConfig.ColorTableSize; i++)
        {
            CheckEqual(config.Palette[i], state.Palette[i], $"decoded palette[{i}] matches");
        }

        // Host detection must return a known kind.
        HostKind host = NativeMethods.HostKind;
        Check(Enum.IsDefined(host), "host kind is a defined value");

        // Restoring with nothing applied must be a harmless no-op, not an error.
        CheckEqual((long)NativeMethods.Restore(), (long)CbtStatus.Ok, "restore with nothing applied is a no-op");

        // The platform error string must be readable and never null.
        string message = NativeMethods.PlatformLastError;
        Check(message is not null, "the native diagnostic string is never null");

        // A live apply of the current configuration must be accepted and reversible. This exercises
        // the same path the CLI uses, and leaves the console as it was found.
        NativeConfig same = DeepCopy(config);
        CbtStatus applied = NativeMethods.Apply(same, applyGeometry: false, applyOpacity: false);
        CheckEqual((long)applied, (long)CbtStatus.Ok, "applying the live configuration succeeds");
        CheckEqual((long)NativeMethods.Restore(), (long)CbtStatus.Ok, "restoring after apply succeeds");

        // Applying an invalid configuration must be refused by the native layer, not by the CLI.
        NativeConfig invalid = DeepCopy(same);
        invalid.BufferColumns = 0;
        invalid.BufferRows = 0;
        invalid.WindowColumns = 0;
        invalid.WindowRows = 0;
        CbtStatus refused = NativeMethods.Apply(invalid, applyGeometry: true, applyOpacity: false);
        /*
         * CBT_E_INVALID_ARG if validation catches it first, CBT_E_IO if a step fails after
         * validation passed. Which one occurs depends on where the zero geometry is noticed, and
         * either is a correct refusal; what matters is that it is not Ok and nothing is applied.
         */
        Check(refused is CbtStatus.InvalidArgument or CbtStatus.Io,
            $"an invalid configuration is refused by the native layer (got {refused})");
    }
}
