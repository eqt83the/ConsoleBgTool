/*
 * ConsoleBgTool - C++ platform layer
 * cbt_platform.hpp - live console styling for the console this process is attached to.
 *
 * Layering
 * --------
 *   cbt_console.c      pure model: colour, encoding, validation        (portable C)
 *   cbt_registry*.c    registry access behind a swappable interface    (C)
 *   cbt_platform.cpp   Win32: read/write a live console, window opacity (C++)
 *   ConsoleBgTool.Cli  user interface and Windows Terminal settings     (C#)
 *
 * The C++ layer contains every handle that can leak and every Win32 call that can fail, so
 * it is built from RAII wrappers and scope guards rather than matched close calls. Nothing
 * here injects code into, hooks, or attaches to another process: all console work is done
 * on the console this process is already attached to, which is why a bug in this layer
 * cannot destabilise cmd.exe or powershell.exe.
 */
#ifndef CBT_PLATFORM_HPP
#define CBT_PLATFORM_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "cbt_console.h"

#if defined(_WIN32)
#  define CBT_EXPORT extern "C" __declspec(dllexport)
#else
#  define CBT_EXPORT extern "C"
#endif

/*
 * Live console state.
 *
 * Deliberately at global scope rather than inside namespace cbt: the C ABI at the bottom of this
 * header passes this structure across the C# boundary, and a namespaced type has no place in a
 * C signature. Its layout is asserted against the managed struct by the test suite.
 *
 * All sizes and coordinates are unsigned because a console size is never negative; the Win32
 * structures use SHORT, so the conversion is done through the checked helper below rather than
 * by a bare assignment.
 */
struct LiveConsoleState {
    bool     valid = false;
    bool     is_console = false;   /* the process is attached to a console */
    bool     has_window = false;   /* the console has a visible window (not a pseudoconsole) */
    bool     v2 = true;            /* the "new" console host, rather than the legacy one */
    uint16_t attributes = 0;
    uint16_t popup_attributes = 0;
    uint16_t buffer_columns = 0;
    uint16_t buffer_rows = 0;
    uint16_t window_columns = 0;
    uint16_t window_rows = 0;
    uint16_t font_width = 0;
    uint16_t font_height = 0;
    uint32_t window_pixel_width = 0;
    uint32_t window_pixel_height = 0;
    cbt_color palette[CBT_COLOR_TABLE_SIZE] = {};
};

namespace cbt {

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Last error for the calling thread. Every API in this layer records a human readable
 * message here on failure, including the Win32 error code where one applies.
 */
const std::string &last_error() noexcept;
void set_last_error(const std::string &message) noexcept;
void clear_last_error() noexcept;

/* ------------------------------------------------------------------------- */
/* Live console state                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Console ownership: opens CONOUT$ for the duration of the scope and closes it again.
 * Deliberately not a singleton, so a failed open cannot leave a stale handle behind.
 */
class ConsoleHandle {
public:
    ConsoleHandle() noexcept;
    ~ConsoleHandle() noexcept;

    ConsoleHandle(const ConsoleHandle &) = delete;
    ConsoleHandle &operator=(const ConsoleHandle &) = delete;
    ConsoleHandle(ConsoleHandle &&other) noexcept;
    ConsoleHandle &operator=(ConsoleHandle &&other) noexcept;

    /* Returns false (and records last_error) when the process has no console. */
    bool open(bool write_access) noexcept;
    bool is_open() const noexcept { return handle_ != nullptr; }
    void close() noexcept;

