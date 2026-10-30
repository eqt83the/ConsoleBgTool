/*
 * ConsoleBgTool - C++ platform layer
 * cbt_platform.cpp - live console styling and window opacity, Win32 implementation.
 *
 * Design rules enforced here
 * --------------------------
 * 1. Every Win32 handle is owned by an RAII type. There is no bare CloseHandle on an error
 *    path, so a failure cannot leak a console handle or a temp DC.
 * 2. Any change to the shared console is made under a ConsoleStateGuard, because the console
 *    documentation requires a program to save and restore screen buffer state for the sake of
 *    the other programs sharing that console.
 * 3. No process injection, no API hooking, no AppInit_DLLs, no AttachConsole to a foreign
 *    console. All work happens on this process's own console, which is what keeps a defect
 *    in this layer from being able to disturb cmd.exe or powershell.exe.
 * 4. Window opacity is opt-in and its original style is captured before modification.
 * 5. Recolouring a console must never change its geometry. SetConsoleScreenBufferInfoEx is
 *    used only through set_screen_buffer_info_keeping_geometry(), because on this host it
 *    applies srWindow one row shorter than requested (measured; see that function). The same
 *    helper is used by ConsoleStateGuard::restore(), so putting the palette back cannot resize
 *    the window either.
 * 6. Applying styling is persistent by design: cbt_platform_apply() disarms the process-wide
 *    guard on success so the palette survives the process exit. Only an explicit
 *    cbt_platform_restore() puts the original appearance back.
 */
#include "cbt_platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cbt_registry.h"

