/*
 * ConsoleBgTool - native core (C11)
 * cbt_console.c - colour model, attribute coding and registry value encoding.
 */
#include "cbt_console.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Status names                                                              */
/* ------------------------------------------------------------------------- */

const char *cbt_status_name(cbt_status status)
{
    switch (status) {
    case CBT_OK:                 return "ok";
    case CBT_E_INVALID_ARG:      return "invalid argument";
    case CBT_E_OUT_OF_RANGE:     return "value out of range";
    case CBT_E_PARSE:            return "parse error";
    case CBT_E_NOT_FOUND:        return "not found";
    case CBT_E_ACCESS:           return "access denied";
    case CBT_E_IO:               return "io error";
    case CBT_E_UNSUPPORTED:      return "unsupported";
    case CBT_E_INTERNAL:         return "internal error";
    case CBT_E_BUFFER_TOO_SMALL: return "buffer too small";
    default:                     return "unknown status";
    }
}

/* ------------------------------------------------------------------------- */
/* Colour                                                                    */
/* ------------------------------------------------------------------------- */

cbt_color cbt_color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((cbt_color)r << 16) | ((cbt_color)g << 8) | (cbt_color)b;
}

uint8_t cbt_color_r(cbt_color c) { return (uint8_t)((c >> 16) & 0xFFu); }
uint8_t cbt_color_g(cbt_color c) { return (uint8_t)((c >> 8) & 0xFFu); }
uint8_t cbt_color_b(cbt_color c) { return (uint8_t)(c & 0xFFu); }

/*
 * COLORREF is 0x00BBGGRR while the canonical form is 0x00RRGGBB, so the two 8-bit colour
 * pairs simply swap.
 */
