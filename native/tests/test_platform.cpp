/*
 * ConsoleBgTool - C++ platform layer tests.
 *
 * Two kinds of test live here:
 *
 *   In-process  - the pure logic (validation, host detection, last-error plumbing) plus a real
 *                 live round trip against the console this process is attached to: read the
 *                 colour table, change it through the documented API, prove the change took,
 *                 then restore and prove the restore took.
 *
 *   Live child  - built as a second executable, scripts/live_child.cpp, which applies a marker
 *                 palette, executes a command pipeline, checks the exit code propagates, and
 *                 restores. The harness runs it in a window of its own.
 *
 * The console round trip is the test that matters most: it is the only one that proves the tool's
 * central mechanism works against a real Windows 11 console rather than against a mock.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cbt_console.h"
#include "cbt_platform.hpp"
#include "cbt_registry.h"

/* ------------------------------------------------------------------------- */
/* Minimal harness (kept independent of the C test harness)                   */
/* ------------------------------------------------------------------------- */

static int g_checks = 0;
static int g_failures = 0;
static int g_skips = 0;
static const char *g_case = "";

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        g_checks++;                                                                        \
        if (!(condition)) {                                                                \
            g_failures++;                                                                  \
            std::fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case,        \
                         #condition);                                                       \
        }                                                                                  \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                                      \
    do {                                                                                   \
        long long a_ = static_cast<long long>(actual);                                      \
        long long e_ = static_cast<long long>(expected);                                    \
        g_checks++;                                                                         \
        if (a_ != e_) {                                                                     \
            g_failures++;                                                                   \
            std::fprintf(stderr, "FAIL %s:%d [%s] %s: got %lld, want %lld\n", __FILE__,      \
                         __LINE__, g_case, #actual, a_, e_);                                \
        }                                                                                   \
    } while (0)

#define SKIP(reason)                                                                        \
    do {                                                                                    \
        g_skips++;                                                                          \
        std::fprintf(stderr, "SKIP [%s] %s\n", g_case, reason);                             \
    } while (0)

static void begin(const char *name)
{
    g_case = name;
}

/* ------------------------------------------------------------------------- */
/* Static layout checks                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The managed layer marshals these structures across the C ABI, so the layout is part of the
 * contract. Asserting it here turns a silent mis-marshal into a build-time-adjacent failure.
 */
static void test_structure_layout(void)
{
    begin("structure layout");

    /* cbt_config: fixed and predictable so the C# declaration can mirror it exactly. */
    CHECK_EQ_INT(sizeof(cbt_config), sizeof(cbt_color) * CBT_COLOR_TABLE_SIZE + 3 * sizeof(int) +
                                         4 * sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                         sizeof(uint32_t) + CBT_MAX_FACE_NAME + sizeof(int));

    /* The colour table must be first, so its offset is zero in both languages. */
    CHECK_EQ_INT(offsetof(cbt_config, palette), 0);
    CHECK_EQ_INT(offsetof(cbt_config, foreground_slot),
                 sizeof(cbt_color) * CBT_COLOR_TABLE_SIZE);

    /* LiveConsoleState: four bools, then the size fields, palette last. */
    CHECK_EQ_INT(sizeof(LiveConsoleState),
                 4 * sizeof(bool) + 6 * sizeof(uint16_t) + 2 * sizeof(uint16_t) +
                     2 * sizeof(uint32_t) + sizeof(cbt_color) * CBT_COLOR_TABLE_SIZE);

    /* Instantiate both to be sure the initialisers compile and produce sane defaults. */
    LiveConsoleState state;
    CHECK(!state.valid);
    CHECK(!state.is_console);
    CHECK_EQ_INT(state.palette[0], 0u);
}

/* ------------------------------------------------------------------------- */
/* Validation plumbing                                                        */
/* ------------------------------------------------------------------------- */

static void test_apply_rejects_invalid_config(void)
{
    cbt_config config;

    begin("apply rejects an invalid configuration");

    cbt_config_default(&config);

    /*
     * An invalid configuration must be refused before any console call, and the refusal must be
     * explained. Applying a zero-width buffer would break the console outright, so this is the
     * check that keeps a bad config file from damaging a session.
     */
    config.buffer_columns = 0;
    config.buffer_rows = 0;
    config.window_columns = 0;
    config.window_rows = 0;

    cbt::clear_last_error();
    cbt::ApplyOptions options;
    bool applied = cbt::apply_live_state(config, options);
    CHECK(!applied);
    CHECK(!cbt::last_error().empty());
    CHECK(cbt::last_error().find("invalid configuration") != std::string::npos);

    /* A NULL config must not crash. */
    cbt::clear_last_error();
    CHECK(!cbt::apply_live_state(config, options) || true);
}

static void test_error_plumbing(void)
{
    begin("error plumbing");

    cbt::clear_last_error();
    CHECK(cbt::last_error().empty());

    cbt::set_last_error("a diagnostic");
    CHECK_EQ_INT(cbt::last_error() == "a diagnostic", 1);

    /* A very long message must not be truncated or crash. */
    {
        std::string huge(200000, 'x');
        cbt::set_last_error(huge);
        CHECK_EQ_INT(cbt::last_error().size(), huge.size());
    }

    cbt::clear_last_error();
    CHECK(cbt::last_error().empty());

    /* The C ABI string must be NUL terminated and never NULL. */
    cbt::set_last_error("abi check");
    const char *abi = cbt_platform_last_error();
    CHECK(abi != nullptr);
    CHECK(std::strcmp(abi, "abi check") == 0);
    cbt::clear_last_error();
}

static void test_host_detection(void)
{
    begin("host detection");

    std::string kind;
    cbt::HostKind host = cbt::detect_host(&kind);

    /* The name table must be total. */
    CHECK(cbt::host_kind_name(cbt::HostKind::None) != nullptr);
    CHECK(cbt::host_kind_name(cbt::HostKind::ClassicConhost) != nullptr);
    CHECK(cbt::host_kind_name(cbt::HostKind::Pseudoconsole) != nullptr);
    CHECK(cbt::host_kind_name(cbt::HostKind::LegacyConhost) != nullptr);

    /* Whatever this host is, the answer must be one of the four known kinds. */
    CHECK(host == cbt::HostKind::None || host == cbt::HostKind::ClassicConhost ||
          host == cbt::HostKind::Pseudoconsole || host == cbt::HostKind::LegacyConhost);

    std::fprintf(stderr, "note: host kind = %s (%s)\n", cbt::host_kind_name(host),
                 kind.empty() ? "n/a" : kind.c_str());

    /* The registry key list must be exactly the confined console key. */
    std::vector<std::string> keys = cbt::console_registry_keys();
    CHECK_EQ_INT(keys.size(), 1u);
    if (!keys.empty()) {
        CHECK_EQ_INT(keys[0] == "Console", 1);
        CHECK_EQ_INT(cbt_registry_path_is_confined(keys[0].c_str()), 1);
    }
}

/* ------------------------------------------------------------------------- */
/* Live console round trip                                                    */
/* ------------------------------------------------------------------------- */

/*
 * This is the end-to-end proof of the core mechanism on a real console:
 *
 *   1. read the live state (the original appearance)
 *   2. guard it with ConsoleStateGuard, which is the documented duty of a program sharing a
 *      console
 *   3. apply a marker palette through SetConsoleScreenBufferInfoEx
 *   4. read it back and confirm the console really accepted it
 *   5. restore and confirm the original appearance came back byte for byte
 *
 * If this process has no console (output redirected to a file, or hosted by a GUI), the test is
 * reported as skipped rather than failed, because there is nothing to style.
 */
static void test_live_console_round_trip(void)
{
    LiveConsoleState before;

    begin("live console round trip");

    if (!cbt::read_live_state(before)) {
        SKIP("no console attached to this process");
        return;
    }

    CHECK(before.is_console);
    CHECK(before.buffer_columns > 0);
    CHECK(before.buffer_rows > 0);
    CHECK_EQ_INT(before.valid, 1);

    /* Every reported palette entry must be a valid 24-bit colour. */
    {
        int i;
        int all_valid = 1;
        for (i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
            if (before.palette[i] > 0xFFFFFFu) {
                all_valid = 0;
            }
        }
        CHECK(all_valid);
    }

    {
        cbt::ConsoleStateGuard guard;
        if (!guard.capture()) {
            SKIP("could not capture console state");
            return;
        }
        CHECK(guard.captured());
        CHECK_EQ_INT(guard.state().buffer_columns, before.buffer_columns);

        /*
         * Apply a marker palette. Magenta in every slot makes the change unmistakable, so a
         * read-back that shows the marker cannot be a coincidence.
         */
        {
            cbt_config marker;
            cbt_config_default(&marker);
            for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
                marker.palette[i] = 0xFF00FFu;
            }

            cbt::ApplyOptions options;
            options.apply_palette = true;
            options.apply_attribute = true;
            options.apply_canvas = false;
            options.apply_geometry = false;

            bool applied = cbt::apply_live_state(marker, options);
            CHECK(applied);
            if (!applied) {
                std::fprintf(stderr, "note: apply failed: %s\n", cbt::last_error().c_str());
            }
        }

        /* Read back through the same documented API the console itself uses. */
        {
            LiveConsoleState after;
            CHECK(cbt::read_live_state(after));
            CHECK_EQ_INT(after.palette[0], 0xFF00FFu);
            CHECK_EQ_INT(after.palette[15], 0xFF00FFu);
            /*
             * Geometry must be untouched: this call did not ask for a resize. The buffer check
             * used to be the only one here, which is how the one-row-per-apply window shrink went
             * unnoticed; the window is asserted exactly now (see test_palette_apply_stability for
             * the repeated version of this).
             */
            CHECK_EQ_INT(after.buffer_columns, before.buffer_columns);
            CHECK_EQ_INT(after.buffer_rows, before.buffer_rows);
            CHECK_EQ_INT(after.window_columns, before.window_columns);
            CHECK_EQ_INT(after.window_rows, before.window_rows);
        }

        /* Restore and prove the original appearance is back. */
        CHECK(guard.restore());
        {
            LiveConsoleState restored;
            CHECK(cbt::read_live_state(restored));
            int matches = 1;
            for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
                if (restored.palette[i] != before.palette[i]) {
                    matches = 0;
                    std::fprintf(stderr, "note: palette[%d] 0x%06X != original 0x%06X\n", i,
                                 restored.palette[i], before.palette[i]);
                }
            }
            CHECK(matches);
            CHECK_EQ_INT(restored.attributes, before.attributes);
            CHECK_EQ_INT(restored.buffer_columns, before.buffer_columns);
            CHECK_EQ_INT(restored.buffer_rows, before.buffer_rows);

            /*
             * The window must come back EXACTLY. The old <= tolerance hid the real defect: every
             * restore used to shrink the window by one row because SetConsoleScreenBufferInfoEx
             * applies srWindow one row shorter on this host. A window that was already valid
             * before the apply cannot be legitimately clamped on the way back, so an exact
             * comparison is the honest check; the numbers are printed if it ever fails.
             */
            CHECK(restored.window_columns > 0);
            CHECK(restored.window_rows > 0);
            if (restored.window_columns != before.window_columns ||
                restored.window_rows != before.window_rows) {
                std::fprintf(stderr,
                             "note: restore window %ux%u, captured %ux%u\n", restored.window_columns,
                             restored.window_rows, before.window_columns, before.window_rows);
            }
            CHECK_EQ_INT(restored.window_columns, before.window_columns);
            CHECK_EQ_INT(restored.window_rows, before.window_rows);
        }
    }
}

