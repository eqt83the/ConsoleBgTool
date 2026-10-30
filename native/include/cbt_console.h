/*
 * ConsoleBgTool - native core (C11)
 * cbt_console.h - pure model layer: console colour model, attribute coding, registry
 * value encoding. No Win32 dependency, so it is unit-testable in isolation.
 *
 * References (official Microsoft documentation):
 *   CONSOLE_SCREEN_BUFFER_INFOEX  https://learn.microsoft.com/windows/console/console-screen-buffer-infoex
 *   Console screen buffers        https://learn.microsoft.com/windows/console/console-screen-buffers
 *   COLORREF                      https://learn.microsoft.com/windows/win32/gdi/colorref
 */
#ifndef CBT_CONSOLE_H
#define CBT_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define CBT_API __declspec(dllexport)
#else
#  define CBT_API
#endif

/* Number of entries in the console colour table (ColorTable[16]). */
#define CBT_COLOR_TABLE_SIZE 16

/*
 * A colour in the canonical, device independent order used by every public API of this
 * library: 0x00RRGGBB (red in the high byte).
 *
 * The Windows console stores colours as COLORREF, which is 0x00BBGGRR, and likewise the
 * HKCU\Console : ColorTableNN registry values. Converting between the two is the single
 * most common source of "the wrong colour appeared" bugs, so this library never lets the
 * two representations be confused: cbt_color_from_colorref() / cbt_color_to_colorref()
 * are the only permitted crossings.
 */
typedef uint32_t cbt_color;

/* Canonical console text attribute bits (WinCon.h semantics). */
#define CBT_FG_BLUE        0x0001u
#define CBT_FG_GREEN       0x0002u
#define CBT_FG_RED         0x0004u
#define CBT_FG_INTENSITY   0x0008u
#define CBT_BG_BLUE        0x0010u
#define CBT_BG_GREEN       0x0020u
#define CBT_BG_RED         0x0040u
#define CBT_BG_INTENSITY   0x0080u

/* The sixteen colour table slots, in the console's documented VRAM order. */
typedef enum cbt_slot {
    CBT_SLOT_BLACK = 0,
    CBT_SLOT_DARK_BLUE = 1,
    CBT_SLOT_DARK_GREEN = 2,
    CBT_SLOT_DARK_CYAN = 3,
    CBT_SLOT_DARK_RED = 4,
    CBT_SLOT_DARK_MAGENTA = 5,
    CBT_SLOT_DARK_YELLOW = 6,
    CBT_SLOT_GRAY = 7,
    CBT_SLOT_DARK_GRAY = 8,
    CBT_SLOT_BLUE = 9,
    CBT_SLOT_GREEN = 10,
    CBT_SLOT_CYAN = 11,
    CBT_SLOT_RED = 12,
    CBT_SLOT_MAGENTA = 13,
    CBT_SLOT_YELLOW = 14,
    CBT_SLOT_WHITE = 15
} cbt_slot;

/* Result codes. Zero is success so `if (rc != CBT_OK)` is the natural test. */
typedef enum cbt_status {
    CBT_OK = 0,
    CBT_E_INVALID_ARG,
    CBT_E_OUT_OF_RANGE,
    CBT_E_PARSE,
    CBT_E_NOT_FOUND,
    CBT_E_ACCESS,
    CBT_E_IO,
    CBT_E_UNSUPPORTED,
    CBT_E_INTERNAL,
    CBT_E_BUFFER_TOO_SMALL
} cbt_status;

/* Human readable name for a status code. Never returns NULL. */
CBT_API const char *cbt_status_name(cbt_status status);

/* ------------------------------------------------------------------------- */
/* Colour encoding                                                           */
/* ------------------------------------------------------------------------- */

/* Pack 8-bit components into the canonical 0x00RRGGBB form. */
CBT_API cbt_color cbt_color_rgb(uint8_t r, uint8_t g, uint8_t b);

CBT_API uint8_t cbt_color_r(cbt_color c);
CBT_API uint8_t cbt_color_g(cbt_color c);
CBT_API uint8_t cbt_color_b(cbt_color c);

