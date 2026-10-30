// ConsoleBgTool - C# interop layer.
//
// This file is the only place that knows the native ABI. Everything above it works with the
// managed types declared here, so a mistake in marshalling cannot leak into the command
// implementations.
//
// The native library is loaded with an explicit, absolute path resolved next to the managed
// assembly. That matters for security: the default DllImport search order would consider the
// current working directory, which would let a DLL planted in whatever directory the user
// happens to be in be loaded instead of ours.
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace ConsoleBgTool.Interop;

/// <summary>Mirrors the native <c>cbt_status</c> enum. Zero is success.</summary>
public enum CbtStatus
{
    Ok = 0,
    InvalidArgument = 1,
    OutOfRange = 2,
    Parse = 3,
    NotFound = 4,
    Access = 5,
    Io = 6,
    Unsupported = 7,
    Internal = 8,
    BufferTooSmall = 9,
}

/// <summary>Mirrors the native <c>cbt_theme_kind</c> enum.</summary>
public enum CbtThemeKind
{
    LightOnDark = 0,
    DarkOnLight = 1,
}

/// <summary>Mirrors the native <c>HostKind</c> enum from the platform layer.</summary>
public enum HostKind
{
    None = 0,
    ClassicConhost = 1,
    Pseudoconsole = 2,
    LegacyConhost = 3,
}

/// <summary>
/// The native <c>cbt_config</c> structure.
///
/// <para>
/// The layout is asserted against the native header by the test suite, so any drift between
/// this declaration and the C definition fails the build rather than silently mis-marshalling
/// a colour or a size.
/// </para>
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, CharSet = CharSet.Ansi)]
public struct NativeConfig
{
    public const int ColorTableSize = 16;
    public const int MaxFaceName = 32;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = ColorTableSize)]
    public uint[] Palette;

    public int ForegroundSlot;
    public int BackgroundSlot;
    public int CanvasSlot;

    public ushort BufferColumns;
    public ushort BufferRows;
    public ushort WindowColumns;
    public ushort WindowRows;

    public ushort FontSize;
    public ushort FontWeight;
    public uint FontFamily;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = MaxFaceName)]
    public string FaceName;

    public int OpacityPercent;
}

/// <summary>Mirrors the native <c>LiveConsoleState</c> structure.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct NativeLiveState
{
    [MarshalAs(UnmanagedType.I1)] public bool Valid;
    [MarshalAs(UnmanagedType.I1)] public bool IsConsole;
    [MarshalAs(UnmanagedType.I1)] public bool HasWindow;
    [MarshalAs(UnmanagedType.I1)] public bool V2;

    public ushort Attributes;
    public ushort PopupAttributes;
    public ushort BufferColumns;
    public ushort BufferRows;
    public ushort WindowColumns;
    public ushort WindowRows;
    public ushort FontWidth;
    public ushort FontHeight;
    public uint WindowPixelWidth;
    public uint WindowPixelHeight;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = NativeConfig.ColorTableSize)]
    public uint[] Palette;
}

/// <summary>
/// Safe wrapper over the native core and platform layers.
///
/// <para>
/// All failure paths surface the native diagnostic string, because a bare error code is not
/// actionable when the user is looking at a console that did not change colour.
/// </para>
/// </summary>
public static class NativeMethods
{
    private const string CoreLibrary = "cbt_core";
    private const string PlatformLibrary = "cbt_platform";

    static NativeMethods()
    {
        NativeLibrary.SetDllImportResolver(typeof(NativeMethods).Assembly, ResolveLibrary);
    }

    /// <summary>
    /// Resolves the native libraries from the directory that contains this assembly, and
    /// nowhere else. No environment variable, no working directory, no PATH.
    ///
    /// <para>
    /// Returning <see cref="IntPtr.Zero"/> lets the runtime report a normal
    /// <c>DllNotFoundException</c> naming the library, which is what a user who copied only the
    /// managed executable needs to see. Deliberately no fallback is offered: a search that
    /// includes the working directory or PATH is how a DLL planted next to a document gets loaded.
    /// </para>
    /// </summary>
    private static IntPtr ResolveLibrary(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != CoreLibrary && libraryName != PlatformLibrary)
        {
            return IntPtr.Zero;
        }

        string fileName = libraryName + ".dll";
        string candidate = Path.Combine(AppContext.BaseDirectory, fileName);
        if (File.Exists(candidate))
        {
            return NativeLibrary.Load(candidate);
        }

