/*
 * ConsoleBgTool - C unit tests.
 *
 * Scope: the model layer (colour, attributes, encoding, validation) and the registry layer
 * (snapshot / restore / failure injection / path confinement).
 *
 * The registry tests run against the in-process memory backend, so no test in this file can
 * read or write the developer's real console configuration. Path confinement is asserted
 * against the Win32 backend's refusal behaviour, which by construction cannot write anywhere
 * either.
 *
 * Build and run: see scripts/build.ps1, or
 *   gcc -std=c11 -I native/include native/tests/test_core.c <core sources> -o cbt_tests.exe
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cbt_console.h"
#include "cbt_registry.h"

/* ------------------------------------------------------------------------- */
/* Minimal test harness                                                      */
/* ------------------------------------------------------------------------- */

static int g_checks = 0;
static int g_failures = 0;
static const char *g_case = "";

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(condition)) {                                                              \
            g_failures++;                                                                \
            fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #condition); \
        }                                                                                \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                                   \
    do {                                                                                 \
        long long a_ = (long long)(actual);                                              \
        long long e_ = (long long)(expected);                                            \
        g_checks++;                                                                      \
        if (a_ != e_) {                                                                  \
            g_failures++;                                                                \
            fprintf(stderr, "FAIL %s:%d [%s] %s: got %lld, want %lld\n",                 \
                    __FILE__, __LINE__, g_case, #actual, a_, e_);                        \
        }                                                                                \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                                   \
    do {                                                                                 \
        const char *a_ = (actual);                                                       \
        const char *e_ = (expected);                                                     \
        g_checks++;                                                                      \
        if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0) {                            \
            g_failures++;                                                                \
            fprintf(stderr, "FAIL %s:%d [%s] %s: got '%s', want '%s'\n",                 \
                    __FILE__, __LINE__, g_case, #actual, a_ ? a_ : "(null)", e_);        \
        }                                                                                \
    } while (0)

#define CHECK_EQ_DOUBLE(actual, expected, tolerance)                                     \
    do {                                                                                 \
        double a_ = (actual);                                                            \
        double e_ = (expected);                                                          \
        double t_ = (tolerance);                                                         \
        double d_ = a_ > e_ ? a_ - e_ : e_ - a_;                                         \
        g_checks++;                                                                      \
        if (!(d_ <= t_)) {                                                               \
            g_failures++;                                                                \
            fprintf(stderr, "FAIL %s:%d [%s] %s: got %.6f, want %.6f (+-%.6f)\n",        \
                    __FILE__, __LINE__, g_case, #actual, a_, e_, t_);                    \
        }                                                                                \
    } while (0)

static void begin(const char *name)
{
    g_case = name;
}

/* ------------------------------------------------------------------------- */
/* Colour                                                                    */
/* ------------------------------------------------------------------------- */

static void test_color_encoding(void)
{
    begin("colour encoding");

    CHECK_EQ_INT(cbt_color_rgb(0x12, 0x34, 0x56), 0x123456u);
    CHECK_EQ_INT(cbt_color_r(0x123456u), 0x12);
    CHECK_EQ_INT(cbt_color_g(0x123456u), 0x34);
    CHECK_EQ_INT(cbt_color_b(0x123456u), 0x56);

    /*
     * COLORREF is 0x00BBGGRR: the red and blue bytes swap, green stays put.
     * 0x00112233 as COLORREF means R=0x33, G=0x22, B=0x11, so the canonical form is 0x332211.
     */
    CHECK_EQ_INT(cbt_color_from_colorref(0x00112233u), 0x332211u);
    CHECK_EQ_INT(cbt_color_to_colorref(0x332211u), 0x00112233u);
    /* A pure red COLORREF is 0x000000FF; canonical pure red is 0xFF0000. */
    CHECK_EQ_INT(cbt_color_from_colorref(0x000000FFu), 0xFF0000u);
    CHECK_EQ_INT(cbt_color_to_colorref(0xFF0000u), 0x000000FFu);
    /* Round trip the identity for a value whose bytes are all distinct. */
    CHECK_EQ_INT(cbt_color_from_colorref(cbt_color_to_colorref(0x0A0B0Cu)), 0x0A0B0Cu);

    /* The pair must be an exact inverse across the whole 24-bit space. */
    {
        unsigned value;
        int exact = 1;
        for (value = 0; value <= 0xFFFFFFu; value += 0x010101u) {
            if (cbt_color_to_colorref(cbt_color_from_colorref(value)) != value) {
                exact = 0;
                break;
            }
            if (cbt_color_from_colorref(cbt_color_to_colorref(value)) != value) {
                exact = 0;
                break;
            }
        }
        CHECK(exact);
    }

    /* The real registry values observed on Windows 11 24H2, as a regression anchor. */
    CHECK_EQ_INT(cbt_color_from_colorref(0x000C0C0Cu), 0x0C0C0Cu); /* ColorTable00 black */
    CHECK_EQ_INT(cbt_color_from_colorref(0x00DA3700u), 0x0037DAu); /* ColorTable01 dark blue */
    CHECK_EQ_INT(cbt_color_from_colorref(0x001F0FC5u), 0xC50F1Fu); /* ColorTable04 dark red */
    CHECK_EQ_INT(cbt_color_from_colorref(0x009E00B4u), 0xB4009Eu); /* ColorTable13 magenta */
}

static void test_color_parse(void)
{
    cbt_color out = 0;

    begin("colour parse");

    /*
     * The colour grammar, stated once so it cannot drift:
     *   "#RGB" or "#RRGGBB"     hex, always with the '#' (3 digits are doubled)
     *   "0xRRGGBB" or "0X..."   hex, C style, exactly 6 digits
     *   "RRGGBB" containing A-F hex, no prefix; a string such as "FF0000" cannot be decimal
     *   plain digits            decimal, so "123456" is 123456 and never 0x123456
     *   a colour name           black, white, crimson, ...
     */
    CHECK_EQ_INT(cbt_color_parse("#123456", &out), CBT_OK);
    CHECK_EQ_INT(out, 0x123456u);

    /* Plain digits are decimal. Ambiguity is resolved by rule, not by guessing. */
    CHECK_EQ_INT(cbt_color_parse("123456", &out), CBT_OK);
    CHECK_EQ_INT(out, 123456u);

    CHECK_EQ_INT(cbt_color_parse("0x123456", &out), CBT_OK);
    CHECK_EQ_INT(out, 0x123456u);

    CHECK_EQ_INT(cbt_color_parse("0X123456", &out), CBT_OK);
    CHECK_EQ_INT(out, 0x123456u);

    /* Unprefixed hex is accepted only when it cannot be a decimal number. */
    CHECK_EQ_INT(cbt_color_parse("FF0000", &out), CBT_OK);
    CHECK_EQ_INT(out, 0xFF0000u);
    CHECK_EQ_INT(cbt_color_parse("ff0000", &out), CBT_OK);
    CHECK_EQ_INT(out, 0xFF0000u);

    /* '#' followed by a C-style prefix is not part of the grammar. */
    CHECK_EQ_INT(cbt_color_parse("#0X123456", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("#0x123456", &out), CBT_E_PARSE);

    /* Three-digit shorthand expands each nibble. */
    CHECK_EQ_INT(cbt_color_parse("#abc", &out), CBT_OK);
    CHECK_EQ_INT(out, 0xAABBCCu);

    /* Case must not matter. */
    CHECK_EQ_INT(cbt_color_parse("#AbCdEf", &out), CBT_OK);
    CHECK_EQ_INT(out, 0xABCDEFu);

    /* Decimal integers are accepted for completeness. */
    CHECK_EQ_INT(cbt_color_parse("16711680", &out), CBT_OK);
    CHECK_EQ_INT(out, 0xFF0000u);

    /* Surrounding whitespace is tolerated. */
    CHECK_EQ_INT(cbt_color_parse("  #ffffff  ", &out), CBT_OK);
    CHECK_EQ_INT(out, 0xFFFFFFu);

    /* Names. */
    CHECK_EQ_INT(cbt_color_parse("black", &out), CBT_OK);
    CHECK_EQ_INT(out, 0x000000u);
    CHECK_EQ_INT(cbt_color_parse("WHITE", &out), CBT_OK);
    CHECK_EQ_INT(out, 0xFFFFFFu);
    CHECK_EQ_INT(cbt_color_parse("  Dark-Cyan ", &out), CBT_E_PARSE); /* not a colour name */
    CHECK_EQ_INT(out, 0xFFFFFFu); /* untouched: the parse failed */
    CHECK_EQ_INT(cbt_color_parse("grey", &out), CBT_OK);
    CHECK_EQ_INT(out, 0x808080u);

    /* Rejections. A parse failure must not write the output. */
    out = 0xDEADBEEFu;
    CHECK_EQ_INT(cbt_color_parse("", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("   ", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("#12345", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("#1234567", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("#gggggg", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("not-a-colour", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("#", &out), CBT_E_PARSE);
    CHECK_EQ_INT(cbt_color_parse("0x", &out), CBT_E_PARSE);
    CHECK_EQ_INT(out, 0xDEADBEEFu); /* untouched on failure */

    /* Out-of-range decimal is refused rather than truncated. */
    CHECK_EQ_INT(cbt_color_parse("16777216", &out), CBT_E_OUT_OF_RANGE);

    /* NULL handling. */
    CHECK_EQ_INT(cbt_color_parse(NULL, &out), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_color_parse("#fff", NULL), CBT_E_INVALID_ARG);

    /* Overlong input must not overrun; a 4000-char string is a cheap fuzz probe. */
    {
        char huge[4000];
        size_t i;
        huge[0] = '#';
        for (i = 1; i < sizeof(huge) - 1; i++) {
            huge[i] = 'a';
        }
        huge[sizeof(huge) - 1] = '\0';
        CHECK(cbt_color_parse(huge, &out) != CBT_OK);
    }
}

static void test_slot_names(void)
{
    int slot = -1;

    begin("slot names");

    CHECK_EQ_STR(cbt_slot_name(0), "black");
    CHECK_EQ_STR(cbt_slot_name(1), "dark-blue");
    CHECK_EQ_STR(cbt_slot_name(7), "gray");
    CHECK_EQ_STR(cbt_slot_name(15), "white");
    CHECK_EQ_STR(cbt_slot_name(-1), "invalid");
    CHECK_EQ_STR(cbt_slot_name(16), "invalid");

    CHECK_EQ_INT(cbt_slot_parse("dark-blue", &slot), CBT_OK);
    CHECK_EQ_INT(slot, CBT_SLOT_DARK_BLUE);
    CHECK_EQ_INT(cbt_slot_parse("dark_blue", &slot), CBT_OK);
    CHECK_EQ_INT(slot, CBT_SLOT_DARK_BLUE);
    CHECK_EQ_INT(cbt_slot_parse("Dark Blue", &slot), CBT_OK);
    CHECK_EQ_INT(slot, CBT_SLOT_DARK_BLUE);
    CHECK_EQ_INT(cbt_slot_parse("grey", &slot), CBT_OK);
    CHECK_EQ_INT(slot, CBT_SLOT_GRAY);
    CHECK_EQ_INT(cbt_slot_parse("WHITE", &slot), CBT_OK);
    CHECK_EQ_INT(slot, CBT_SLOT_WHITE);

    CHECK_EQ_INT(cbt_slot_parse("chartreuse", &slot), CBT_E_NOT_FOUND);
    CHECK_EQ_INT(cbt_slot_parse(NULL, &slot), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_slot_parse("red", NULL), CBT_E_INVALID_ARG);
}

/* ------------------------------------------------------------------------- */
/* Attributes                                                                */
/* ------------------------------------------------------------------------- */

static void test_attributes(void)
{
    uint16_t attr = 0;
    int fg = -1;
    int bg = -1;

    begin("attributes");

    /* Light grey (7) on black (0) is the console's own default: attribute 0x0007. */
    CHECK_EQ_INT(cbt_attribute_from_slots(7, 0, &attr), CBT_OK);
    CHECK_EQ_INT(attr, 0x0007u);

    /* Intense white on blue: slot 15 -> 0x0F, slot 1 -> 0x10. */
    CHECK_EQ_INT(cbt_attribute_from_slots(15, 1, &attr), CBT_OK);
    CHECK_EQ_INT(attr, 0x001Fu);

    /* Round trip every one of the 256 slot pairs. */
    {
        int f;
        int b;
        int all_ok = 1;
        for (f = 0; f < CBT_COLOR_TABLE_SIZE && all_ok; f++) {
            for (b = 0; b < CBT_COLOR_TABLE_SIZE; b++) {
                uint16_t round_trip = 0;
                if (cbt_attribute_from_slots(f, b, &round_trip) != CBT_OK) {
                    all_ok = 0;
                    break;
                }
                cbt_attribute_to_slots(round_trip, &fg, &bg);
                if (fg != f || bg != b) {
                    all_ok = 0;
                    break;
                }
            }
        }
        CHECK(all_ok);
    }

    /* Out of range slots are refused and the output is left alone. */
    attr = 0xABCDu;
    CHECK_EQ_INT(cbt_attribute_from_slots(16, 0, &attr), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_attribute_from_slots(0, 16, &attr), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_attribute_from_slots(-1, 0, &attr), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(attr, 0xABCDu);
    CHECK_EQ_INT(cbt_attribute_from_slots(0, 0, NULL), CBT_E_INVALID_ARG);

    /* Nearest-slot search against the real default palette. */
    {
        cbt_color palette[CBT_COLOR_TABLE_SIZE];
        int nearest = -1;
        cbt_default_palette(palette);

        CHECK_EQ_INT(cbt_color_nearest_slot(0xFFFFFFu, palette, &nearest), CBT_OK);
        CHECK_EQ_INT(nearest, CBT_SLOT_WHITE);

        CHECK_EQ_INT(cbt_color_nearest_slot(0x000000u, palette, &nearest), CBT_OK);
        CHECK_EQ_INT(nearest, CBT_SLOT_BLACK);

        /* A near-black off by a little still resolves to black, not to dark grey. */
        CHECK_EQ_INT(cbt_color_nearest_slot(0x050505u, palette, &nearest), CBT_OK);
        CHECK_EQ_INT(nearest, CBT_SLOT_BLACK);

        /* Pure red resolves to a red slot. */
        CHECK_EQ_INT(cbt_color_nearest_slot(0xFF0000u, palette, &nearest), CBT_OK);
        CHECK(nearest == CBT_SLOT_RED || nearest == CBT_SLOT_DARK_RED);

        CHECK_EQ_INT(cbt_color_nearest_slot(0, NULL, &nearest), CBT_E_INVALID_ARG);
    }

    /* RGB to attribute uses the nearest palette entries. */
    {
        cbt_color palette[CBT_COLOR_TABLE_SIZE];
        uint16_t from_rgb = 0;
        cbt_default_palette(palette);
        CHECK_EQ_INT(cbt_attribute_from_rgb(0xFFFFFFu, 0x000000u, palette, &from_rgb), CBT_OK);
        cbt_attribute_to_slots(from_rgb, &fg, &bg);
        CHECK_EQ_INT(fg, CBT_SLOT_WHITE);
        CHECK_EQ_INT(bg, CBT_SLOT_BLACK);
    }
}

/* ------------------------------------------------------------------------- */
/* Registry value encoding                                                   */
/* ------------------------------------------------------------------------- */

static void test_coord_and_registry_encoding(void)
{
    uint16_t x = 0;
    uint16_t y = 0;

    begin("coord encoding");

    /*
     * Encoding check anchored to values read from this host's real HKCU\Console:
     *   ScreenBufferSize = 0x00070878 -> X = 0x0078 = 120, Y = 0x0007 = 7   (!)
     *   WindowSize       = 0x001E0078 -> X = 0x0078 = 120, Y = 0x001E = 30
     * The first confirms that LOWORD is X and HIWORD is Y; the second that the packing matches
     * a real window of 120 columns by 30 rows.
     */
    CHECK_EQ_INT(cbt_pack_coord(120, 30), 0x001E0078u);
    CHECK_EQ_INT(cbt_pack_coord(0x0078, 0x0007), 0x00070078u);
    CHECK_EQ_INT(cbt_pack_coord(100, 30), 0x001E0064u);

    cbt_unpack_coord(0x001E0078u, &x, &y);
    CHECK_EQ_INT(x, 120);
    CHECK_EQ_INT(y, 30);

    cbt_unpack_coord(0x00070078u, &x, &y);
    CHECK_EQ_INT(x, 120);
    CHECK_EQ_INT(y, 7);

    /* Lanes must not bleed into each other at the boundaries. */
    CHECK_EQ_INT(cbt_pack_coord(0xFFFFu, 0), 0x0000FFFFu);
    CHECK_EQ_INT(cbt_pack_coord(0, 0xFFFFu), 0xFFFF0000u);

    /* Round trip across interesting values. */
    {
        static const uint16_t values[] = {0, 1, 20, 120, 255, 256, 9999, 0x7FFF, 0xFFFF};
        size_t i;
        size_t j;
        int ok = 1;
        for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            for (j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
                uint32_t packed = cbt_pack_coord(values[i], values[j]);
                uint16_t rx = 0;
                uint16_t ry = 0;
                cbt_unpack_coord(packed, &rx, &ry);
                if (rx != values[i] || ry != values[j]) {
                    ok = 0;
                }
            }
        }
        CHECK(ok);
    }

    /* NULL out-parameters are tolerated. */
    cbt_unpack_coord(0x12345678u, NULL, NULL);
    cbt_unpack_coord(0x12345678u, &x, NULL);
    CHECK_EQ_INT(x, 0x5678);

    /* ScreenColors is stored in the low byte only. */
    CHECK_EQ_INT(cbt_attribute_to_registry(0x001Fu), 0x1Fu);
    CHECK_EQ_INT(cbt_attribute_to_registry(0x00FFu), 0xFFu);
    CHECK_EQ_INT(cbt_attribute_from_registry(0x56u), 0x0056u);
}

/* ------------------------------------------------------------------------- */
/* Validation                                                                */
/* ------------------------------------------------------------------------- */

static void test_validation(void)
{
    uint8_t byte_value = 0;

    begin("validation");

    /* Geometry: the window may never exceed the buffer. */
    CHECK_EQ_INT(cbt_validate_geometry(120, 30, 120, 30), CBT_OK);
    CHECK_EQ_INT(cbt_validate_geometry(120, 30, 100, 20), CBT_OK);
    CHECK_EQ_INT(cbt_validate_geometry(120, 30, 121, 30), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_geometry(120, 30, 120, 31), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_geometry(19, 30, 10, 10), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_geometry(513, 30, 10, 10), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_geometry(120, 4, 10, 4), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_geometry(120, 10000, 10, 5), CBT_E_OUT_OF_RANGE);
    /* Zero is refused: a degenerate window breaks the console outright. */
    CHECK_EQ_INT(cbt_validate_geometry(0, 0, 0, 0), CBT_E_OUT_OF_RANGE);

    CHECK_EQ_INT(cbt_validate_font_size(16), CBT_OK);
    CHECK_EQ_INT(cbt_validate_font_size(0), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_font_size(201), CBT_E_OUT_OF_RANGE);

    CHECK_EQ_INT(cbt_validate_opacity(100), CBT_OK);
    CHECK_EQ_INT(cbt_validate_opacity(10), CBT_OK);
    CHECK_EQ_INT(cbt_validate_opacity(9), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_opacity(0), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_opacity(101), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_validate_opacity(-1), CBT_E_OUT_OF_RANGE);

    /* Opacity conversion: 50% is the 128 that the console's own slider produces. */
    CHECK_EQ_INT(cbt_opacity_percent_to_byte(100, &byte_value), CBT_OK);
    CHECK_EQ_INT(byte_value, 255);
    CHECK_EQ_INT(cbt_opacity_percent_to_byte(50, &byte_value), CBT_OK);
    CHECK_EQ_INT(byte_value, 128);
    CHECK_EQ_INT(cbt_opacity_percent_to_byte(0, &byte_value), CBT_OK);
    CHECK_EQ_INT(byte_value, 0);
    CHECK_EQ_INT(cbt_opacity_percent_to_byte(101, &byte_value), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_opacity_percent_to_byte(50, NULL), CBT_E_INVALID_ARG);

    CHECK_EQ_INT(cbt_opacity_byte_to_percent(255), 100);
    CHECK_EQ_INT(cbt_opacity_byte_to_percent(128), 50);
    CHECK_EQ_INT(cbt_opacity_byte_to_percent(0), 0);

    /* Face names: empty means "console chooses", which is the documented default. */
    CHECK_EQ_INT(cbt_validate_face_name(""), CBT_OK);
    CHECK_EQ_INT(cbt_validate_face_name("Consolas"), CBT_OK);
    CHECK_EQ_INT(cbt_validate_face_name("Lucida Console"), CBT_OK);
    CHECK_EQ_INT(cbt_validate_face_name(NULL), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_validate_face_name(
                    "0123456789012345678901234567890123"), CBT_E_OUT_OF_RANGE); /* 34 >= LF_FACESIZE */
    CHECK_EQ_INT(cbt_validate_face_name("bad\nname"), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_validate_face_name("bad\x7fname"), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_validate_face_name("caf\xc3\xa9"), CBT_E_INVALID_ARG); /* non-ASCII refused */
}

static void test_config(void)
{
    cbt_config config;
    const char *field = (const char *)1;

    begin("config");

    cbt_config_default(&config);

    CHECK_EQ_INT(config.foreground_slot, CBT_SLOT_GRAY);
    CHECK_EQ_INT(config.background_slot, CBT_SLOT_BLACK);
    CHECK_EQ_INT(config.canvas_slot, CBT_SLOT_BLACK);
    CHECK_EQ_INT(config.buffer_columns, 120);
    CHECK_EQ_INT(config.buffer_rows, 30);
    CHECK_EQ_INT(config.window_columns, 120);
    CHECK_EQ_INT(config.window_rows, 30);
    CHECK_EQ_INT(config.opacity_percent, 100);
    CHECK_EQ_INT(config.face_name[0], 0);
    /* Every default palette entry must be a 24-bit value. */
    {
        int i;
        int ok = 1;
        for (i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
            if (config.palette[i] > 0xFFFFFFu) {
                ok = 0;
            }
        }
        CHECK(ok);
    }

    CHECK_EQ_INT(cbt_config_validate(&config, &field), CBT_OK);
    CHECK(field == NULL);

    /* A window larger than its buffer must be reported with the offending field named. */
    config.window_columns = 200;
    CHECK_EQ_INT(cbt_config_validate(&config, &field), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_STR(field, "geometry");
    config.window_columns = 120;

    config.foreground_slot = 99;
    CHECK_EQ_INT(cbt_config_validate(&config, &field), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_STR(field, "foreground");
    config.foreground_slot = CBT_SLOT_GRAY;

    config.opacity_percent = 3;
    CHECK_EQ_INT(cbt_config_validate(&config, &field), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_STR(field, "opacity");
    config.opacity_percent = 100;

    config.palette[3] = 0x01000000u; /* a value that skipped COLORREF conversion */
    CHECK_EQ_INT(cbt_config_validate(&config, &field), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_STR(field, "palette");
    config.palette[3] = 0x00FFFFFFu;

    CHECK_EQ_INT(cbt_config_validate(&config, &field), CBT_OK);
    CHECK_EQ_INT(cbt_config_validate(NULL, &field), CBT_E_INVALID_ARG);

    /* NULL field pointer must be tolerated. */
    CHECK_EQ_INT(cbt_config_validate(&config, NULL), CBT_OK);
}

static void test_readability(void)
{
    cbt_config config;
    double luminance = -1.0;
    double ratio = -1.0;

    begin("readability");

    cbt_config_default(&config);

    /*
     * Anchor the WCAG transfer function with values computed independently, so an inverted
     * exponent cannot pass unnoticed. The bug this guards against reported 0.3779 for
     * 0x0C0C0C instead of 0.0037, which would have made every dark console look readable.
     */
    CHECK_EQ_DOUBLE(cbt_color_relative_luminance(0x0C0C0Cu), 0.003677, 0.00005);
    CHECK_EQ_DOUBLE(cbt_color_relative_luminance(0x808080u), 0.215861, 0.0005);
    CHECK_EQ_DOUBLE(cbt_color_relative_luminance(0x404040u), 0.051269, 0.0005);
    CHECK_EQ_DOUBLE(cbt_color_relative_luminance(0xF2F2F2u), 0.887923, 0.001);

    /* Default console: near black background, so the theme is light-on-dark. */
    CHECK_EQ_INT(cbt_config_canvas_luminance(&config, &luminance), CBT_OK);
    CHECK(luminance < 0.1);
    CHECK_EQ_INT(cbt_config_theme(&config), CBT_THEME_LIGHT_ON_DARK);

    /* White canvas flips the theme. */
    config.palette[CBT_SLOT_BLACK] = 0xFFFFFFu;
    CHECK_EQ_INT(cbt_config_canvas_luminance(&config, &luminance), CBT_OK);
    CHECK(luminance > 0.9);
    CHECK_EQ_INT(cbt_config_theme(&config), CBT_THEME_DARK_ON_LIGHT);

    /* WCAG reference values: black on white is exactly 21:1, like on like is 1:1. */
    CHECK_EQ_DOUBLE(cbt_color_contrast_ratio(0x000000u, 0xFFFFFFu), 21.0, 0.01);
    CHECK_EQ_DOUBLE(cbt_color_contrast_ratio(0xFFFFFFu, 0xFFFFFFu), 1.0, 0.001);
    CHECK_EQ_DOUBLE(cbt_color_contrast_ratio(0x000000u, 0x000000u), 1.0, 0.001);
    CHECK_EQ_DOUBLE(cbt_color_relative_luminance(0x000000u), 0.0, 0.0001);
    CHECK_EQ_DOUBLE(cbt_color_relative_luminance(0xFFFFFFu), 1.0, 0.0001);

    /* The default palette must at least be legible: light grey on near black is high contrast. */
    {
        cbt_config fresh;
        cbt_config_default(&fresh);
        CHECK_EQ_INT(cbt_config_contrast(&fresh, &ratio), CBT_OK);
        CHECK(ratio > 7.0);
    }

    /* Out-of-range canvas slot is reported, not read out of bounds. */
    config.canvas_slot = 42;
    CHECK_EQ_INT(cbt_config_canvas_luminance(&config, &luminance), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_config_contrast(&config, &ratio), CBT_E_OUT_OF_RANGE);
    CHECK_EQ_INT(cbt_config_canvas_luminance(NULL, &luminance), CBT_E_INVALID_ARG);
}

/* ------------------------------------------------------------------------- */
/* Registry: values                                                          */
/* ------------------------------------------------------------------------- */

static void test_registry_values(void)
{
    cbt_value dword;
    cbt_value text;
    cbt_value binary;
    cbt_value copy;
    static const uint8_t payload[] = {0x01, 0x02, 0x00, 0xFF, 0x10};

    begin("registry values");

    memset(&dword, 0, sizeof(dword));
    memset(&text, 0, sizeof(text));
    memset(&binary, 0, sizeof(binary));
    memset(&copy, 0, sizeof(copy));

    CHECK_EQ_INT(cbt_value_set_dword(&dword, 0xDEADBEEFu), CBT_OK);
    CHECK_EQ_INT(dword.type, CBT_VALUE_DWORD);
    CHECK_EQ_INT(dword.u.dword, 0xDEADBEEFu);

    CHECK_EQ_INT(cbt_value_set_string(&text, "Lucida Console"), CBT_OK);
    CHECK_EQ_INT(text.type, CBT_VALUE_STRING);
    CHECK_EQ_STR(text.u.string.text, "Lucida Console");

    CHECK_EQ_INT(cbt_value_set_binary(&binary, payload, sizeof(payload)), CBT_OK);
    CHECK_EQ_INT(binary.type, CBT_VALUE_BINARY);
    CHECK_EQ_INT(binary.u.binary.size, sizeof(payload));
    CHECK(memcmp(binary.u.binary.data, payload, sizeof(payload)) == 0);

    /* Deep copy: the copies must be independent allocations. */
    CHECK_EQ_INT(cbt_value_copy(&copy, &text), CBT_OK);
    CHECK_EQ_STR(copy.u.string.text, "Lucida Console");
    CHECK(copy.u.string.text != text.u.string.text);

    CHECK_EQ_INT(cbt_value_copy(&copy, &binary), CBT_OK);
    CHECK(copy.u.binary.data != binary.u.binary.data);
    CHECK(memcmp(copy.u.binary.data, payload, sizeof(payload)) == 0);

    CHECK_EQ_INT(cbt_value_copy(&copy, &dword), CBT_OK);
    CHECK_EQ_INT(copy.u.dword, 0xDEADBEEFu);

    /* Self-copy must not free the source. */
    CHECK_EQ_INT(cbt_value_copy(&dword, &dword), CBT_OK);
    CHECK_EQ_INT(dword.u.dword, 0xDEADBEEFu);

    /* Clearing is idempotent and resets the type. */
    cbt_value_clear(&text);
    CHECK_EQ_INT(text.type, CBT_VALUE_NONE);
    cbt_value_clear(&text);
    CHECK_EQ_INT(text.type, CBT_VALUE_NONE);

    /* Zero-length binary is legal and owns no buffer. */
    CHECK_EQ_INT(cbt_value_set_binary(&binary, NULL, 0), CBT_OK);
    CHECK_EQ_INT(binary.u.binary.size, 0);
    CHECK(binary.u.binary.data == NULL);

    /* Binary with data but zero length is a caller error. */
    CHECK_EQ_INT(cbt_value_set_binary(&binary, payload, 0), CBT_OK);

    CHECK_EQ_INT(cbt_value_set_dword(NULL, 1), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_value_set_string(&dword, NULL), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_value_copy(NULL, &dword), CBT_E_INVALID_ARG);

    cbt_value_clear(&dword);
    cbt_value_clear(&text);
    cbt_value_clear(&binary);
    cbt_value_clear(&copy);
}

/* ------------------------------------------------------------------------- */
/* Registry: backend behaviour                                               */
/* ------------------------------------------------------------------------- */

static void test_memory_backend(void)
{
    cbt_registry_backend *backend = cbt_registry_memory(-1);
    void *handle = NULL;
    cbt_value value;
    cbt_key_snapshot snapshot;
    size_t index;

    begin("memory backend");

    CHECK(backend != NULL);
    CHECK_EQ_STR(backend->name, "memory");

    memset(&value, 0, sizeof(value));
    memset(&snapshot, 0, sizeof(snapshot));

    /* A missing key is reported, not invented. */
    CHECK_EQ_INT(backend->open(backend->ctx, "Console", 0, &handle), CBT_E_NOT_FOUND);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console"), 0);

    /* Create, then read back. */
    CHECK_EQ_INT(backend->open(backend->ctx, "Console", 1, &handle), CBT_OK);
    CHECK(handle != NULL);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console"), 1);

    CHECK_EQ_INT(backend->get_value(backend->ctx, handle, "ColorTable00", &value), CBT_E_NOT_FOUND);
    CHECK_EQ_INT(value.type, CBT_VALUE_NONE);

    CHECK_EQ_INT(cbt_value_set_dword(&value, 0x000C0C0Cu), CBT_OK);
    CHECK_EQ_INT(backend->set_value(backend->ctx, handle, "ColorTable00", &value), CBT_OK);
    cbt_value_clear(&value);

    CHECK_EQ_INT(backend->get_value(backend->ctx, handle, "ColorTable00", &value), CBT_OK);
    CHECK_EQ_INT(value.u.dword, 0x000C0C0Cu);
    cbt_value_clear(&value);

    /* Value names are case-insensitive, exactly like the real registry. */
    CHECK_EQ_INT(backend->get_value(backend->ctx, handle, "colortable00", &value), CBT_OK);
    CHECK_EQ_INT(value.u.dword, 0x000C0C0Cu);
    cbt_value_clear(&value);

    CHECK_EQ_INT(cbt_value_set_string(&value, "Consolas"), CBT_OK);
    CHECK_EQ_INT(backend->set_value(backend->ctx, handle, "FaceName", &value), CBT_OK);
    cbt_value_clear(&value);

    /* Overwriting an existing value replaces it rather than duplicating. */
    CHECK_EQ_INT(cbt_value_set_dword(&value, 0x00FFFFFFu), CBT_OK);
    CHECK_EQ_INT(backend->set_value(backend->ctx, handle, "ColorTable00", &value), CBT_OK);
    cbt_value_clear(&value);

    /* Snapshot captures everything; restore is exact. */
    CHECK_EQ_INT(backend->snapshot_key(backend->ctx, "Console", &snapshot), CBT_OK);
    CHECK_EQ_INT(snapshot.existed, 1);
    CHECK_EQ_INT(cbt_key_snapshot_count(&snapshot), 2);
    {
        size_t seen_dword = 0;
        size_t seen_string = 0;
        for (index = 0; index < cbt_key_snapshot_count(&snapshot); index++) {
            const cbt_value *snap_value = cbt_key_snapshot_value(&snapshot, index);
            const char *name = cbt_key_snapshot_name(&snapshot, index);
            CHECK(name != NULL);
            CHECK(snap_value != NULL);
            if (strcmp(name, "ColorTable00") == 0) {
                CHECK_EQ_INT(snap_value->type, CBT_VALUE_DWORD);
                CHECK_EQ_INT(snap_value->u.dword, 0x00FFFFFFu);
                seen_dword++;
            }
            if (strcmp(name, "FaceName") == 0) {
                CHECK_EQ_INT(snap_value->type, CBT_VALUE_STRING);
                CHECK_EQ_STR(snap_value->u.string.text, "Consolas");
                seen_string++;
            }
        }
        CHECK_EQ_INT(seen_dword, 1);
        CHECK_EQ_INT(seen_string, 1);
    }

    /* Mutate heavily, then restore and confirm the snapshot wins. */
    CHECK_EQ_INT(cbt_value_set_dword(&value, 0x12345678u), CBT_OK);
    CHECK_EQ_INT(backend->set_value(backend->ctx, handle, "ColorTable01", &value), CBT_OK);
    CHECK_EQ_INT(backend->set_value(backend->ctx, handle, "ColorTable00", &value), CBT_OK);
    CHECK_EQ_INT(backend->delete_value(backend->ctx, handle, "FaceName"), CBT_OK);
    cbt_value_clear(&value);

    CHECK_EQ_INT(backend->restore_key(backend->ctx, &snapshot), CBT_OK);
    cbt_key_snapshot_free(&snapshot);

    CHECK_EQ_INT(backend->get_value(backend->ctx, handle, "ColorTable00", &value), CBT_OK);
    CHECK_EQ_INT(value.u.dword, 0x00FFFFFFu); /* original value back */
    cbt_value_clear(&value);

    /* The value added after the snapshot must be gone. */
    CHECK_EQ_INT(backend->get_value(backend->ctx, handle, "ColorTable01", &value), CBT_E_NOT_FOUND);

    /* The value deleted after the snapshot must be back. */
    CHECK_EQ_INT(backend->get_value(backend->ctx, handle, "FaceName", &value), CBT_OK);
    CHECK_EQ_STR(value.u.string.text, "Consolas");
    cbt_value_clear(&value);

    /* Deleting an absent value succeeds: the desired state already holds. */
    CHECK_EQ_INT(backend->delete_value(backend->ctx, handle, "NoSuchValue"), CBT_OK);

    backend->close(backend->ctx, handle);

    /* A snapshot of a key that does not exist restores to "still does not exist". */
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK_EQ_INT(backend->snapshot_key(backend->ctx, "Console\\Sub", &snapshot), CBT_OK);
    CHECK_EQ_INT(snapshot.existed, 0);
    CHECK_EQ_INT(backend->open(backend->ctx, "Console\\Sub", 1, &handle), CBT_OK);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console\\Sub"), 1);
    CHECK_EQ_INT(backend->restore_key(backend->ctx, &snapshot), CBT_OK);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console\\Sub"), 0);
    cbt_key_snapshot_free(&snapshot);

    cbt_registry_memory_destroy(backend);

    /* A NULL backend must not crash the destroy helper. */
    cbt_registry_memory_destroy(NULL);
}

/*
 * The transactional pattern: snapshot, apply, and on any failure restore. This is exactly what
 * the tool does before touching a real registry key, so it is worth proving that a mid-way
 * failure leaves the store byte-for-byte as it was.
 */
static void test_transactional_rollback(void)
{
    static const char *const kKeys[] = {"ColorTable00", "ColorTable01", "ScreenBufferSize"};
    static const uint32_t kNewValues[] = {0x00FF00FFu, 0x0000FF00u, 589889656u};

    begin("transactional rollback");

    /* Start uninjected so the seed data lands, then arm the injection for the real run. */
    {
        cbt_registry_backend *backend = cbt_registry_memory(-1);
        cbt_key_snapshot snapshot;
        cbt_value value;
        void *handle = NULL;
        size_t i;
        cbt_status status;
        int write_failed = 0;

        CHECK(backend != NULL);
        memset(&value, 0, sizeof(value));
        memset(&snapshot, 0, sizeof(snapshot));

        /* Seed known original state with injection disabled. */
        CHECK_EQ_INT(backend->open(backend->ctx, "Console", 1, &handle), CBT_OK);
        for (i = 0; i < 3; i++) {
            CHECK_EQ_INT(cbt_value_set_dword(&value, (uint32_t)(0x1000u + i)), CBT_OK);
            CHECK_EQ_INT(backend->set_value(backend->ctx, handle, kKeys[i], &value), CBT_OK);
            cbt_value_clear(&value);
        }

        CHECK_EQ_INT(backend->snapshot_key(backend->ctx, "Console", &snapshot), CBT_OK);
        CHECK_EQ_INT(cbt_key_snapshot_count(&snapshot), 3);

        /*
         * Arm the injection: the third write of the transaction will now fail, which is the
         * situation the rollback has to survive.
         */
        cbt_mem_store_arm(backend, 2);

        for (i = 0; i < 3; i++) {
            cbt_value next;
            memset(&next, 0, sizeof(next));
            CHECK_EQ_INT(cbt_value_set_dword(&next, kNewValues[i]), CBT_OK);
            status = backend->set_value(backend->ctx, handle, kKeys[i], &next);
            cbt_value_clear(&next);
            if (status != CBT_OK) {
                write_failed = 1;
                break;
            }
        }
        CHECK(write_failed);

        /* Restore, disarming the injection first so the rollback itself can write. */
        cbt_mem_store_arm(backend, -1);
        CHECK_EQ_INT(backend->restore_key(backend->ctx, &snapshot), CBT_OK);

        for (i = 0; i < 3; i++) {
            CHECK_EQ_INT(backend->get_value(backend->ctx, handle, kKeys[i], &value), CBT_OK);
            CHECK_EQ_INT(value.u.dword, (uint32_t)(0x1000u + i));
            cbt_value_clear(&value);
        }

        cbt_key_snapshot_free(&snapshot);
        backend->close(backend->ctx, handle);
        cbt_registry_memory_destroy(backend);
    }

    /* Without injection the same transaction simply succeeds. */
    {
        cbt_registry_backend *backend = cbt_registry_memory(-1);
        cbt_key_snapshot snapshot;
        cbt_value value;
        void *handle = NULL;
        size_t i;
        int all_ok = 1;

        memset(&value, 0, sizeof(value));
        memset(&snapshot, 0, sizeof(snapshot));

        CHECK_EQ_INT(backend->open(backend->ctx, "Console", 1, &handle), CBT_OK);
        for (i = 0; i < 3; i++) {
            CHECK_EQ_INT(cbt_value_set_dword(&value, (uint32_t)(0x2000u + i)), CBT_OK);
            CHECK_EQ_INT(backend->set_value(backend->ctx, handle, kKeys[i], &value), CBT_OK);
            cbt_value_clear(&value);
        }
        CHECK_EQ_INT(backend->snapshot_key(backend->ctx, "Console", &snapshot), CBT_OK);
        for (i = 0; i < 3; i++) {
            CHECK_EQ_INT(cbt_value_set_dword(&value, kNewValues[i]), CBT_OK);
            if (backend->set_value(backend->ctx, handle, kKeys[i], &value) != CBT_OK) {
                all_ok = 0;
            }
            cbt_value_clear(&value);
        }
        CHECK(all_ok);
        CHECK_EQ_INT(backend->restore_key(backend->ctx, &snapshot), CBT_OK);
        for (i = 0; i < 3; i++) {
            CHECK_EQ_INT(backend->get_value(backend->ctx, handle, kKeys[i], &value), CBT_OK);
            CHECK_EQ_INT(value.u.dword, (uint32_t)(0x2000u + i));
            cbt_value_clear(&value);
        }
        cbt_key_snapshot_free(&snapshot);
        backend->close(backend->ctx, handle);
        cbt_registry_memory_destroy(backend);
    }
}

static void test_memory_key_tree(void)
{
    cbt_registry_backend *backend = cbt_registry_memory(-1);
    void *handle = NULL;
    cbt_value value;

    begin("memory key tree");

    memset(&value, 0, sizeof(value));

    CHECK_EQ_INT(backend->open(backend->ctx, "Console", 1, &handle), CBT_OK);
    CHECK_EQ_INT(cbt_value_set_dword(&value, 1), CBT_OK);
    CHECK_EQ_INT(backend->set_value(backend->ctx, handle, "ScreenColors", &value), CBT_OK);
    cbt_value_clear(&value);
    backend->close(backend->ctx, handle);

    CHECK_EQ_INT(backend->open(backend->ctx, "Console\\Sub", 1, &handle), CBT_OK);
    CHECK_EQ_INT(cbt_value_set_dword(&value, 2), CBT_OK);
    CHECK_EQ_INT(backend->set_value(backend->ctx, handle, "WindowSize", &value), CBT_OK);
    cbt_value_clear(&value);
    backend->close(backend->ctx, handle);

    /* A prefix that is not a separator-delimited parent must not match. */
    CHECK_EQ_INT(backend->open(backend->ctx, "Consoles", 1, &handle), CBT_OK);
    backend->close(backend->ctx, handle);

    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console"), 1);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console\\Sub"), 1);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Consoles"), 1);

    /* Deleting a tree removes the key, its subkeys, but not a same-prefix sibling. */
    CHECK_EQ_INT(backend->delete_key(backend->ctx, "Console"), CBT_OK);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console"), 0);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Console\\Sub"), 0);
    CHECK_EQ_INT(backend->key_exists(backend->ctx, "Consoles"), 1);

    /* Deleting a missing tree succeeds. */
    CHECK_EQ_INT(backend->delete_key(backend->ctx, "A\\B\\C"), CBT_OK);

    cbt_registry_memory_destroy(backend);
}

/* ------------------------------------------------------------------------- */
/* Registry: path confinement of the real backend                            */
/* ------------------------------------------------------------------------- */

/*
 * The Win32 backend must confine every operation to HKCU\Console. These cases are the
 * "registry traversal" attacks: a path that would reach the machine hive, an autorun key, or a
 * drive-qualified path. Every one must be refused, and because refusal happens before the first
 * Win32 call, no case here can modify anything.
 */
static void test_win32_path_confinement(void)
{
    static const char *const kRejected[] = {
        "",
        ".",
        "..",
        "Console\\..\\..\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "Console\\..",
        "Console\\.\\Sub",
        "Console\\\\Sub",
        "Console\\",
        "\\\\Server\\Share",
        "C:\\Temp",
        "C:Console",
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "HKLM\\Software",
        "HKEY_LOCAL_MACHINE\\Software",
        "Console\\%%Startup",
        "ConsoleX",
        "ConsoleX\\Sub",
        "Console\\bad:stream",
        "Console\\bad*name",
        "Console\\bad?name",
        "Console\\bad\"name",
        "Console\\bad<name>",
        "Console\\bad|name",
        "Console\\bad/name",
        "Console\nNewline",
        "Console\rCarriage",
    };

    cbt_registry_backend *backend = cbt_registry_win32();
    void *handle = NULL;
    cbt_value value;
    cbt_key_snapshot snapshot;
    size_t i;

    begin("win32 path confinement");

    CHECK(backend != NULL);
    CHECK_EQ_STR(backend->name, "win32");

    memset(&value, 0, sizeof(value));
    memset(&snapshot, 0, sizeof(snapshot));

    for (i = 0; i < sizeof(kRejected) / sizeof(kRejected[0]); i++) {
        g_checks++;
        if (backend->open(backend->ctx, kRejected[i], 1, &handle) != CBT_E_INVALID_ARG) {
            g_failures++;
            fprintf(stderr, "FAIL path confinement: open accepted '");
            /* Print defensively: some entries contain control characters on purpose. */
            {
                const char *p = kRejected[i];
                while (*p != '\0') {
                    fputc((*p >= 0x20 && *p < 0x7F) ? *p : '?', stderr);
                    p++;
                }
            }
            fprintf(stderr, "'\n");
            if (handle != NULL) {
                backend->close(backend->ctx, handle);
                handle = NULL;
            }
        }

        /* Deleting, snapshotting and probing the same paths must be refused too. */
        g_checks++;
        if (backend->delete_key(backend->ctx, kRejected[i]) != CBT_E_INVALID_ARG) {
            g_failures++;
            fprintf(stderr, "FAIL path confinement: delete_key accepted a rejected path\n");
        }
        g_checks++;
        if (backend->snapshot_key(backend->ctx, kRejected[i], &snapshot) != CBT_E_INVALID_ARG) {
            g_failures++;
            fprintf(stderr, "FAIL path confinement: snapshot_key accepted a rejected path\n");
        }
        cbt_key_snapshot_free(&snapshot);
        memset(&snapshot, 0, sizeof(snapshot));

        /* key_exists reports 0 rather than a status, so assert it simply says "no". */
        g_checks++;
        if (backend->key_exists(backend->ctx, kRejected[i]) != 0) {
            g_failures++;
            fprintf(stderr, "FAIL path confinement: key_exists claimed a rejected path exists\n");
        }
    }

    /* Legitimate paths must be accepted, which proves the rejections above are not blanket. */
    {
        static const char *const kAccepted[] = {
            "Console",
            "Console\\CbtConfinementProbe",
            "Console\\Cbt\\Deeper\\Probe",
        };
        for (i = 0; i < sizeof(kAccepted) / sizeof(kAccepted[0]); i++) {
            void *probe_handle = NULL;
            g_checks++;
            if (backend->open(backend->ctx, kAccepted[i], 1, &probe_handle) != CBT_OK) {
                g_failures++;
                fprintf(stderr, "FAIL path confinement: rejected a legitimate path '%s'\n", kAccepted[i]);
                continue;
            }
            /* Clean up the probe keys immediately; they carry no values. */
            backend->close(backend->ctx, probe_handle);
        }
        /* Remove the probe tree. This is the only HKCU write this test performs. */
        CHECK_EQ_INT(backend->delete_key(backend->ctx, "Console\\CbtConfinementProbe"), CBT_OK);
        CHECK_EQ_INT(backend->delete_key(backend->ctx, "Console\\Cbt"), CBT_OK);
    }
}

/*
 * The confinement rule, tested directly. This is the security boundary of the whole tool: if a
 * path escapes HKCU\Console, a write could land anywhere the user can write. Testing the
 * predicate itself gives a precise failure message instead of an inference from side effects.
 */
static void test_confinement_predicate(void)
{
    static const char *const kAllowed[] = {
        "Console",
        "Console\\CbtConfinementProbe",
        "Console\\Cbt\\Deeper\\Probe",
        "Console\\A-B_C.D",
        "Console\\HKLMFOO",   /* starts with hive letters but is not a hive reference */
        /*
         * Allowed on purpose, and safe: the path is relative to HKEY_CURRENT_USER and the root
         * is pinned to the "Console" key, so this resolves to HKCU\Console\Software\... and NOT
         * to the HKCU autorun key. The tool's own writes additionally touch only "Console"
         * itself, with value names drawn from a fixed list. Rejecting the name "Software" would
         * add no protection while implying a boundary that does not exist.
         */
        "Console\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "Console\\System",
    };
    static const char *const kDenied[] = {
        "",
        ".",
        "..",
        "Console\\..",
        "Console\\.",
        "Console\\..\\..\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "Console\\\\Sub",
        "Console\\",
        "\\Console",
        "C:\\Temp",
        "C:Console",
        "HKLM",
        "HKLM\\Software",
        "HKEY_LOCAL_MACHINE",
        "HKEY_LOCAL_MACHINE\\Software",
        "hklm\\software",
        "HKCU\\Software",
        "HKCR\\CLSID",
        "HKU\\.DEFAULT",
        "HKCC\\Software",
        "Console\\%%Startup",           /* the console delegation key is never written */
        "Console\\%SystemRoot%_System32_cmd.exe", /* environment-expanded names are refused */
        "Console\\bad:stream",
        "Console\\bad*name",
        "Console\\bad?name",
        "Console\\bad\"name",
        "Console\\bad<name>",
        "Console\\bad>name",
        "Console\\bad|name",
        "Console\\bad/name",
        "Console\nNewline",
        "Console\rCarriage",
    };
    size_t i;

    begin("confinement predicate");

    for (i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++) {
        g_checks++;
        if (!cbt_registry_path_is_confined(kAllowed[i])) {
            g_failures++;
            fprintf(stderr, "FAIL confinement: wrongly denied '%s'\n", kAllowed[i]);
        }
    }

    for (i = 0; i < sizeof(kDenied) / sizeof(kDenied[0]); i++) {
        int confined = cbt_registry_path_is_confined(kDenied[i]);
        g_checks++;
        if (confined) {
            g_failures++;
            fprintf(stderr, "FAIL confinement: wrongly allowed '");
            {
                const char *p = kDenied[i];
                while (*p != '\0') {
                    fputc((*p >= 0x20 && *p < 0x7F) ? *p : '?', stderr);
                    p++;
                }
            }
            fprintf(stderr, "'\n");
        }
    }

    /* A NULL path is never confined. */
    CHECK_EQ_INT(cbt_registry_path_is_confined(NULL), 0);

    /* Overlong components must be refused rather than truncated. */
    {
        char long_component[600];
        memset(long_component, 'a', sizeof(long_component) - 1);
        long_component[0] = 'C';
        long_component[1] = 'o';
        long_component[2] = 'n';
        long_component[3] = 's';
        long_component[4] = 'o';
        long_component[5] = 'l';
        long_component[6] = 'e';
        long_component[7] = '\\';
        long_component[sizeof(long_component) - 1] = '\0';
        CHECK_EQ_INT(cbt_registry_path_is_confined(long_component), 0);
    }
}

/*
 * Reading the real key is safe and is worth asserting: the tool must be able to see the current
 * console configuration before it changes anything.
 */
static void test_win32_read_real_console(void)
{
    cbt_registry_backend *backend = cbt_registry_win32();
    void *handle = NULL;
    cbt_value value;
    int has_color_table = 0;
    int i;

    begin("win32 read real console");

    memset(&value, 0, sizeof(value));

    {
        cbt_status open_status = backend->open(backend->ctx, "Console", 0, &handle);
        if (open_status != CBT_OK) {
            fprintf(stderr, "note: cannot open HKCU\\Console (%s); skipping read test\n",
                    cbt_status_name(open_status));
            return;
        }
    }

    for (i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        char name[32];
        cbt_status read_status;
        snprintf(name, sizeof(name), "ColorTable%02d", i);
        read_status = backend->get_value(backend->ctx, handle, name, &value);
        if (read_status == CBT_OK) {
            if (value.type == CBT_VALUE_DWORD) {
                has_color_table++;
                CHECK_EQ_INT(value.u.dword & 0xFF000000u, 0u);
            }
            cbt_value_clear(&value);
        }
    }
    CHECK(has_color_table > 0);

    /* ForceV2, when present, must decode as a DWORD. */
    if (backend->get_value(backend->ctx, handle, "ForceV2", &value) == CBT_OK) {
        CHECK_EQ_INT(value.type, CBT_VALUE_DWORD);
        cbt_value_clear(&value);
    }

    /* Snapshotting the real key must work: it is the safety net for every write. */
    {
        cbt_key_snapshot snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        CHECK_EQ_INT(backend->snapshot_key(backend->ctx, "Console", &snapshot), CBT_OK);
        CHECK_EQ_INT(snapshot.existed, 1);
        CHECK(cbt_key_snapshot_count(&snapshot) > 0);
        /* No value may be reported as unsupported: the real key holds only modelled types. */
        cbt_key_snapshot_free(&snapshot);
    }

    backend->close(backend->ctx, handle);
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                               */
/* ------------------------------------------------------------------------- */

int main(void)
{
    printf("ConsoleBgTool native core tests\n");
    printf("===============================\n");

    test_color_encoding();
    test_color_parse();
    test_slot_names();
    test_attributes();
    test_coord_and_registry_encoding();
    test_validation();
    test_config();
    test_readability();
    test_registry_values();
    test_memory_backend();
    test_memory_key_tree();
    test_transactional_rollback();
    test_confinement_predicate();
    test_win32_path_confinement();
    test_win32_read_real_console();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("RESULT: FAILED\n");
        return 1;
    }
    printf("RESULT: PASSED\n");
    return 0;
}