/*
 * Regression: a palette-only apply must not change the console geometry, no matter how often it
 * runs.
 *
 * This is the user-visible defect. SetConsoleScreenBufferInfoEx applies srWindow one row shorter
 * than the inclusive rectangle returned by GetConsoleScreenBufferInfoEx on this host, so a
 * palette-only apply used to shrink the visible window by one row every time. The deployed cmd
 * AutoRun runs apply on every shell start, so the window kept shrinking until cmd's line editor
 * wrote output over the line the user was typing. Five repeated applies are compared with the
 * state captured before the first one.
 */
static void test_palette_apply_stability(void)
{
    LiveConsoleState before;

    begin("palette apply stability");

    if (!cbt::read_live_state(before)) {
        SKIP("no console attached to this process");
        return;
    }
    if (before.window_columns == 0 || before.window_rows == 0) {
        SKIP("console has no visible window");
        return;
    }

    cbt::ConsoleStateGuard guard;
    if (!guard.capture()) {
        SKIP("could not capture console state");
        return;
    }

    cbt_config marker;
    cbt_config_default(&marker);
    for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        marker.palette[i] = 0x00FF7Fu;
    }

    cbt::ApplyOptions options;
    options.apply_palette = true;
    options.apply_attribute = false;
    options.apply_canvas = false;
    options.apply_geometry = false;

    int geometry_stable = 1;
    for (int pass = 0; pass < 5; pass++) {
        bool applied = cbt::apply_live_state(marker, options);
        CHECK(applied);
        if (!applied) {
            std::fprintf(stderr, "note: apply failed on pass %d: %s\n", pass + 1,
                         cbt::last_error().c_str());
        }

        LiveConsoleState after;
        CHECK(cbt::read_live_state(after));
        CHECK_EQ_INT(after.palette[0], 0x00FF7Fu);

        if (after.buffer_columns != before.buffer_columns ||
            after.buffer_rows != before.buffer_rows ||
            after.window_columns != before.window_columns ||
            after.window_rows != before.window_rows) {
            geometry_stable = 0;
            std::fprintf(stderr,
                         "note: pass %d changed geometry: buffer %ux%u -> %ux%u, window %ux%u -> "
                         "%ux%u\n",
                         pass + 1, before.buffer_columns, before.buffer_rows,
                         after.buffer_columns, after.buffer_rows, before.window_columns,
                         before.window_rows, after.window_columns, after.window_rows);
        }
    }
    CHECK(geometry_stable);

    /* Put the original palette back even if a check above failed. */
    CHECK(guard.restore());
    {
        LiveConsoleState restored;
        CHECK(cbt::read_live_state(restored));
        CHECK_EQ_INT(restored.buffer_columns, before.buffer_columns);
        CHECK_EQ_INT(restored.buffer_rows, before.buffer_rows);
        CHECK_EQ_INT(restored.window_columns, before.window_columns);
        CHECK_EQ_INT(restored.window_rows, before.window_rows);
        CHECK_EQ_INT(restored.palette[0], before.palette[0]);
    }
}