        return IntPtr.Zero;
    }

    internal static string LastError(Func<IntPtr> getError)
    {
        IntPtr pointer = getError();
        if (pointer == IntPtr.Zero)
        {
            return string.Empty;
        }
        return Marshal.PtrToStringUTF8(pointer) ?? string.Empty;
    }

    // ---- platform layer ---------------------------------------------------

    [DllImport(PlatformLibrary, EntryPoint = "cbt_platform_apply", CallingConvention = CallingConvention.Cdecl)]
    private static extern int PlatformApplyNative(ref NativeConfig config, int applyGeometry, int applyOpacity);

    [DllImport(PlatformLibrary, EntryPoint = "cbt_platform_restore", CallingConvention = CallingConvention.Cdecl)]
    private static extern int PlatformRestoreNative();

    [DllImport(PlatformLibrary, EntryPoint = "cbt_platform_read", CallingConvention = CallingConvention.Cdecl)]
    private static extern int PlatformReadNative(out NativeConfig config, out NativeLiveState state);

    [DllImport(PlatformLibrary, EntryPoint = "cbt_platform_last_error", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr PlatformLastErrorNative();

    [DllImport(PlatformLibrary, EntryPoint = "cbt_platform_host_kind", CallingConvention = CallingConvention.Cdecl)]
    private static extern int PlatformHostKindNative();

    [DllImport(PlatformLibrary, EntryPoint = "cbt_platform_is_windows_terminal", CallingConvention = CallingConvention.Cdecl)]
    private static extern int PlatformIsWindowsTerminalNative();

    // ---- core model -------------------------------------------------------

    [DllImport(CoreLibrary, EntryPoint = "cbt_color_parse", CallingConvention = CallingConvention.Cdecl)]
    private static extern int ColorParseNative(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text, out uint color);

    [DllImport(CoreLibrary, EntryPoint = "cbt_color_from_colorref", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint ColorFromColorRefNative(uint colorRef);

    [DllImport(CoreLibrary, EntryPoint = "cbt_color_to_colorref", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint ColorToColorRefNative(uint color);

    [DllImport(CoreLibrary, EntryPoint = "cbt_config_default", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ConfigDefaultNative(ref NativeConfig config);

    [DllImport(CoreLibrary, EntryPoint = "cbt_config_validate", CallingConvention = CallingConvention.Cdecl)]
    private static extern int ConfigValidateNative(ref NativeConfig config, out IntPtr field);

    [DllImport(CoreLibrary, EntryPoint = "cbt_config_contrast", CallingConvention = CallingConvention.Cdecl)]
    private static extern int ConfigContrastNative(ref NativeConfig config, out double ratio);

    [DllImport(CoreLibrary, EntryPoint = "cbt_opacity_percent_to_byte", CallingConvention = CallingConvention.Cdecl)]
    private static extern int OpacityToByteNative(int percent, out byte value);

    [DllImport(CoreLibrary, EntryPoint = "cbt_pack_coord", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint PackCoordNative(ushort x, ushort y);

    [DllImport(CoreLibrary, EntryPoint = "cbt_unpack_coord", CallingConvention = CallingConvention.Cdecl)]
    private static extern void UnpackCoordNative(uint packed, out ushort x, out ushort y);

    // ---- managed surface --------------------------------------------------

    public static CbtStatus Apply(NativeConfig config, bool applyGeometry, bool applyOpacity)
    {
        CbtStatus status = (CbtStatus)PlatformApplyNative(ref config, applyGeometry ? 1 : 0, applyOpacity ? 1 : 0);
        return status;
    }

    public static CbtStatus Restore() => (CbtStatus)PlatformRestoreNative();

    public static CbtStatus Read(out NativeConfig config, out NativeLiveState state) =>
        (CbtStatus)PlatformReadNative(out config, out state);

    public static NativeConfig DefaultConfig()
    {
        NativeConfig config = new()
        {
            Palette = new uint[NativeConfig.ColorTableSize],
            FaceName = string.Empty,
        };
        ConfigDefaultNative(ref config);
        return config;
    }

    public static CbtStatus Validate(ref NativeConfig config, out string field)
    {
        CbtStatus status = (CbtStatus)ConfigValidateNative(ref config, out IntPtr fieldPointer);
        field = fieldPointer == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(fieldPointer) ?? string.Empty;
        return status;
    }

    public static CbtStatus Contrast(ref NativeConfig config, out double ratio) =>
        (CbtStatus)ConfigContrastNative(ref config, out ratio);

    public static CbtStatus ParseColor(string text, out uint color) =>
        (CbtStatus)ColorParseNative(text, out color);

    public static uint ColorFromColorRef(uint value) => ColorFromColorRefNative(value);

    public static uint ColorToColorRef(uint value) => ColorToColorRefNative(value);

    public static CbtStatus OpacityToByte(int percent, out byte value) =>
        (CbtStatus)OpacityToByteNative(percent, out value);

    public static uint PackCoord(ushort x, ushort y) => PackCoordNative(x, y);

    public static void UnpackCoord(uint packed, out ushort x, out ushort y) => UnpackCoordNative(packed, out x, out y);

    public static HostKind HostKind => (HostKind)PlatformHostKindNative();

    public static bool IsWindowsTerminalHost => PlatformIsWindowsTerminalNative() != 0;

    public static string PlatformLastError => LastError(() => PlatformLastErrorNative());

    /// <summary>Composes a message from a status code and the native diagnostic.</summary>
    public static string Describe(CbtStatus status, string context)
    {
        string detail = PlatformLastError;
        var builder = new StringBuilder();
        builder.Append(context);
        builder.Append(" failed: ");
        builder.Append(status);
        if (!string.IsNullOrEmpty(detail))
        {
            builder.Append(" - ");
            builder.Append(detail);
        }
        return builder.ToString();
    }
}