namespace cbt {

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------- */

namespace {

thread_local std::string g_last_error;

std::string win32_message(const char *what, DWORD error)
{
    std::string message(what == nullptr ? "error" : what);
    message += " (Win32 error ";
    message += std::to_string(static_cast<unsigned long>(error));
    message += ")";

    LPWSTR buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                      FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                  reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length != 0 && buffer != nullptr) {
        /* Trim the trailing CR/LF that FormatMessage always appends. */
        while (length > 0 && (buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n' ||
                              buffer[length - 1] == L' ' || buffer[length - 1] == L'.')) {
            buffer[--length] = L'\0';
        }
        int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            std::string text(static_cast<size_t>(needed - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, -1, text.data(), needed, nullptr, nullptr);
            message += ": ";
            message += text;
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

} /* namespace */

const std::string &last_error() noexcept
{
    return g_last_error;
}

void set_last_error(const std::string &message) noexcept
{
    try {
        g_last_error = message;
    } catch (...) {
        /* Never let diagnostics allocation failure escape a noexcept boundary. */
    }
}

void clear_last_error() noexcept
{
    g_last_error.clear();
}

namespace {

void fail(const std::string &message) noexcept
{
    set_last_error(message);
}

void fail_win32(const char *what) noexcept
{
    set_last_error(win32_message(what, GetLastError()));
}

} /* namespace */

/* ------------------------------------------------------------------------- */
/* ConsoleHandle                                                             */
/* ------------------------------------------------------------------------- */

ConsoleHandle::ConsoleHandle() noexcept : handle_(nullptr) {}

ConsoleHandle::~ConsoleHandle() noexcept
{
    close();
}

ConsoleHandle::ConsoleHandle(ConsoleHandle &&other) noexcept : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

ConsoleHandle &ConsoleHandle::operator=(ConsoleHandle &&other) noexcept
{
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool ConsoleHandle::open(bool write_access) noexcept
{
    close();
    clear_last_error();

    DWORD access = GENERIC_READ | (write_access ? GENERIC_WRITE : 0);
    HANDLE handle = CreateFileW(L"CONOUT$", access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        /*
         * ERROR_INVALID_HANDLE / ERROR_FILE_NOT_FOUND here simply mean "this process has no
         * console" - a normal situation for a GUI host or a redirected pipeline - so it is
         * reported as unsupported rather than as a fault.
         */
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_HANDLE || error == ERROR_FILE_NOT_FOUND) {
            fail("no console is attached to this process");
        } else {
            fail_win32("CreateFile(CONOUT$) failed");
        }
        return false;
    }
    handle_ = handle;
    return true;
}

void ConsoleHandle::close() noexcept
{
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

/* ------------------------------------------------------------------------- */
/* Console state translation                                                 */
/* ------------------------------------------------------------------------- */

namespace {

/*
 * Checked narrowing.
 *
 * The console structures use SHORT for sizes and coordinates while this layer models them as
 * unsigned. A negative SHORT is meaningless here and would silently become a huge unsigned value,
 * so it is clamped to zero and reported through the false return rather than propagated.
 */
bool to_size(short value, uint16_t &out) noexcept
{
    if (value < 0) {
        return false;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

CONSOLE_SCREEN_BUFFER_INFOEX make_info_ex()
{
    CONSOLE_SCREEN_BUFFER_INFOEX info;
    std::memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    return info;
}

bool read_info_ex(HANDLE console, CONSOLE_SCREEN_BUFFER_INFOEX &out) noexcept
{
    out = make_info_ex();
    if (!GetConsoleScreenBufferInfoEx(console, &out)) {
        fail_win32("GetConsoleScreenBufferInfoEx failed");
        return false;
    }
    return true;
}

/* Inclusive, zero-based console rectangles: Right/Bottom are the last visible row/column. */
bool rect_fits(COORD size, SMALL_RECT rect) noexcept
{
    return size.X > 0 && size.Y > 0 && rect.Left >= 0 && rect.Top >= 0 &&
           rect.Right >= rect.Left && rect.Bottom >= rect.Top && rect.Right < size.X &&
           rect.Bottom < size.Y;
}

/* Clamp a rectangle into a buffer, keeping it non-empty. */
SMALL_RECT clamp_rect(SMALL_RECT rect, COORD size) noexcept
{
    SMALL_RECT out{};
    out.Left = rect.Left > 0 ? rect.Left : 0;
    out.Top = rect.Top > 0 ? rect.Top : 0;
    out.Right = rect.Right < static_cast<SHORT>(size.X - 1) ? rect.Right
                                                            : static_cast<SHORT>(size.X - 1);
    out.Bottom = rect.Bottom < static_cast<SHORT>(size.Y - 1)
                     ? rect.Bottom
                     : static_cast<SHORT>(size.Y - 1);
    if (out.Right < out.Left) {
        out.Right = out.Left;
    }
    if (out.Bottom < out.Top) {
        out.Bottom = out.Top;
    }
    return out;
}

bool same_geometry(const CONSOLE_SCREEN_BUFFER_INFOEX &info, COORD size,
                   SMALL_RECT window) noexcept
{
    return info.dwSize.X == size.X && info.dwSize.Y == size.Y &&
           info.srWindow.Left == window.Left && info.srWindow.Top == window.Top &&
           info.srWindow.Right == window.Right && info.srWindow.Bottom == window.Bottom;
}

std::string geometry_text(COORD size, SMALL_RECT window)
{
    std::string text = std::to_string(static_cast<int32_t>(size.X));
    text += "x";
    text += std::to_string(static_cast<int32_t>(size.Y));
    text += ", window ";
    text += std::to_string(static_cast<int32_t>(window.Left));
    text += "..";
    text += std::to_string(static_cast<int32_t>(window.Right));
    text += " x ";
    text += std::to_string(static_cast<int32_t>(window.Top));
    text += "..";
    text += std::to_string(static_cast<int32_t>(window.Bottom));
    return text;
}

/*
 * SetConsoleScreenBufferInfoEx without letting the host change the console geometry.
 *
 * MEASURED BEHAVIOUR (Windows 11 24H2, both the classic conhost and the ConPTY relay that
 * Windows Terminal uses): SetConsoleScreenBufferInfoEx interprets srWindow.Bottom as if it were
 * ONE PAST the last visible row - it applies rows [Top, Bottom - 1] - while
 * GetConsoleScreenBufferInfoEx reports Bottom as the inclusive last visible row. Passing back the
 * structure that was just read therefore trims one row off the window on every call. The deployed
 * cmd AutoRun applies the palette on every shell start, so the window used to shrink one row per
 * cmd start until the shell's line editor wrote over the typed line.
 *
 * The fix is to issue the same request compensated so the host lands on the intended rectangle:
 * pass Bottom + 1, i.e. one row past the intended last visible row. A valid window never has
 * Bottom beyond dwSize.Y - 1, so Bottom + 1 is at most dwSize.Y, and that value was measured to
 * be accepted by both the classic conhost and the ConPTY relay (it is exactly the "window sits on
 * the buffer's bottom edge" case of a normal cmd scrollback buffer). The host then applies
 * [Top, Bottom], which is the intended window. Top - 1 is kept only as a last-resort compensation
 * for a host that refuses Bottom + 1; the rectangle as written is the final fallback. The
 * compensation costs nothing on a host that applies the rectangle literally: the read-back then
 * differs from the intended geometry and the correction below puts it back.
 *
 * A second measured host quirk is that the classic conhost forces the window WIDTH to the buffer
 * width in this call (it only honours srWindow.Bottom, not srWindow.Right), so a deliberately
 * narrower window is put back by the correction below as well.
 *
 * After the call the geometry is read back and compared with the intended one. If the host still
 * moved anything, the intended geometry is re-asserted explicitly (SetConsoleWindowInfo for the
 * window, plus SetConsoleScreenBufferSize when the buffer changed too - the window first when the
 * buffer is shrinking, because the old window no longer fits the new buffer), and verified once
 * more. Returns false with a precise last_error only when the intended geometry cannot be
 * reached.
 *
 * Called by both apply_live_state() and ConsoleStateGuard::restore(), which must not resize the
 * console they are only supposed to recolour or put back.
 */
bool set_screen_buffer_info_keeping_geometry(HANDLE console,
                                             CONSOLE_SCREEN_BUFFER_INFOEX &info) noexcept
{
    const COORD intended_size = info.dwSize;
    const SMALL_RECT intended_window = info.srWindow;

    /*
     * Pre-flight: the console rejects a buffer change while the live window does not fit the new
     * buffer, so shrink the window into the corner shared by the current and intended buffers
     * first. This only does anything when the buffer is actually being made smaller.
     */
    CONSOLE_SCREEN_BUFFER_INFOEX current = make_info_ex();
    if (!GetConsoleScreenBufferInfoEx(console, &current)) {
        fail_win32("GetConsoleScreenBufferInfoEx failed before applying the buffer info");
        return false;
    }
    if (!rect_fits(intended_size, current.srWindow)) {
        COORD limit{};
        limit.X = current.dwSize.X < intended_size.X ? current.dwSize.X : intended_size.X;
        limit.Y = current.dwSize.Y < intended_size.Y ? current.dwSize.Y : intended_size.Y;
        SMALL_RECT interim = clamp_rect(current.srWindow, limit);
        if (!SetConsoleWindowInfo(console, TRUE, &interim)) {
            fail_win32("SetConsoleWindowInfo failed while fitting the window to the buffer");
            return false;
        }
    }

    /* Compensated request: see the measured behaviour above. */
    bool compensated = false;
    CONSOLE_SCREEN_BUFFER_INFOEX request = info;
    if (static_cast<int32_t>(request.srWindow.Bottom) + 1 <=
        static_cast<int32_t>(request.dwSize.Y)) {
        /*
         * Ask for one row past the intended last one. A valid window never has Bottom beyond
         * dwSize.Y - 1, so this stays in range (Bottom + 1 == dwSize.Y is the window sitting on
         * the buffer's bottom edge, which is the normal cmd scrollback case and was measured to
         * be accepted on both hosts).
         */
        request.srWindow.Bottom = static_cast<SHORT>(static_cast<int32_t>(request.srWindow.Bottom) + 1);
        compensated = true;
    } else if (request.srWindow.Top > 0) {
        request.srWindow.Top = static_cast<SHORT>(static_cast<int32_t>(request.srWindow.Top) - 1);
        compensated = true;
    }

    if (!SetConsoleScreenBufferInfoEx(console, &request)) {
        if (!compensated) {
            fail_win32("SetConsoleScreenBufferInfoEx failed");
            return false;
        }
        /*
         * A host may refuse the compensated rectangle. Retry with the rectangle exactly as the
         * caller wrote it; a host that accepts it literally lands on the intended geometry, and a
         * host that trims a row is repaired by the fallback below.
         */
        request = info;
        if (!SetConsoleScreenBufferInfoEx(console, &request)) {
            fail_win32("SetConsoleScreenBufferInfoEx failed");
            return false;
        }
    }

    CONSOLE_SCREEN_BUFFER_INFOEX after = make_info_ex();
    if (!GetConsoleScreenBufferInfoEx(console, &after)) {
        fail_win32("GetConsoleScreenBufferInfoEx failed after applying the buffer info");
        return false;
    }
    if (same_geometry(after, intended_size, intended_window)) {
        return true;
    }

    /* Fallback: the host did not land on the intended geometry - put it there explicitly. */
    const SMALL_RECT target = clamp_rect(intended_window, intended_size);
    const bool buffer_differs =
        after.dwSize.X != intended_size.X || after.dwSize.Y != intended_size.Y;
    const bool buffer_shrinks =
        static_cast<int32_t>(after.dwSize.X) > static_cast<int32_t>(intended_size.X) ||
        static_cast<int32_t>(after.dwSize.Y) > static_cast<int32_t>(intended_size.Y);
    if (buffer_differs && buffer_shrinks) {
        COORD limit{};
        limit.X = after.dwSize.X < intended_size.X ? after.dwSize.X : intended_size.X;
        limit.Y = after.dwSize.Y < intended_size.Y ? after.dwSize.Y : intended_size.Y;
        SMALL_RECT interim = clamp_rect(target, limit);
        if (!SetConsoleWindowInfo(console, TRUE, &interim)) {
            fail_win32("SetConsoleWindowInfo failed while preparing to shrink the buffer");
            return false;
        }
        if (!SetConsoleScreenBufferSize(console, intended_size)) {
            fail_win32("SetConsoleScreenBufferSize failed while restoring the buffer size");
            return false;
        }
    } else if (buffer_differs) {
        if (!SetConsoleScreenBufferSize(console, intended_size)) {
            fail_win32("SetConsoleScreenBufferSize failed while restoring the buffer size");
            return false;
        }
    }
    if (!SetConsoleWindowInfo(console, TRUE, &target)) {
        fail_win32("SetConsoleWindowInfo failed while restoring the window");
        return false;
    }

    if (!GetConsoleScreenBufferInfoEx(console, &after)) {
        fail_win32("GetConsoleScreenBufferInfoEx failed while verifying the restored geometry");
        return false;
    }
    if (!same_geometry(after, intended_size, intended_window)) {
        fail("the console would not keep the intended geometry: wanted " +
             geometry_text(intended_size, intended_window) + ", got " +
             geometry_text(after.dwSize, after.srWindow));
        return false;
    }
    return true;
}

void to_live_state(const CONSOLE_SCREEN_BUFFER_INFOEX &info, LiveConsoleState &out) noexcept
{
    out.valid = true;
    out.attributes = info.wAttributes;
    out.popup_attributes = info.wPopupAttributes;

    /* Clamp rather than propagate: a negative size is a broken console, not a large one. */
    (void)to_size(info.dwSize.X, out.buffer_columns);
    (void)to_size(info.dwSize.Y, out.buffer_rows);

    {
        /* Compute the window extent in wide signed arithmetic first, then narrow once. */
        int32_t columns = static_cast<int32_t>(info.srWindow.Right) -
                          static_cast<int32_t>(info.srWindow.Left) + 1;
        int32_t rows = static_cast<int32_t>(info.srWindow.Bottom) -
                       static_cast<int32_t>(info.srWindow.Top) + 1;
        out.window_columns = columns > 0 ? static_cast<uint16_t>(columns) : 0;
        out.window_rows = rows > 0 ? static_cast<uint16_t>(rows) : 0;
    }

    for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        out.palette[i] = cbt_color_from_colorref(info.ColorTable[i]);
    }
}

/*
 * ForceV2 is the one console setting the console API does not expose, so it is read from the
 * registry. Reading is safe: this never writes.
 */
bool read_force_v2() noexcept
{
    HKEY key = nullptr;
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LSTATUS status;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Console", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true; /* absent means the modern host */
    }
    status = RegQueryValueExW(key, L"ForceV2", nullptr, &type,
                              reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) {
        return true;
    }
    return value != 0;
}

} /* namespace */

/* ------------------------------------------------------------------------- */
/* ConsoleStateGuard                                                         */
/* ------------------------------------------------------------------------- */

ConsoleStateGuard::~ConsoleStateGuard() noexcept
{
    if (captured_ && armed_) {
        restore();
    }
}

bool ConsoleStateGuard::capture() noexcept
{
    clear_last_error();

    ConsoleHandle console;
    if (!console.open(true)) {
        return false;
    }

    CONSOLE_SCREEN_BUFFER_INFOEX info = make_info_ex();
    if (!read_info_ex(static_cast<HANDLE>(console.raw()), info)) {
        return false;
    }

    to_live_state(info, state_);

    CONSOLE_FONT_INFOEX font;
    std::memset(&font, 0, sizeof(font));
    font.cbSize = sizeof(font);
    if (GetCurrentConsoleFontEx(static_cast<HANDLE>(console.raw()), FALSE, &font)) {
        (void)to_size(font.dwFontSize.X, state_.font_width);
        (void)to_size(font.dwFontSize.Y, state_.font_height);
    }

    state_.v2 = read_force_v2();

    captured_ = true;
    armed_ = true;
    return true;
}

bool ConsoleStateGuard::restore() noexcept
{
    clear_last_error();

    if (!captured_) {
        return true; /* nothing to undo: restoring an uncaptured guard is a no-op */
    }

    ConsoleHandle console;
    if (!console.open(true)) {
        return false;
    }
    HANDLE handle = static_cast<HANDLE>(console.raw());

    CONSOLE_SCREEN_BUFFER_INFOEX info = make_info_ex();
    if (!read_info_ex(handle, info)) {
        return false;
    }

    /* The geometry to put back, clamped into the buffer it has to live in. */
    COORD buffer_size{};
    bool buffer_valid = false;
    if (state_.buffer_columns != 0 && state_.buffer_rows != 0) {
        buffer_size.X = static_cast<SHORT>(state_.buffer_columns);
        buffer_size.Y = static_cast<SHORT>(state_.buffer_rows);
        buffer_valid = true;
    }

    SMALL_RECT window_rect{};
    bool window_valid = false;
    if (state_.window_columns != 0 && state_.window_rows != 0) {
        /* Clamp: a window bigger than its buffer is rejected by the console. */
        uint16_t columns = buffer_valid ? std::min<uint16_t>(state_.window_columns,
                                                            state_.buffer_columns)
                                        : state_.window_columns;
        uint16_t rows = buffer_valid ? std::min<uint16_t>(state_.window_rows, state_.buffer_rows)
                                     : state_.window_rows;
        window_rect.Left = 0;
        window_rect.Top = 0;
        window_rect.Right = static_cast<SHORT>(static_cast<int32_t>(columns) - 1);
        window_rect.Bottom = static_cast<SHORT>(static_cast<int32_t>(rows) - 1);
        window_valid = true;
    }

    /*
     * Geometry first, in the order the console accepts. The buffer must never be smaller than
     * the window: shrink the window first when the buffer is being made smaller, otherwise the
     * set-buffer call is refused because the old window no longer fits.
     */
    if (buffer_valid && (info.dwSize.X != buffer_size.X || info.dwSize.Y != buffer_size.Y)) {
        const bool shrinking =
            static_cast<int32_t>(buffer_size.X) < static_cast<int32_t>(info.dwSize.X) ||
            static_cast<int32_t>(buffer_size.Y) < static_cast<int32_t>(info.dwSize.Y);
        if (shrinking && window_valid) {
            COORD limit{};
            limit.X = buffer_size.X < info.dwSize.X ? buffer_size.X : info.dwSize.X;
            limit.Y = buffer_size.Y < info.dwSize.Y ? buffer_size.Y : info.dwSize.Y;
            SMALL_RECT interim = clamp_rect(window_rect, limit);
            if (!SetConsoleWindowInfo(handle, TRUE, &interim)) {
                fail_win32("SetConsoleWindowInfo failed while preparing to restore the buffer");
                return false;
            }
        }
        if (!SetConsoleScreenBufferSize(handle, buffer_size)) {
            fail_win32("SetConsoleScreenBufferSize failed while restoring");
            return false;
        }
    }

    if (window_valid) {
        if (!SetConsoleWindowInfo(handle, TRUE, &window_rect)) {
            fail_win32("SetConsoleWindowInfo failed while restoring");
            return false;
        }
    }

    if (!read_info_ex(handle, info)) {
        return false;
    }
    for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        info.ColorTable[i] = cbt_color_to_colorref(state_.palette[i]);
    }
    info.wAttributes = state_.attributes;
    info.wPopupAttributes = state_.popup_attributes;
    /*
     * The colour table is written through SetConsoleScreenBufferInfoEx, which is also where the
     * host used to trim one row off the window on every restore. The helper keeps the geometry
     * the console reports here (already the intended one) exactly as it is.
     */
    if (!set_screen_buffer_info_keeping_geometry(handle, info)) {
        return false;
    }

    armed_ = false;
    return true;
}

bool read_live_state(LiveConsoleState &out) noexcept
{
    clear_last_error();
    out = LiveConsoleState{};

    ConsoleHandle console;
    if (!console.open(false)) {
        return false;
    }
    HANDLE handle = static_cast<HANDLE>(console.raw());

    CONSOLE_SCREEN_BUFFER_INFOEX info = make_info_ex();
    if (!read_info_ex(handle, info)) {
        return false;
    }
    to_live_state(info, out);

    CONSOLE_FONT_INFOEX font;
    std::memset(&font, 0, sizeof(font));
    font.cbSize = sizeof(font);
    if (GetCurrentConsoleFontEx(handle, FALSE, &font)) {
        (void)to_size(font.dwFontSize.X, out.font_width);
        (void)to_size(font.dwFontSize.Y, out.font_height);
    }

    HWND window = GetConsoleWindow();
    if (window != nullptr) {
        RECT rect;
        if (GetWindowRect(window, &rect)) {
            /* Window extents are signed LONGs; a negative extent is clamped to zero. */
            int64_t width = static_cast<int64_t>(rect.right) - static_cast<int64_t>(rect.left);
            int64_t height = static_cast<int64_t>(rect.bottom) - static_cast<int64_t>(rect.top);
            out.window_pixel_width = width > 0 ? static_cast<uint32_t>(width) : 0;
            out.window_pixel_height = height > 0 ? static_cast<uint32_t>(height) : 0;
        }
    }

    out.is_console = true;
    out.has_window = window != nullptr;
    out.v2 = read_force_v2();
    return true;
}

/* ------------------------------------------------------------------------- */
/* Apply                                                                     */
/* ------------------------------------------------------------------------- */

bool apply_live_state(const cbt_config &config, const ApplyOptions &options) noexcept
{
    clear_last_error();

    const char *field = nullptr;
    cbt_status status = cbt_config_validate(&config, &field);
    if (status != CBT_OK) {
        std::string message = "refusing to apply an invalid configuration: ";
        message += cbt_status_name(status);
        if (field != nullptr) {
            message += " in field '";
            message += field;
            message += "'";
        }
        fail(message);
        return false;
    }

    ConsoleHandle console;
    if (!console.open(true)) {
        return false;
    }
    HANDLE handle = static_cast<HANDLE>(console.raw());

    CONSOLE_SCREEN_BUFFER_INFOEX info = make_info_ex();
    if (!read_info_ex(handle, info)) {
        return false;
    }

    /* --- geometry ------------------------------------------------------- */
    if (options.apply_geometry && options.buffer_columns != 0 && options.buffer_rows != 0) {
        SMALL_RECT current = info.srWindow;
        uint16_t current_window_columns = 0;
        uint16_t current_window_rows = 0;

        /* Widths are computed in wide signed arithmetic, then narrowed once through to_size. */
        {
            int32_t columns = static_cast<int32_t>(current.Right) -
                              static_cast<int32_t>(current.Left) + 1;
            int32_t rows = static_cast<int32_t>(current.Bottom) -
                           static_cast<int32_t>(current.Top) + 1;
            if (columns > 0) {
                (void)to_size(static_cast<short>(columns), current_window_columns);
            }
            if (rows > 0) {
                (void)to_size(static_cast<short>(rows), current_window_rows);
            }
        }

        uint16_t target_window_columns = options.window_columns != 0 ? options.window_columns
                                                                    : current_window_columns;
        uint16_t target_window_rows =
            options.window_rows != 0 ? options.window_rows : current_window_rows;

        /* Never let the window exceed the buffer: the console rejects that combination. */
        target_window_columns = std::min<uint16_t>(target_window_columns, options.buffer_columns);
        target_window_rows = std::min<uint16_t>(target_window_rows, options.buffer_rows);

        COORD buffer_size{static_cast<SHORT>(options.buffer_columns),
                          static_cast<SHORT>(options.buffer_rows)};

        /*
         * Shrink the window first when either dimension is being reduced, otherwise the
         * buffer shrink would be refused because the window no longer fits inside it.
         */
        if (target_window_columns < current_window_columns ||
            target_window_rows < current_window_rows) {
            SMALL_RECT shrink{current.Left, current.Top,
                              static_cast<SHORT>(static_cast<int32_t>(current.Left) +
                                                 static_cast<int32_t>(target_window_columns) - 1),
                              static_cast<SHORT>(static_cast<int32_t>(current.Top) +
                                                 static_cast<int32_t>(target_window_rows) - 1)};
            if (!SetConsoleWindowInfo(handle, TRUE, &shrink)) {
                fail_win32("SetConsoleWindowInfo failed while shrinking the window");
                return false;
            }
        }

        if (info.dwSize.X != buffer_size.X || info.dwSize.Y != buffer_size.Y) {
            if (!SetConsoleScreenBufferSize(handle, buffer_size)) {
                fail_win32("SetConsoleScreenBufferSize failed");
                return false;
            }
        }

        SMALL_RECT grow{0, 0,
                        static_cast<SHORT>(static_cast<int32_t>(target_window_columns) - 1),
                        static_cast<SHORT>(static_cast<int32_t>(target_window_rows) - 1)};
        if (!SetConsoleWindowInfo(handle, TRUE, &grow)) {
            fail_win32("SetConsoleWindowInfo failed while sizing the window");
            return false;
        }

        if (!read_info_ex(handle, info)) {
            return false;
        }
    }

    /* --- colour --------------------------------------------------------- */
    if (options.apply_palette) {
        for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
            info.ColorTable[i] = cbt_color_to_colorref(config.palette[i]);
        }
    }

    if (options.apply_attribute) {
        uint16_t attribute = 0;
        status = cbt_attribute_from_slots(config.foreground_slot, config.background_slot, &attribute);
        if (status != CBT_OK) {
            fail(std::string("invalid text attribute: ") + cbt_status_name(status));
            return false;
        }
        info.wAttributes = attribute;
    }

    if (options.apply_canvas) {
        /*
         * The window background is the colour table entry the "black on light grey" default
         * attribute selects, i.e. slot 0. Writing it both into the colour table and into the
         * default attribute is what makes the whole window take the new colour.
         */
        if (config.canvas_slot < 0 || config.canvas_slot >= CBT_COLOR_TABLE_SIZE) {
            fail("invalid canvas slot");
            return false;
        }
        if (!options.apply_palette) {
            info.ColorTable[config.canvas_slot] =
                cbt_color_to_colorref(config.palette[config.canvas_slot]);
        }
    }

    /*
     * Apply palette/attribute/geometry through the geometry-preserving helper: a plain call here
     * trims one row off the window on this host (see the measured behaviour documented above the
     * helper), which is the "output overlaps typed characters" corruption the cmd AutoRun caused
     * by applying the palette on every shell start. Geometry that was requested above must come
     * back exactly; a palette-only apply must leave it exactly unchanged.
     */
    if (!set_screen_buffer_info_keeping_geometry(handle, info)) {
        return false;
    }

    /*
     * Repaint. The console redraws on its own for the cells it knows about, but the newly
     * exposed area after a resize needs an explicit invalidate, and asking for a full redraw
     * makes the change visible immediately in a console that is idle at a prompt.
     */
    HWND window = GetConsoleWindow();
    if (window != nullptr) {
        InvalidateRect(window, nullptr, TRUE);
        UpdateWindow(window);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* WindowOpacity                                                             */
/* ------------------------------------------------------------------------- */

WindowOpacity::WindowOpacity() noexcept
    : window_(nullptr), original_ex_style_(0), captured_(false), modified_(false)
{
}

void *WindowOpacity::GetConsoleWindowSafe() noexcept
{
    return static_cast<void *>(GetConsoleWindow());
}

WindowOpacity::~WindowOpacity() noexcept
{
    if (modified_) {
        restore();
    }
}

bool WindowOpacity::open() noexcept
{
    clear_last_error();

    HWND window = GetConsoleWindow();
    if (window == nullptr) {
        /*
         * A pseudoconsole (Windows Terminal, ConEmu, an IDE debug host) gives the client no
         * window of its own, so there is nothing this process may legally restyle. Reported as
         * unsupported rather than attempted.
         */
        fail("this console has no window of its own (pseudoconsole host); "
             "window opacity is not available");
        return false;
    }

    window_ = window;
    captured_ = true;
    modified_ = false;
    refresh_original_style();
    return true;
}

/*
 * (Re-)establish the style to restore to, from the *current* window.
 *
 * The console is allowed to destroy and recreate its window - a resize is one case where it does -
 * which invalidates a handle captured earlier. Re-querying here is what keeps the "restore" path
 * pointing at a live window instead of a destroyed one.
 */
bool WindowOpacity::refresh_original_style() noexcept
{
    HWND window = GetConsoleWindow();
    if (window == nullptr) {
        fail("the console window disappeared");
        captured_ = false;
        return false;
    }

    SetLastError(0);
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (style == 0 && GetLastError() != 0) {
        fail_win32("GetWindowLongPtr(GWL_EXSTYLE) failed");
        captured_ = false;
        return false;
    }

    window_ = window;
    /*
     * The baseline is always an UNLAYERED window, so the captured style has WS_EX_LAYERED masked
     * off. Capturing it verbatim would be wrong whenever this object is created while the window
     * is already layered - a second apply inside the same console, for instance - because
     * "fully opaque" would then faithfully restore the layered bit and the window could never be
     * made opaque again.
     */
    original_ex_style_ = static_cast<int32_t>(style & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    return true;
}

bool WindowOpacity::is_layered() const noexcept
{
    HWND window = GetConsoleWindow();
    if (window == nullptr) {
        return false;
    }
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    return (style & WS_EX_LAYERED) != 0;
}

bool WindowOpacity::apply_percent(int percent) noexcept
{
    clear_last_error();

    if (!captured_) {
        fail("WindowOpacity::open() must succeed before apply_percent()");
        return false;
    }
    if (cbt_validate_opacity(percent) != CBT_OK) {
        fail("opacity percentage out of range");
        return false;
    }

    if (percent >= CBT_MAX_OPACITY) {
        /* Fully opaque: remove the layered style and put the original style back. */
        return restore();
    }

    uint8_t alpha = 0;
    if (cbt_opacity_percent_to_byte(percent, &alpha) != CBT_OK) {
        fail("opacity conversion failed");
        return false;
    }

    /* Re-read the window and its style: it may not be the window we captured earlier. */
    if (!refresh_original_style()) {
        return false;
    }

    HWND window = static_cast<HWND>(window_);
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(window, GWL_EXSTYLE,
                                          static_cast<LONG_PTR>(original_ex_style_) | WS_EX_LAYERED);
    if (previous == 0 && GetLastError() != 0) {
        fail_win32("SetWindowLongPtr(GWL_EXSTYLE) failed");
        return false;
    }

    if (!SetLayeredWindowAttributes(window, 0, alpha, LWA_ALPHA)) {
        /* Undo the style change so a failed alpha call cannot leave a broken window. */
        SetWindowLongPtrW(window, GWL_EXSTYLE, static_cast<LONG_PTR>(original_ex_style_));
        fail_win32("SetLayeredWindowAttributes failed");
        return false;
    }

    if (!SetWindowPos(window, nullptr, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                          SWP_FRAMECHANGED)) {
        fail_win32("SetWindowPos failed while applying the layered style");
        return false;
    }

    modified_ = true;
    return true;
}

bool WindowOpacity::restore() noexcept
{
    clear_last_error();

    if (!captured_ || !modified_) {
        modified_ = false;
        return true;
    }

    /*
     * Prefer the window that is live now. If the original one still exists, the style to restore
     * is the one captured from it; if the console replaced it, re-capturing gives the honest
     * "unstyled" baseline for the new window.
     */
    HWND window = GetConsoleWindow();
    if (window == nullptr) {
        modified_ = false;
        return true; /* nothing to restore to */
    }

    LONG_PTR target_style = (window == static_cast<HWND>(window_))
                                ? static_cast<LONG_PTR>(original_ex_style_)
                                : 0;

    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(window, GWL_EXSTYLE, target_style);
    if (previous == 0 && GetLastError() != 0) {
        fail_win32("SetWindowLongPtr(GWL_EXSTYLE) failed while restoring opacity");
        return false;
    }
    /* A stale alpha would resurface if the window were made layered again by someone else. */
    SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    modified_ = false;
    return !is_layered();
}

/* ------------------------------------------------------------------------- */
/* Host detection                                                            */
/* ------------------------------------------------------------------------- */

const char *host_kind_name(HostKind kind) noexcept
{
    switch (kind) {
    case HostKind::None:           return "none";
    case HostKind::ClassicConhost: return "classic-conhost";
    case HostKind::Pseudoconsole:  return "pseudoconsole";
    case HostKind::LegacyConhost:  return "legacy-conhost";
    default:                       return "unknown";
    }
}

namespace {

/* Class name of the window a classic conhost creates for itself. */
constexpr wchar_t kConhostWindowClass[] = L"ConsoleWindowClass";
/* Windows Terminal's top-level window class, as observed on Windows 11 24H2. */
constexpr wchar_t kTerminalWindowClass[] = L"CASCADIA_HOSTING_WINDOW_CLASS";

bool process_name_of_window(HWND window, std::string &out_name) noexcept
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }

    wchar_t path[MAX_PATH];
    DWORD size = static_cast<DWORD>(std::size(path));
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (!ok) {
        return false;
    }

    const wchar_t *base = wcsrchr(path, L'\\');
    base = base == nullptr ? path : base + 1;

    int needed = WideCharToMultiByte(CP_UTF8, 0, base, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return false;
    }
    out_name.assign(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, base, -1, out_name.data(), needed, nullptr, nullptr);
    return true;
}

bool force_v2_enabled() noexcept
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Console", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true; /* absent means the modern host */
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExW(key, L"ForceV2", nullptr, &type,
                                      reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) {
        return true;
    }
    return value != 0;
}

} /* namespace */

HostKind detect_host(std::string *terminal_kind) noexcept
{
    if (terminal_kind != nullptr) {
        terminal_kind->clear();
    }

    HWND window = GetConsoleWindow();
    if (window == nullptr) {
        /*
         * No window: either there is no console at all, or the console is a pseudoconsole.
         * Distinguish the two by asking for the console mode.
         */
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (output == nullptr || output == INVALID_HANDLE_VALUE ||
            !GetConsoleMode(output, &mode)) {
            return HostKind::None;
        }
        if (terminal_kind != nullptr) {
            *terminal_kind = "unknown";
        }
        return HostKind::Pseudoconsole;
    }

    wchar_t class_name[128] = {};
    if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) == 0) {
        return force_v2_enabled() ? HostKind::ClassicConhost : HostKind::LegacyConhost;
    }