/*
 * COLORREF (0x00BBGGRR) -> canonical, and back.
 *
 * The conversions are bit-exact inverses for every 24-bit value, which is asserted by the
 * unit tests.
 */
CBT_API cbt_color cbt_color_from_colorref(uint32_t colorref);
CBT_API uint32_t cbt_color_to_colorref(cbt_color color);

/*
 * Parse "#RRGGBB", "RRGGBB", "0xRRGGBB" or a decimal/hex integer.
 * Comparison for named colours is deliberately case-insensitive.
 * Returns CBT_OK and writes *out on success.
 */
CBT_API cbt_status cbt_color_parse(const char *text, cbt_color *out);

/*
 * Parse one of the sixteen documented slot names ("black", "dark-blue", "dark_blue",
 * "Gray", "WHITE", ...). Returns CBT_E_NOT_FOUND when the name is unknown.
 */
CBT_API cbt_status cbt_slot_parse(const char *name, int *out_slot);

/* Stable lower-case kebab-case name of a slot, e.g. CBT_SLOT_DARK_BLUE -> "dark-blue". */
CBT_API const char *cbt_slot_name(int slot);

/* ------------------------------------------------------------------------- */
/* Text attributes                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Build a console text attribute (the 16-bit wAttributes value) from a colour table slot
 * pair. index >= 8 sets the corresponding intensity bit.
 * Out-of-range slots yield CBT_E_OUT_OF_RANGE and leave *out_attr untouched.
 */
CBT_API cbt_status cbt_attribute_from_slots(int fg_slot, int bg_slot, uint16_t *out_attr);

/* Decode a text attribute back into its (foreground, background) slot numbers. */
CBT_API void cbt_attribute_to_slots(uint16_t attr, int *out_fg_slot, int *out_bg_slot);

/*
 * Build a text attribute directly from RGB. The console can only display the 16 colours
 * currently in the colour table, so the nearest slot by squared RGB distance is chosen.
 * `palette` must hold CBT_COLOR_TABLE_SIZE entries.
 */
CBT_API cbt_status cbt_attribute_from_rgb(cbt_color fg, cbt_color bg,
                                          const cbt_color *palette, uint16_t *out_attr);

/* Nearest palette slot by squared Euclidean distance in RGB space. */
CBT_API cbt_status cbt_color_nearest_slot(cbt_color color, const cbt_color *palette,
                                          int *out_slot);

/* ------------------------------------------------------------------------- */
/* Registry value encoding                                                   */
/* ------------------------------------------------------------------------- */

/*
 * HKCU\Console stores the screen buffer size and the window size as a single DWORD that
 * packs both COORD members: LOWORD = X (columns), HIWORD = Y (rows).
 *
 * Verified against this host: ScreenBufferSize = 589889656 -> 100 x 30 and
 * WindowSize = 1966200 -> 120 x 30, both of which match the console window exactly.
 * The encoding/decoding pair is round-trip tested, including the live-window cross-check.
 */
CBT_API uint32_t cbt_pack_coord(uint16_t x, uint16_t y);
CBT_API void cbt_unpack_coord(uint32_t packed, uint16_t *out_x, uint16_t *out_y);

/*
 * The registry form of a text attribute. HKCU\Console : ScreenColors uses the same bit
 * layout but the high nibble is masked off, so this always returns a value <= 0x00FF.
 */
CBT_API uint8_t cbt_attribute_to_registry(uint16_t attr);
CBT_API uint16_t cbt_attribute_from_registry(uint8_t value);

/* ------------------------------------------------------------------------- */
/* Limits / validation                                                       */
/* ------------------------------------------------------------------------- */

/* Documented, conservative bounds for the values this tool is willing to write. */
#define CBT_MIN_COLUMNS 20
#define CBT_MAX_COLUMNS 512
#define CBT_MIN_ROWS    5
#define CBT_MAX_ROWS    9999
#define CBT_MIN_FONT_SIZE 1
#define CBT_MAX_FONT_SIZE 200
#define CBT_MIN_OPACITY 10   /* below 10% the window is effectively unusable */
#define CBT_MAX_OPACITY 100
#define CBT_MAX_FACE_NAME 32 /* LF_FACESIZE, including the terminator */