    void *raw() const noexcept { return handle_; }
    /* Non-owning: for passing to the C core's registry-style helpers. */
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    void *handle_;
};

/*
 * RAII guard that restores a saved console state on scope exit.
 *
 * The console documentation is explicit that an application changing screen buffer
 * properties "should either create their own screen buffer or save the state of the
 * inherited screen buffer during startup and restore it at exit", because other programs
 * share the console. This class is that requirement expressed as a type.
 *
 * restore() is idempotent and its result is reported, so a caller can distinguish "restored
 * cleanly" from "restore failed" instead of assuming success.
 *
 * MEASURED HOST BEHAVIOUR: on Windows 11 24H2 (classic conhost and the ConPTY relay used by
 * Windows Terminal), SetConsoleScreenBufferInfoEx applies srWindow as if Bottom were one past
 * the last visible row - i.e. one row shorter than the rectangle returned by
 * GetConsoleScreenBufferInfoEx. restore() writes the colour table through that API, so a naive
 * restore shrank the window by one row every time. Geometry-preserving application now lives in
 * one helper in the implementation, used by both apply and restore, so a restore cannot resize
 * the console either.
 *
 * PERSISTENCE: applying styling is normally meant to persist for the rest of the session, which
 * is why cbt_platform_apply() disarms the guard. disarm() only stops the destructor from
 * restoring; restore() itself still works when disarmed, so an explicit restore command always
 * puts the original appearance back.
 */
class ConsoleStateGuard {
public:
    ConsoleStateGuard() noexcept = default;
    ~ConsoleStateGuard() noexcept;

    ConsoleStateGuard(const ConsoleStateGuard &) = delete;
    ConsoleStateGuard &operator=(const ConsoleStateGuard &) = delete;

    /* Capture the current state. Returns false when there is no console to guard. */
    bool capture() noexcept;

    /* Put the captured state back. Safe to call more than once. */
    bool restore() noexcept;

    bool captured() const noexcept { return captured_; }
    const LiveConsoleState &state() const noexcept { return state_; }

    /*
     * Disarm the guard: restore() becomes a no-op. Used when the caller deliberately wants
     * the new appearance to persist for the remainder of the session, which is the normal
     * case for this tool. Restoring is then the job of an explicit "restore" command.
     */
    void disarm() noexcept { armed_ = false; }
    bool armed() const noexcept { return armed_; }

private:
    LiveConsoleState state_{};
    bool captured_ = false;
    bool armed_ = false;
};

/* Read the live console. Returns false when there is no usable console. */
bool read_live_state(LiveConsoleState &out) noexcept;

/*
 * Apply palette, default attribute and geometry to the console this process is attached to.
 *
 * Only the fields listed in `what` are touched. Returns false on the first failure; the
 * caller is expected to hold a ConsoleStateGuard so a partial application is undone.
 *
 * Geometry is preserved exactly when it is not being changed: on this host a bare
 * SetConsoleScreenBufferInfoEx trims one row off the window on every call (see
 * ConsoleStateGuard above), which is why a palette-only apply used to shrink the console a row
 * at a time under the deployed cmd AutoRun. The implementation compensates and verifies the
 * geometry, and when the caller asks for a specific buffer/window size that size is reached
 * exactly or the call fails with a precise diagnostic.
 */
struct ApplyOptions {
    bool apply_palette = true;
    bool apply_attribute = true;
    bool apply_canvas = false; /* set slot 0 as the window fill colour */
    bool apply_geometry = false;
    uint16_t buffer_columns = 0;
    uint16_t buffer_rows = 0;
    uint16_t window_columns = 0;
    uint16_t window_rows = 0;
};

bool apply_live_state(const cbt_config &config, const ApplyOptions &options) noexcept;

/* ------------------------------------------------------------------------- */
/* Window opacity                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Opacity for the console window.
 *
 * IMPORTANT - support status
 * --------------------------
 * Per-window opacity for a classic console window is NOT an official console setting.
 * Windows Terminal exposes opacity through its documented `opacity` / `useAcrylic` profile
 * settings; the classic conhost window has no equivalent documented interface. This tool
 * therefore offers opacity as an explicit, opt-in, fully reversible window-style change,
 * and records the original window style so it can be put back exactly.
 *
 * Measured behaviour on Windows 11 24H2 (build 26100): applying WS_EX_LAYERED to a conhost
 * window succeeded and the window genuinely re-composited at the given alpha (an A/B screen
 * sample of the same window at alpha 255 vs 140 changed 16.3% of sampled pixels), while the
 * registry value HKCU\Console : WindowAlpha alone changed nothing.
 */
class WindowOpacity {
public:
    WindowOpacity() noexcept;
    ~WindowOpacity() noexcept;

    WindowOpacity(const WindowOpacity &) = delete;
    WindowOpacity &operator=(const WindowOpacity &) = delete;