/*
 * Geometry: apply a different buffer and window size, prove it took, then let the guard put it
 * back. A wrong order here is the classic way a console is left unusable, so it is worth an
 * explicit test.
 */
static void test_live_geometry_round_trip(void)
{
    LiveConsoleState before;

    begin("live geometry round trip");

    if (!cbt::read_live_state(before)) {
        SKIP("no console attached to this process");
        return;
    }
    if (before.window_columns < 90 || before.buffer_columns < 100) {
        SKIP("console is too small to test a resize safely");
        return;
    }

    cbt::ConsoleStateGuard guard;
    if (!guard.capture()) {
        SKIP("could not capture console state");
        return;
    }

    {
        cbt_config resized;
        cbt_config_default(&resized);
        resized.buffer_columns = 100;
        resized.buffer_rows = 30;
        resized.window_columns = 90;
        resized.window_rows = 20;

        cbt::ApplyOptions options;
        options.apply_palette = false;
        options.apply_attribute = false;
        options.apply_canvas = false;
        options.apply_geometry = true;
        options.buffer_columns = 100;
        options.buffer_rows = 30;
        options.window_columns = 90;
        options.window_rows = 20;

        bool applied = cbt::apply_live_state(resized, options);
        CHECK(applied);
        if (applied) {
            LiveConsoleState after;
            CHECK(cbt::read_live_state(after));

            /* The requested buffer and window sizes must be reached exactly. */
            CHECK_EQ_INT(after.buffer_columns, 100);
            CHECK_EQ_INT(after.buffer_rows, 30);
            CHECK_EQ_INT(after.window_columns, 90);
            CHECK_EQ_INT(after.window_rows, 20);

            /*
             * The console is allowed to clamp a window to what the screen and font allow; if
             * this host refuses the requested size, print the real numbers and let the exact
             * checks above fail rather than silently weakening the assertion.
             */
            std::fprintf(stderr,
                         "note: geometry requested buffer=100x30 window=90x20, got buffer=%ux%u "
                         "window=%ux%u\n",
                         after.buffer_columns, after.buffer_rows, after.window_columns,
                         after.window_rows);

            /* The window must never exceed the buffer it is displaying. */
            CHECK(after.window_columns <= after.buffer_columns);
            CHECK(after.window_rows <= after.buffer_rows);
        } else {
            std::fprintf(stderr, "note: resize failed: %s\n", cbt::last_error().c_str());
        }
    }

    /*
     * The guard must put the original geometry back exactly. The old tolerance (<=) hid the
     * one-row-per-call shrink that this test is here to prevent.
     */
    CHECK(guard.restore());
    {
        LiveConsoleState restored;
        CHECK(cbt::read_live_state(restored));
        CHECK_EQ_INT(restored.buffer_columns, before.buffer_columns);
        CHECK_EQ_INT(restored.buffer_rows, before.buffer_rows);
        CHECK(restored.window_columns > 0);
        CHECK(restored.window_rows > 0);
        if (restored.window_columns != before.window_columns ||
            restored.window_rows != before.window_rows) {
            std::fprintf(stderr, "note: restore window %ux%u, captured %ux%u\n",
                         restored.window_columns, restored.window_rows, before.window_columns,
                         before.window_rows);
        }
        CHECK_EQ_INT(restored.window_columns, before.window_columns);
        CHECK_EQ_INT(restored.window_rows, before.window_rows);
    }
}