    if (wcscmp(class_name, kConhostWindowClass) == 0) {
        return force_v2_enabled() ? HostKind::ClassicConhost : HostKind::LegacyConhost;
    }
    if (wcscmp(class_name, kTerminalWindowClass) == 0) {
        if (terminal_kind != nullptr) {
            *terminal_kind = "WindowsTerminal.exe";
        }
        return HostKind::Pseudoconsole;
    }

    /* Any other window class means a third-party console host or terminal. */
    if (terminal_kind != nullptr && !process_name_of_window(window, *terminal_kind)) {
        *terminal_kind = "unknown";
    }
    return HostKind::Pseudoconsole;
}

bool is_windows_terminal_host() noexcept
{
    HWND window = GetConsoleWindow();
    if (window == nullptr) {
        return false;
    }
    wchar_t class_name[128] = {};
    if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) == 0) {
        return false;
    }
    return wcscmp(class_name, kTerminalWindowClass) == 0;
}

/* ------------------------------------------------------------------------- */
/* Registry key list                                                         */
/* ------------------------------------------------------------------------- */

std::vector<std::string> console_registry_keys()
{
    /*
     * Only these are ever snapshotted or written, and the backend confines every one of them
     * to HKEY_CURRENT_USER. The list is deliberately short and named after the documented
     * console settings; nothing here can influence how a command is executed.
     */
    return {
        "Console",
    };
}

} /* namespace cbt */