    /* Returns false when the console has no window (for example a pseudoconsole). */
    bool open() noexcept;

    /* True when the window is actually layered right now. */
    bool is_layered() const noexcept;

    /*
     * Apply an opacity percentage (CBT_MIN_OPACITY..100). 100 removes the layered style and
     * returns the window to fully opaque, restoring the original extended style.
     */
    bool apply_percent(int percent) noexcept;

    /* Put the original extended style and alpha back. Idempotent. */
    bool restore() noexcept;

    bool has_window() const noexcept { return GetConsoleWindowSafe() != nullptr; }

    /// <summary>True once a baseline has been captured and can be restored to.</summary>
    bool opened() const noexcept { return captured_; }

private:
    /*
     * The console may destroy and recreate its window - a resize is one documented way it does -
     * so the handle is re-queried on every operation rather than cached. is_layered() and
     * restore() therefore always act on a live window.
     */
    static void *GetConsoleWindowSafe() noexcept;
    bool refresh_original_style() noexcept;

    void   *window_;
    int32_t original_ex_style_;
    bool    captured_;
    bool    modified_;
};

/* ------------------------------------------------------------------------- */
/* Host detection                                                            */
/* ------------------------------------------------------------------------- */

enum class HostKind {
    None = 0,     /* no console at all: output is redirected or the process is a GUI app */
    ClassicConhost, /* the classic console window (ConsoleWindowClass) */
    Pseudoconsole,  /* a pseudoconsole: Windows Terminal, ConEmu, VS debug host, ... */
    LegacyConhost   /* the legacy console host (ForceV2 = 0) */
};

const char *host_kind_name(HostKind kind) noexcept;

/*
 * Classify the host. `terminal_kind` is filled with the owning process name when the console
 * is a pseudoconsole, for example "WindowsTerminal.exe", otherwise it is empty.
 */
HostKind detect_host(std::string *terminal_kind) noexcept;

/*
 * True when the console window class is Windows Terminal's, which means the profile settings
 * written by the C# layer are what govern the appearance.
 */
bool is_windows_terminal_host() noexcept;

/* ------------------------------------------------------------------------- */
/* Registry journal paths (delegated to the C core)                          */
/* ------------------------------------------------------------------------- */

/*
 * The registry keys this tool considers part of "the console environment", in the order a
 * snapshot should record them. Confined to HKEY_CURRENT_USER by the backend.
 */
std::vector<std::string> console_registry_keys();

} /* namespace cbt */

/* ------------------------------------------------------------------------- */
/* C ABI, so the C# layer can P/Invoke without C++ name mangling             */
/* ------------------------------------------------------------------------- */

/*
 * Applies a configuration to the calling process's console. `argv`-free and string based so
 * it is trivial to call from .NET.
 *
 * Returns CBT_OK on success. On failure cbt_platform_last_error() describes why.
 *
 * On success the styling PERSISTS after this process exits: the process-wide state guard is
 * disarmed so its destructor cannot undo the palette. That is the tool's purpose - it styles
 * the console window for the commands that run in it, and the deployed cmd AutoRun relies on
 * the apply surviving the transient apply process. Use cbt_platform_restore() to put the
 * original appearance back explicitly.
 */
CBT_EXPORT int cbt_platform_apply(const cbt_config *config, int apply_geometry, int apply_opacity);

/*
 * Restores the console state captured by the most recent cbt_platform_apply.
 *
 * Works whether or not the guard was disarmed by a successful apply; only an explicit call
 * does this, the process exit never does it for a successful apply.
 */
CBT_EXPORT int cbt_platform_restore(void);

/* Reads the live console into *out. Returns CBT_OK, or CBT_E_UNSUPPORTED with no console. */
CBT_EXPORT int cbt_platform_read(cbt_config *out, LiveConsoleState *out_state);

/* UTF-8 description of the last platform-layer failure. Never NULL. */
CBT_EXPORT const char *cbt_platform_last_error(void);

/* Host classification for the C# layer; see HostKind. */
CBT_EXPORT int cbt_platform_host_kind(void);

/* Whether the current host renders Windows Terminal profile settings. */
CBT_EXPORT int cbt_platform_is_windows_terminal(void);

#endif /* CBT_PLATFORM_HPP */