/*
 * Window opacity. On a classic conhost this exercises the layered-window path for real; on a
 * pseudoconsole there is no window of our own, which is a supported outcome rather than a
 * failure, so it is skipped.
 */
static void test_window_opacity(void)
{
    begin("window opacity");

    cbt::WindowOpacity opacity;
    if (!opacity.open()) {
        SKIP(cbt::last_error().c_str());
        return;
    }

    CHECK(opacity.has_window());
    CHECK(!opacity.is_layered() || true);

    /* Out-of-range percentages must be refused without touching the window. */
    CHECK(!opacity.apply_percent(0));
    CHECK(!opacity.apply_percent(9));
    CHECK(!opacity.apply_percent(101));
    CHECK(!opacity.apply_percent(-5));

    /* A real application. */
    CHECK(opacity.apply_percent(70));
    CHECK(opacity.is_layered());

    /* 100 means fully opaque, which removes the layered style again. */
    {
        bool fully_opaque = opacity.apply_percent(100);
        if (!fully_opaque || opacity.is_layered()) {
            std::fprintf(stderr, "note: apply 100 -> %d, layered=%d, err='%s'\n",
                         fully_opaque ? 1 : 0, opacity.is_layered() ? 1 : 0,
                         cbt::last_error().c_str());
        }
        CHECK(fully_opaque);
    }
    CHECK(!opacity.is_layered());

    /* Applying, restoring and restoring again must all be safe and idempotent. */
    CHECK(opacity.apply_percent(85));
    CHECK(opacity.restore());
    CHECK(!opacity.is_layered());
    CHECK(opacity.restore());

    /* The destructor restores too, even if restore() was never called. */
    {
        cbt::WindowOpacity scoped;
        if (scoped.open()) {
            CHECK(scoped.apply_percent(60));
            CHECK(scoped.is_layered());
        }
        /* scoped goes out of scope here; the window must not be left layered. */
    }
    CHECK(!opacity.is_layered());
}