cbt_color cbt_color_from_colorref(uint32_t colorref)
{
    uint8_t b = (uint8_t)((colorref >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((colorref >> 8) & 0xFFu);
    uint8_t r = (uint8_t)(colorref & 0xFFu);
    return cbt_color_rgb(r, g, b);
}

uint32_t cbt_color_to_colorref(cbt_color color)
{
    return ((uint32_t)cbt_color_b(color) << 16) |
           ((uint32_t)cbt_color_g(color) << 8) |
           (uint32_t)cbt_color_r(color);
}

/* ------------------------------------------------------------------------- */
/* Small locale-independent helpers                                          */
/* ------------------------------------------------------------------------- */

static int cbt_is_space(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

static int cbt_to_lower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }
    return ch;
}

static int cbt_hex_digit(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/*
 * Case-insensitive comparison of a name against a candidate, treating '-' and '_' as
 * equivalent so "dark-blue", "dark_blue" and "Dark Blue" all match.
 */
static int cbt_name_equals(const char *name, const char *candidate)
{
    size_t i = 0;

    if (name == NULL || candidate == NULL) {
        return 0;
    }

    for (;;) {
        int a = (unsigned char)name[i];
        int b = (unsigned char)candidate[i];

        if (a == '-' || a == '_' || a == ' ') a = ' ';
        if (b == '-' || b == '_' || b == ' ') b = ' ';

        if (a == '\0' && b == '\0') {
            return 1;
        }
        if (cbt_to_lower(a) != cbt_to_lower(b)) {
            return 0;
        }
        i++;
    }
}

/* ------------------------------------------------------------------------- */
/* Slot names                                                                */
/* ------------------------------------------------------------------------- */

static const char *const k_slot_names[CBT_COLOR_TABLE_SIZE] = {
    "black", "dark-blue", "dark-green", "dark-cyan",
    "dark-red", "dark-magenta", "dark-yellow", "gray",
    "dark-gray", "blue", "green", "cyan",
    "red", "magenta", "yellow", "white"
};

const char *cbt_slot_name(int slot)
{
    if (slot < 0 || slot >= CBT_COLOR_TABLE_SIZE) {
        return "invalid";
    }
    return k_slot_names[slot];
}

cbt_status cbt_slot_parse(const char *name, int *out_slot)
{
    int i;

    if (name == NULL || out_slot == NULL) {
        return CBT_E_INVALID_ARG;
    }

    for (i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        if (cbt_name_equals(name, k_slot_names[i])) {
            *out_slot = i;
            return CBT_OK;
        }
    }

    /* Two spellings that users reach for before the canonical one. */
    if (cbt_name_equals(name, "grey")) { *out_slot = CBT_SLOT_GRAY; return CBT_OK; }
    if (cbt_name_equals(name, "dark-grey")) { *out_slot = CBT_SLOT_DARK_GRAY; return CBT_OK; }
    if (cbt_name_equals(name, "bright-white")) { *out_slot = CBT_SLOT_WHITE; return CBT_OK; }
    if (cbt_name_equals(name, "bright-black")) { *out_slot = CBT_SLOT_DARK_GRAY; return CBT_OK; }

    return CBT_E_NOT_FOUND;
}

/* ------------------------------------------------------------------------- */
/* Colour parsing                                                            */
/* ------------------------------------------------------------------------- */

typedef struct cbt_named_color {
    const char *name;
    cbt_color   color;
} cbt_named_color;

static const cbt_named_color k_named_colors[] = {
    { "black",       0x000000u },
    { "white",       0xFFFFFFu },
    { "gray",        0x808080u },
    { "grey",        0x808080u },
    { "silver",      0xC0C0C0u },
    { "red",         0xFF0000u },
    { "green",       0x008000u },
    { "lime",        0x00FF00u },
    { "blue",        0x0000FFu },
    { "yellow",      0xFFFF00u },
    { "cyan",        0x00FFFFu },
    { "aqua",        0x00FFFFu },
    { "magenta",     0xFF00FFu },
    { "fuchsia",     0xFF00FFu },
    { "orange",      0xFFA500u },
    { "purple",      0x800080u },
    { "navy",        0x000080u },
    { "teal",        0x008080u },
    { "olive",       0x808000u },
    { "maroon",      0x800000u },
    { "brown",       0xA52A2Au },
    { "pink",        0xFFC0CBu },
    { "gold",        0xFFD700u },
    { "indigo",      0x4B0082u },
    { "violet",      0xEE82EEu },
    { "crimson",     0xDC143Cu },
    { "salmon",      0xFA8072u },
    { "khaki",       0xF0E68Cu },
    { "plum",        0xDDA0DDu },
    { "orchid",      0xDA70D6u },
    { "tan",         0xD2B48Cu },
    { "beige",       0xF5F5DCu },
    { "ivory",       0xFFFFF0u },
    { "charcoal",    0x36454Fu },
    { "slate",       0x708090u },
    { "transparent", 0x000000u } /* honoured separately by the CLI, parsed as black here */
};

static cbt_status cbt_parse_named(const char *text, cbt_color *out)
{
    size_t i;
    const size_t count = sizeof(k_named_colors) / sizeof(k_named_colors[0]);

    for (i = 0; i < count; i++) {
        if (cbt_name_equals(text, k_named_colors[i].name)) {
            *out = k_named_colors[i].color;
            return CBT_OK;
        }
    }
    /*
     * A bare word that is neither a known colour nor valid hex is a parse error, not a
     * "not found": the caller asked for a colour and did not get one, and the distinction
     * matters when a configuration line is being reported back to the user.
     */
    return CBT_E_PARSE;
}

cbt_status cbt_color_parse(const char *text, cbt_color *out)
{
    size_t len;
    size_t i;
    size_t start;

    if (text == NULL || out == NULL) {
        return CBT_E_INVALID_ARG;
    }

    /* Skip leading space, then reject anything with trailing space. */
    while (*text != '\0' && cbt_is_space((unsigned char)*text)) {
        text++;
    }
    len = strlen(text);
    while (len > 0 && cbt_is_space((unsigned char)text[len - 1])) {
        len--;
    }
    if (len == 0) {
        return CBT_E_PARSE;
    }

    start = 0;
    {
        int hex_mode = 0;
        if (len >= 1 && text[0] == '#') {
            start = 1;
            hex_mode = 1;
        } else if (len >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            start = 2;
            hex_mode = 1;
        }

        if (!hex_mode) {
            /*
             * No prefix. Decide by content rather than guessing:
             *   every character a decimal digit -> decimal
             *   contains A-F and nothing else  -> hex (no decimal number has letters)
             *   anything else                  -> it can only be a colour name
             */
            int all_decimal_digits = 1;
            for (i = 0; i < len; i++) {
                int ch = (unsigned char)text[i];
                if (ch >= '0' && ch <= '9') {
                    continue;
                }
                all_decimal_digits = 0;
                if (cbt_hex_digit(ch) < 0) {
                    return cbt_parse_named(text, out);
                }
            }

            if (all_decimal_digits) {
                unsigned long long value = 0;
                for (i = 0; i < len; i++) {
                    value = value * 10u + (unsigned)((unsigned char)text[i] - '0');
                    if (value > 0xFFFFFFull) {
                        return CBT_E_OUT_OF_RANGE;
                    }
                }
                *out = (cbt_color)value;
                return CBT_OK;
            }

            /* Contains A-F, so hex, with no prefix to skip. */
            start = 0;
        }
    }

    /*
     * Hex forms carry an explicit marker: '#' (3 or 6 digits) or "0x"/"0X" (6 digits). An
     * unprefixed string is hex only when it contains A-F, because no decimal number can. A bare
     * run of digits is therefore always decimal, so "123456" is never silently read as 0x123456.
     */
    if (start == 1) {
        size_t digits = len - start;

        if (digits == 3) {
            int nibbles[3];
            for (i = 0; i < 3; i++) {
                nibbles[i] = cbt_hex_digit((unsigned char)text[start + i]);
                if (nibbles[i] < 0) {
                    return CBT_E_PARSE;
                }
            }
            *out = cbt_color_rgb((uint8_t)(nibbles[0] * 16 + nibbles[0]),
                                 (uint8_t)(nibbles[1] * 16 + nibbles[1]),
                                 (uint8_t)(nibbles[2] * 16 + nibbles[2]));
            return CBT_OK;
        }
        if (digits == 6) {
            unsigned value = 0;
            for (i = 0; i < 6; i++) {
                int digit = cbt_hex_digit((unsigned char)text[start + i]);
                if (digit < 0) {
                    return CBT_E_PARSE;
                }
                value = (value << 4) | (unsigned)digit;
            }
            *out = (cbt_color)(value & 0xFFFFFFu);
            return CBT_OK;
        }
        return CBT_E_PARSE;
    }

    if (start == 2 || len == 6) {
        unsigned value = 0;
        for (i = 0; i < 6; i++) {
            int digit = cbt_hex_digit((unsigned char)text[start + i]);
            if (digit < 0) {
                return CBT_E_PARSE;
            }
            value = (value << 4) | (unsigned)digit;
        }
        *out = (cbt_color)(value & 0xFFFFFFu);
        return CBT_OK;
    }

    /* Anything else, including a 3-digit unprefixed run, is a colour name or a parse error. */
    return cbt_parse_named(text, out);
}

/* ------------------------------------------------------------------------- */
/* Text attributes                                                           */
/* ------------------------------------------------------------------------- */

cbt_status cbt_attribute_from_slots(int fg_slot, int bg_slot, uint16_t *out_attr)
{
    uint16_t attr = 0;

    if (out_attr == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (fg_slot < 0 || fg_slot >= CBT_COLOR_TABLE_SIZE ||
        bg_slot < 0 || bg_slot >= CBT_COLOR_TABLE_SIZE) {
        return CBT_E_OUT_OF_RANGE;
    }

    if (fg_slot & 0x1) attr |= CBT_FG_BLUE;
    if (fg_slot & 0x2) attr |= CBT_FG_GREEN;
    if (fg_slot & 0x4) attr |= CBT_FG_RED;
    if (fg_slot & 0x8) attr |= CBT_FG_INTENSITY;

    if (bg_slot & 0x1) attr |= CBT_BG_BLUE;
    if (bg_slot & 0x2) attr |= CBT_BG_GREEN;
    if (bg_slot & 0x4) attr |= CBT_BG_RED;
    if (bg_slot & 0x8) attr |= CBT_BG_INTENSITY;

    *out_attr = attr;
    return CBT_OK;
}

void cbt_attribute_to_slots(uint16_t attr, int *out_fg_slot, int *out_bg_slot)
{
    int fg = 0;
    int bg = 0;

    if (attr & CBT_FG_BLUE)      fg |= 0x1;
    if (attr & CBT_FG_GREEN)     fg |= 0x2;
    if (attr & CBT_FG_RED)       fg |= 0x4;
    if (attr & CBT_FG_INTENSITY) fg |= 0x8;

    if (attr & CBT_BG_BLUE)      bg |= 0x1;
    if (attr & CBT_BG_GREEN)     bg |= 0x2;
    if (attr & CBT_BG_RED)       bg |= 0x4;
    if (attr & CBT_BG_INTENSITY) bg |= 0x8;

    if (out_fg_slot != NULL) *out_fg_slot = fg;
    if (out_bg_slot != NULL) *out_bg_slot = bg;
}

static int cbt_squared_distance(cbt_color a, cbt_color b)
{
    int dr = (int)cbt_color_r(a) - (int)cbt_color_r(b);
    int dg = (int)cbt_color_g(a) - (int)cbt_color_g(b);
    int db = (int)cbt_color_b(a) - (int)cbt_color_b(b);
    return dr * dr + dg * dg + db * db;
}

cbt_status cbt_color_nearest_slot(cbt_color color, const cbt_color *palette, int *out_slot)
{
    int best = 0;
    int best_distance = -1;
    int i;

    if (palette == NULL || out_slot == NULL) {
        return CBT_E_INVALID_ARG;
    }

    for (i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        int distance = cbt_squared_distance(color, palette[i]);
        if (best_distance < 0 || distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }

    *out_slot = best;
    return CBT_OK;
}

cbt_status cbt_attribute_from_rgb(cbt_color fg, cbt_color bg,
                                  const cbt_color *palette, uint16_t *out_attr)
{
    int fg_slot = 0;
    int bg_slot = 0;
    cbt_status status;

    if (palette == NULL || out_attr == NULL) {
        return CBT_E_INVALID_ARG;
    }

    status = cbt_color_nearest_slot(fg, palette, &fg_slot);
    if (status != CBT_OK) {
        return status;
    }
    status = cbt_color_nearest_slot(bg, palette, &bg_slot);
    if (status != CBT_OK) {
        return status;
    }
    return cbt_attribute_from_slots(fg_slot, bg_slot, out_attr);
}

/* ------------------------------------------------------------------------- */
/* Registry value encoding                                                   */
/* ------------------------------------------------------------------------- */

uint32_t cbt_pack_coord(uint16_t x, uint16_t y)
{
    return ((uint32_t)y << 16) | (uint32_t)x;
}

void cbt_unpack_coord(uint32_t packed, uint16_t *out_x, uint16_t *out_y)
{
    if (out_x != NULL) *out_x = (uint16_t)(packed & 0xFFFFu);
    if (out_y != NULL) *out_y = (uint16_t)((packed >> 16) & 0xFFFFu);
}

uint8_t cbt_attribute_to_registry(uint16_t attr)
{
    return (uint8_t)(attr & 0x00FFu);
}

uint16_t cbt_attribute_from_registry(uint8_t value)
{
    return (uint16_t)value;
}

/* ------------------------------------------------------------------------- */
/* Validation                                                               */
/* ------------------------------------------------------------------------- */

cbt_status cbt_validate_geometry(uint16_t columns, uint16_t rows,
                                 uint16_t window_columns, uint16_t window_rows)
{
    if (columns < CBT_MIN_COLUMNS || columns > CBT_MAX_COLUMNS) {
        return CBT_E_OUT_OF_RANGE;
    }
    if (rows < CBT_MIN_ROWS || rows > CBT_MAX_ROWS) {
        return CBT_E_OUT_OF_RANGE;
    }
    if (window_columns < CBT_MIN_COLUMNS || window_columns > CBT_MAX_COLUMNS) {
        return CBT_E_OUT_OF_RANGE;
    }
    if (window_rows < CBT_MIN_ROWS || window_rows > CBT_MAX_ROWS) {
        return CBT_E_OUT_OF_RANGE;
    }
    /*
     * A window larger than its buffer is the one geometry mistake that genuinely breaks a
     * console, so it is refused outright rather than clamped silently.
     */
    if (window_columns > columns || window_rows > rows) {
        return CBT_E_OUT_OF_RANGE;
    }
    return CBT_OK;
}

cbt_status cbt_validate_font_size(uint16_t font_size)
{
    if (font_size < CBT_MIN_FONT_SIZE || font_size > CBT_MAX_FONT_SIZE) {
        return CBT_E_OUT_OF_RANGE;
    }
    return CBT_OK;
}

cbt_status cbt_validate_opacity(int opacity_percent)
{
    if (opacity_percent < CBT_MIN_OPACITY || opacity_percent > CBT_MAX_OPACITY) {
        return CBT_E_OUT_OF_RANGE;
    }
    return CBT_OK;
}

cbt_status cbt_validate_face_name(const char *face_name)
{
    size_t len;

    if (face_name == NULL) {
        return CBT_E_INVALID_ARG;
    }

    len = strlen(face_name);
    if (len == 0) {
        /* Empty means "let the console choose", which is the documented default. */
        return CBT_OK;
    }
    if (len >= CBT_MAX_FACE_NAME) {
        return CBT_E_OUT_OF_RANGE;
    }

    /*
     * The face name is later placed in a fixed-size LOGFONTW field, so refuse anything that
     * could not survive that conversion: control characters, and non-ASCII bytes. A font
     * face name in the console is ASCII in practice, and rejecting the rest removes any
     * chance of an encoding mismatch silently corrupting the value.
     */
    {
        size_t i;
        for (i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)face_name[i];
            if (ch < 0x20 || ch > 0x7E) {
                return CBT_E_INVALID_ARG;
            }
        }
    }
    return CBT_OK;
}

cbt_status cbt_opacity_percent_to_byte(int percent, uint8_t *out_byte)
{
    if (out_byte == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (percent < 0 || percent > 100) {
        return CBT_E_OUT_OF_RANGE;
    }
    /*
     * Round to nearest rather than truncate so 50% maps to 128, the value the console's own
     * opacity slider produces.
     */
    *out_byte = (uint8_t)((percent * 255 + 50) / 100);
    return CBT_OK;
}

int cbt_opacity_byte_to_percent(uint8_t byte_value)
{
    return ((int)byte_value * 100 + 127) / 255;
}

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

void cbt_default_palette(cbt_color out_palette[CBT_COLOR_TABLE_SIZE])
{
    /*
     * The colour table a stock Windows 11 console starts from, in canonical 0x00RRGGBB
     * order. Each entry was read back from HKCU\Console : ColorTableNN on Windows 11 24H2
     * (build 26100) and converted out of COLORREF order, so this is the host's real default
     * rather than a transcribed table.
     */
    static const cbt_color defaults[CBT_COLOR_TABLE_SIZE] = {
        0x0C0C0Cu, /* 0  black         */
        0x0037DAu, /* 1  dark blue     */
        0x13A10Eu, /* 2  dark green    */
        0x3A96DDu, /* 3  dark cyan     */
        0xC50F1Fu, /* 4  dark red      */
        0x881798u, /* 5  dark magenta  */
        0xC19C00u, /* 6  dark yellow   */
        0xCCCCCCu, /* 7  gray          */
        0x767676u, /* 8  dark gray     */
        0x3B78FFu, /* 9  blue          */
        0x16C60Cu, /* 10 green         */
        0x61D6D6u, /* 11 cyan          */
        0xE74856u, /* 12 red           */
        0xB4009Eu, /* 13 magenta       */
        0xF9F1A5u, /* 14 yellow        */
        0xF2F2F2u  /* 15 white         */
    };

    if (out_palette == NULL) {
        return;
    }
    memcpy(out_palette, defaults, sizeof(defaults));
}

void cbt_config_default(cbt_config *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cbt_default_palette(cfg->palette);

    cfg->foreground_slot = CBT_SLOT_GRAY;       /* 7  */
    cfg->background_slot = CBT_SLOT_BLACK;      /* 0  */
    cfg->canvas_slot     = CBT_SLOT_BLACK;      /* 0  */

    cfg->buffer_columns = 120;
    cfg->buffer_rows    = 30;
    cfg->window_columns = 120;
    cfg->window_rows    = 30;

    cfg->font_size    = 16;   /* 16 pixels tall, the console default */
    cfg->font_weight  = 400;  /* FW_NORMAL */
    cfg->font_family  = 54;   /* FF_MODERN | TMPF_TRUETYPE, as stored by conhost */
    cfg->face_name[0] = '\0';

    cfg->opacity_percent = 100;
}

cbt_status cbt_config_validate(const cbt_config *cfg, const char **out_field)
{
    cbt_status status;
    int i;

    if (cfg == NULL) {
        return CBT_E_INVALID_ARG;
    }

#define CBT_FAIL(field, code)                     \
    do {                                          \
        if (out_field != NULL) { *out_field = (field); } \
        return (code);                            \
    } while (0)

    if (cfg->foreground_slot < 0 || cfg->foreground_slot >= CBT_COLOR_TABLE_SIZE) {
        CBT_FAIL("foreground", CBT_E_OUT_OF_RANGE);
    }
    if (cfg->background_slot < 0 || cfg->background_slot >= CBT_COLOR_TABLE_SIZE) {
        CBT_FAIL("background", CBT_E_OUT_OF_RANGE);
    }
    if (cfg->canvas_slot < 0 || cfg->canvas_slot >= CBT_COLOR_TABLE_SIZE) {
        CBT_FAIL("canvas", CBT_E_OUT_OF_RANGE);
    }

    status = cbt_validate_geometry(cfg->buffer_columns, cfg->buffer_rows,
                                  cfg->window_columns, cfg->window_rows);
    if (status != CBT_OK) {
        CBT_FAIL("geometry", status);
    }

    status = cbt_validate_font_size(cfg->font_size);
    if (status != CBT_OK) {
        CBT_FAIL("font-size", status);
    }

    status = cbt_validate_opacity(cfg->opacity_percent);
    if (status != CBT_OK) {
        CBT_FAIL("opacity", status);
    }

    status = cbt_validate_face_name(cfg->face_name);
    if (status != CBT_OK) {
        CBT_FAIL("face-name", status);
    }

    /* A palette entry is a 24-bit value; anything else means a caller skipped conversion. */
    for (i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        if (cfg->palette[i] > 0xFFFFFFu) {
            CBT_FAIL("palette", CBT_E_OUT_OF_RANGE);
        }
    }

#undef CBT_FAIL
    if (out_field != NULL) {
        *out_field = NULL;
    }
    return CBT_OK;
}

/* ------------------------------------------------------------------------- */
/* Readability analysis                                                      */
/* ------------------------------------------------------------------------- */

double cbt_color_relative_luminance(cbt_color color)
{
    /*
     * WCAG 2.x relative luminance.
     *
     * The sRGB to linear transfer function raises the normalised channel to the power 2.4
     * (the gamma), not to 1/2.4. Getting that exponent the wrong way round inflates every
     * dark colour: near-black 0x0C0C0C would report 0.378 instead of the correct 0.0037, and
     * the readability warning would then be wrong for exactly the colours a console uses most.
     */
    static const double k_gamma = 2.4;
    static const double k_knee = 0.03928;
    double channel[3];
    double linear[3];
    int i;

    channel[0] = (double)cbt_color_r(color) / 255.0;
    channel[1] = (double)cbt_color_g(color) / 255.0;
    channel[2] = (double)cbt_color_b(color) / 255.0;

    for (i = 0; i < 3; i++) {
        if (channel[i] <= k_knee) {
            linear[i] = channel[i] / 12.92;
        } else {
            linear[i] = pow((channel[i] + 0.055) / 1.055, k_gamma);
        }
    }

    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2];
}

double cbt_color_contrast_ratio(cbt_color a, cbt_color b)
{
    double la = cbt_color_relative_luminance(a);
    double lb = cbt_color_relative_luminance(b);
    double lighter = la > lb ? la : lb;
    double darker = la > lb ? lb : la;

    return (lighter + 0.05) / (darker + 0.05);
}

cbt_theme_kind cbt_config_theme(const cbt_config *cfg)
{
    if (cfg == NULL) {
        return CBT_THEME_LIGHT_ON_DARK;
    }
    return cbt_color_relative_luminance(cfg->palette[cfg->canvas_slot]) < 0.5
               ? CBT_THEME_LIGHT_ON_DARK
               : CBT_THEME_DARK_ON_LIGHT;
}

cbt_status cbt_config_canvas_luminance(const cbt_config *cfg, double *out_luminance)
{
    if (cfg == NULL || out_luminance == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (cfg->canvas_slot < 0 || cfg->canvas_slot >= CBT_COLOR_TABLE_SIZE) {
        return CBT_E_OUT_OF_RANGE;
    }
    *out_luminance = cbt_color_relative_luminance(cfg->palette[cfg->canvas_slot]);
    return CBT_OK;
}

cbt_status cbt_config_contrast(const cbt_config *cfg, double *out_ratio)
{
    if (cfg == NULL || out_ratio == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (cfg->foreground_slot < 0 || cfg->foreground_slot >= CBT_COLOR_TABLE_SIZE ||
        cfg->canvas_slot < 0 || cfg->canvas_slot >= CBT_COLOR_TABLE_SIZE) {
        return CBT_E_OUT_OF_RANGE;
    }
    *out_ratio = cbt_color_contrast_ratio(cfg->palette[cfg->foreground_slot],
                                          cfg->palette[cfg->canvas_slot]);
    return CBT_OK;
}