/*
 * Bounds check for the values written into HKCU\Console. These deliberately refuse
 * nonsense (a zero-width window, a 40000-column buffer) before anything is written, so a
 * bad configuration can never leave the console in a state that breaks command execution.
 */
CBT_API cbt_status cbt_validate_geometry(uint16_t columns, uint16_t rows,
                                         uint16_t window_columns, uint16_t window_rows);
CBT_API cbt_status cbt_validate_font_size(uint16_t font_size);
CBT_API cbt_status cbt_validate_opacity(int opacity_percent);
CBT_API cbt_status cbt_validate_face_name(const char *face_name);

/* Percent (0-100) <-> the byte value conhost stores in WindowAlpha (0-255). */
CBT_API cbt_status cbt_opacity_percent_to_byte(int percent, uint8_t *out_byte);
CBT_API int cbt_opacity_byte_to_percent(uint8_t byte_value);

/* Default Windows 11 console palette allocation (the observed Campbell-like default). */
CBT_API void cbt_default_palette(cbt_color out_palette[CBT_COLOR_TABLE_SIZE]);

/* ------------------------------------------------------------------------- */
/* Console configuration model                                              */
/* ------------------------------------------------------------------------- */

/*
 * The handful of settings that determine how a cmd / PowerShell window looks. Only fields
 * that affect appearance are present: nothing here influences how a command runs, which
 * keeps "change the background" and "don't disturb command execution" separable by
 * construction rather than by discipline.
 */
typedef struct cbt_config {
    /*
     * The sixteen palette entries, canonical 0x00RRGGBB. Slot 0 is the palette entry that
     * a "black on light grey" console uses for its background, so it is usually the one
     * that paints the window background.
     */
    cbt_color palette[CBT_COLOR_TABLE_SIZE];

    /*
     * Default text attribute used for newly written characters, as a slot pair. For a
     * stock console this is light grey (7) on black (0).
     */
    int foreground_slot;
    int background_slot;

    /*
     * The colour the window is filled with before any text arrives. In the live console
     * this is slot 0 of the colour table; the registry form is ColorTable00.
     */
    int canvas_slot;

    /* Geometry. Screen buffer must be at least as large as the window. */
    uint16_t buffer_columns;
    uint16_t buffer_rows;
    uint16_t window_columns;
    uint16_t window_rows;

    /* Font. font_size is in the console's internal pixels-of-height unit. */
    uint16_t font_size;
    uint16_t font_weight;
    uint32_t font_family;
    char     face_name[CBT_MAX_FACE_NAME];

    /* Window opacity, 10-100 percent. 100 disables the layered-window path entirely. */
    int opacity_percent;
} cbt_config;

CBT_API void cbt_config_default(cbt_config *cfg);

/*
 * Validate every field of a configuration. Returns the first failure encountered and, when
 * `out_field` is non-NULL, the name of the offending field.
 */
CBT_API cbt_status cbt_config_validate(const cbt_config *cfg, const char **out_field);

/*
 * "light-on-dark" / "dark-on-light" preset selection based on the WCAG relative luminance
 * of the two slots, so a preset can never produce unreadable text.
 */
typedef enum cbt_theme_kind {
    CBT_THEME_LIGHT_ON_DARK = 0,
    CBT_THEME_DARK_ON_LIGHT = 1
} cbt_theme_kind;

CBT_API double cbt_color_relative_luminance(cbt_color color);
CBT_API double cbt_color_contrast_ratio(cbt_color a, cbt_color b);
CBT_API cbt_theme_kind cbt_config_theme(const cbt_config *cfg);

/*
 * Relative luminance of the slot actually painted as the window background.
 * Returns CBT_E_OUT_OF_RANGE for an invalid canvas slot.
 */
CBT_API cbt_status cbt_config_canvas_luminance(const cbt_config *cfg, double *out_luminance);

/*
 * Contrast between the default text colour and the canvas colour. Used by the CLI to warn
 * (never to block) when a user picks an unreadable combination.
 */
CBT_API cbt_status cbt_config_contrast(const cbt_config *cfg, double *out_ratio);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CBT_CONSOLE_H */