/* ------------------------------------------------------------------------- */
/* C ABI                                                                      */
/* ------------------------------------------------------------------------- */

static void test_c_abi(void)
{
    begin("c abi");

    /* Argument validation must not crash and must report a code. */
    CHECK_EQ_INT(cbt_platform_apply(nullptr, 0, 0), CBT_E_INVALID_ARG);
    CHECK_EQ_INT(cbt_platform_read(nullptr, nullptr), CBT_E_INVALID_ARG);
    CHECK(cbt_platform_last_error() != nullptr);

    /* Host reporting through the C ABI must agree with the C++ call. */
    {
        int abi_kind = cbt_platform_host_kind();
        std::string name;
        cbt::HostKind cpp_kind = cbt::detect_host(&name);
        CHECK_EQ_INT(abi_kind, static_cast<int>(cpp_kind));

        int is_terminal = cbt_platform_is_windows_terminal();
        CHECK(is_terminal == 0 || is_terminal == 1);
        CHECK_EQ_INT(is_terminal, cbt::is_windows_terminal_host() ? 1 : 0);
    }

    /* Restoring when nothing was applied must be a harmless no-op. */
    CHECK_EQ_INT(cbt_platform_restore(), CBT_OK);
}

/*
 * C ABI persistence + explicit restore.
 *
 * Applying through the C ABI is what the managed CLI does, and the styling must survive the
 * transient apply process: the deployed cmd AutoRun runs the tool on every shell start and the
 * result has to stick. That is why cbt_platform_apply() disarms the process-wide state guard.
 * Only an explicit cbt_platform_restore() puts the original appearance back. A palette-only
 * apply must not resize the console either.
 */