/* ------------------------------------------------------------------------- */
/* C ABI                                                                     */
/* ------------------------------------------------------------------------- */

namespace {

/*
 * The guard and opacity object for the C ABI. Both are process wide because the thing they
 * protect - this process's console - is process wide. Access is serialised so a second apply
 * cannot overwrite the state saved by the first, which would make the original appearance
 * unrecoverable.
 */
std::mutex g_abi_mutex;
cbt::ConsoleStateGuard g_abi_guard;
cbt::WindowOpacity g_abi_opacity;
bool g_abi_active = false;

} /* namespace */

extern "C" int cbt_platform_apply(const cbt_config *config, int apply_geometry, int apply_opacity)
{
    if (config == nullptr) {
        cbt::set_last_error("cbt_platform_apply: config is null");
        return CBT_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(g_abi_mutex);
    cbt::clear_last_error();

    if (!g_abi_active) {
        /*
         * Capture only once. If the tool is run twice in the same console the first capture is
         * still the true original state, so keeping it is what makes "restore" meaningful.
         */
        if (!g_abi_guard.capture()) {
            return CBT_E_UNSUPPORTED;
        }
    }

    cbt::ApplyOptions options;
    options.apply_palette = true;
    options.apply_attribute = true;
    options.apply_canvas = true;
    options.apply_geometry = apply_geometry != 0;
    options.buffer_columns = config->buffer_columns;
    options.buffer_rows = config->buffer_rows;
    options.window_columns = config->window_columns;
    options.window_rows = config->window_rows;

    if (!cbt::apply_live_state(*config, options)) {
        /*
         * Undo whatever succeeded before the failure, so the console is never half styled. The
         * original diagnostic from apply_live_state is preserved and prefixed, because "IO error" on
         * its own does not say whether the geometry, the palette or the attribute failed.
         */
        g_abi_guard.restore();
        std::string message = "colour/geometry: ";
        message += cbt::last_error().empty() ? "apply failed" : cbt::last_error();
        cbt::set_last_error(message);
        return CBT_E_IO;
    }

    if (apply_opacity != 0) {
        /*
         * open() must be attempted whenever the object has not already captured its baseline. The
         * condition deliberately tests "not captured yet" rather than "has no window": an object
         * whose window appeared and then disappeared would otherwise satisfy has_window() and be
         * skipped, leaving apply_percent() with no captured baseline.
         */
        if (!g_abi_opacity.opened() && !g_abi_opacity.open()) {
            /*
             * No window of our own (pseudoconsole): opacity is unavailable, but the colours were
             * applied successfully, so this is not a failure of the whole operation. The reason is
             * kept in the diagnostic rather than discarded, so a user who asked for opacity can see
             * why it did not happen.
             */
            g_abi_active = true;
            /*
             * The styling is the product: disarm the guard so the destructor does not undo it when
             * the process exits. Restoring is the job of an explicit cbt_platform_restore(), which
             * ignores the armed flag on purpose.
             */
            g_abi_guard.disarm();
            return CBT_OK;
        }
        if (config->opacity_percent < CBT_MAX_OPACITY) {
            if (!g_abi_opacity.apply_percent(config->opacity_percent)) {
                std::string message = "opacity: ";
                message += cbt::last_error().empty() ? "apply failed" : cbt::last_error();
                cbt::set_last_error(message);
                return CBT_E_IO;
            }
        }
    }

    /*
     * Success: disarm the guard so the ConsoleStateGuard destructor does not restore the palette
     * when this process exits. The tool's whole purpose is to leave the console styled, and the
     * deployed cmd AutoRun depends on the styling surviving the apply process; the original
     * appearance is put back by an explicit cbt_platform_restore() call, which deliberately
     * ignores armed_.
     */
    g_abi_guard.disarm();
    g_abi_active = true;
    return CBT_OK;
}

extern "C" int cbt_platform_restore(void)
{
    std::lock_guard<std::mutex> lock(g_abi_mutex);
    cbt::clear_last_error();

    int result = CBT_OK;
    if (g_abi_opacity.has_window()) {
        if (!g_abi_opacity.restore()) {
            result = CBT_E_IO;
        }
    }
    if (g_abi_guard.captured()) {
        if (!g_abi_guard.restore()) {
            result = CBT_E_IO;
        }
    }
    g_abi_active = false;
    return result;
}

extern "C" int cbt_platform_read(cbt_config *out, LiveConsoleState *out_state)
{
    if (out == nullptr) {
        cbt::set_last_error("cbt_platform_read: out is null");
        return CBT_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(g_abi_mutex);
    cbt::clear_last_error();

    LiveConsoleState state;
    if (!cbt::read_live_state(state)) {
        return CBT_E_UNSUPPORTED;
    }
    if (out_state != nullptr) {
        *out_state = state;
    }

    cbt_config_default(out);
    for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        out->palette[i] = state.palette[i];
    }
    cbt_attribute_to_slots(state.attributes, &out->foreground_slot, &out->background_slot);
    out->canvas_slot = CBT_SLOT_BLACK;
    out->buffer_columns = state.buffer_columns;
    out->buffer_rows = state.buffer_rows;
    out->window_columns = state.window_columns;
    out->window_rows = state.window_rows;
    if (state.font_height != 0) {
        out->font_size = state.font_height;
    }
    out->opacity_percent = CBT_MAX_OPACITY;

    /* Derive the canvas slot from the live default background rather than assuming slot 0. */
    out->canvas_slot = out->background_slot;
    return CBT_OK;
}

extern "C" const char *cbt_platform_last_error(void)
{
    /* Stable for the lifetime of the thread's last error; the caller must not free it. */
    return cbt::last_error().c_str();
}

extern "C" int cbt_platform_host_kind(void)
{
    return static_cast<int>(cbt::detect_host(nullptr));
}

extern "C" int cbt_platform_is_windows_terminal(void)
{
    return cbt::is_windows_terminal_host() ? 1 : 0;
}