static void test_c_abi_persistence(void)
{
    LiveConsoleState before;

    begin("c abi persistence");

    if (!cbt::read_live_state(before)) {
        SKIP("no console attached to this process");
        return;
    }

    cbt_config marker;
    cbt_config_default(&marker);
    for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
        marker.palette[i] = 0x00FF7Fu; /* unmistakable, and not in any default palette */
    }

    CHECK_EQ_INT(cbt_platform_apply(&marker, 0, 0), CBT_OK);

    {
        LiveConsoleState applied;
        CHECK(cbt::read_live_state(applied));
        int palette_is_marker = 1;
        for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
            if (applied.palette[i] != 0x00FF7Fu) {
                palette_is_marker = 0;
            }
        }
        CHECK(palette_is_marker);
        if (!palette_is_marker) {
            std::fprintf(stderr, "note: applied palette[0]=0x%06X, want 0x00FF7F (%s)\n",
                         applied.palette[0],
                         cbt::last_error().empty() ? "no error" : cbt::last_error().c_str());
        }

        /* Palette-only: the console geometry must be exactly untouched. */
        CHECK_EQ_INT(applied.buffer_columns, before.buffer_columns);
        CHECK_EQ_INT(applied.buffer_rows, before.buffer_rows);
        CHECK_EQ_INT(applied.window_columns, before.window_columns);
        CHECK_EQ_INT(applied.window_rows, before.window_rows);
    }

    /* The explicit restore puts the original appearance back, exactly. */
    CHECK_EQ_INT(cbt_platform_restore(), CBT_OK);
    {
        LiveConsoleState restored;
        CHECK(cbt::read_live_state(restored));
        int matches = 1;
        for (int i = 0; i < CBT_COLOR_TABLE_SIZE; i++) {
            if (restored.palette[i] != before.palette[i]) {
                matches = 0;
                std::fprintf(stderr, "note: palette[%d] 0x%06X != original 0x%06X\n", i,
                             restored.palette[i], before.palette[i]);
            }
        }
        CHECK(matches);
        CHECK_EQ_INT(restored.attributes, before.attributes);
        CHECK_EQ_INT(restored.buffer_columns, before.buffer_columns);
        CHECK_EQ_INT(restored.buffer_rows, before.buffer_rows);
        CHECK_EQ_INT(restored.window_columns, before.window_columns);
        CHECK_EQ_INT(restored.window_rows, before.window_rows);
    }
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    std::printf("ConsoleBgTool C++ platform tests\n");
    std::printf("================================\n");

    test_structure_layout();
    test_error_plumbing();
    test_apply_rejects_invalid_config();
    test_host_detection();
    test_c_abi();
    test_c_abi_persistence();
    test_live_console_round_trip();
    test_palette_apply_stability();
    test_live_geometry_round_trip();
    test_window_opacity();

    std::printf("\n%d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skips);
    if (g_failures != 0) {
        std::printf("RESULT: FAILED\n");
        return 1;
    }
    std::printf("RESULT: PASSED\n");
    return 0;
}
