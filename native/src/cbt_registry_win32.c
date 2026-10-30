/*
 * ConsoleBgTool - native core (C11)
 * cbt_registry_win32.c - the real registry backend.
 *
 * Security posture
 * ----------------
 * 1. Every key is opened relative to HKEY_CURRENT_USER. The machine-wide hive is never
 *    reachable: an absolute path such as "HKLM\\Software\\..." is refused before any Win32
 *    call is made.
 * 2. Every path is validated component by component. Empty components, ".", "..", drive
 *    qualifiers, alternate data streams and embedded NULs are rejected, so no crafted
 *    configuration value can redirect a write outside HKCU\Console ("registry traversal").
 * 3. Only HKCU\\Console and its descendants may be addressed at all, so even a fully
 *    attacker-controlled config cannot reach, for example, the autorun keys.
 * 4. Keys are opened with a narrow access mask: only the rights the single operation needs.
 * 5. REG_EXPAND_SZ values are expanded when read so that a snapshot never records the
 *    unexpanded text and later restores something different from what was there.
 */
#include "cbt_registry.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

/* The only subtree this tool is ever allowed to touch. */
#define CBT_ROOT_KEY_PATH L"Console"

typedef struct cbt_win32_ctx {
    int unused;
} cbt_win32_ctx;

/* ------------------------------------------------------------------------- */
/* Encoding helpers                                                          */
/* ------------------------------------------------------------------------- */

static wchar_t *cbt_utf8_to_wide(const char *text)
{
    int needed;
    wchar_t *wide;

    if (text == NULL) {
        return NULL;
    }
    needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }
    wide = (wchar_t *)calloc((size_t)needed, sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, needed) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *cbt_wide_to_utf8(const wchar_t *wide)
{
    int needed;
    char *text;

    if (wide == NULL) {
        return NULL;
    }
    needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return NULL;
    }
    text = (char *)calloc((size_t)needed, 1);
    if (text == NULL) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, needed, NULL, NULL) <= 0) {
        free(text);
        return NULL;
    }
    return text;
}

/* ------------------------------------------------------------------------- */
/* Path validation - the security boundary                                   */
/* ------------------------------------------------------------------------- */

/*
 * Characters that must never appear in a key or value name we are willing to write.
 *
 * '%' is included deliberately. The console keys that carry environment expansions, such as
 * "%SystemRoot%_System32_cmd.exe" and the "%%Startup" delegation key, are never something this
 * tool needs to write, and refusing them outright removes a whole class of "a name that looks
 * like a variable got expanded into somewhere else" reasoning from the security argument.
 */
static int cbt_has_forbidden_char(const wchar_t *component)
{
    size_t i;

    for (i = 0; component[i] != L'\0'; i++) {
        wchar_t ch = component[i];
        if (ch < 0x20) {
            return 1; /* control characters, including embedded CR/LF */
        }
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' ||
            ch == L'%') {
            return 1;
        }
    }
    return 0;
}

/*
 * Reject paths whose first component names a registry hive.
 *
 * The path is always resolved relative to HKEY_CURRENT_USER, so "HKLM\\Software" would in fact
 * create HKCU\Console\HKLM\Software and not touch the machine hive. But a caller writing such a
 * path is confused about where the write lands, and this tool never needs to. Refusing the
 * whole path is both safer and clearer than silently creating a key whose name implies it
 * points somewhere it does not.
 */
static int cbt_path_names_a_hive(const char *relative_path)
{
    static const char *const prefixes[] = {
        "HKLM", "HKCU", "HKCR", "HKU", "HKCC", "HKEY_",
    };
    size_t p;

    for (p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        size_t length = strlen(prefixes[p]);
        size_t i;
        int match = 1;

        for (i = 0; i < length; i++) {
            unsigned char a = (unsigned char)relative_path[i];
            unsigned char b = (unsigned char)prefixes[p][i];
            if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
            if (a != b) {
                match = 0;
                break;
            }
        }
        if (match) {
            /* "HKLMFOO" is just a name that starts with those letters; require a boundary. */
            unsigned char next = (unsigned char)relative_path[length];
            if (next == '\0' || next == '\\' || next == '_' || next == '-') {
                return 1;
            }
        }
    }
    return 0;
}

/*
 * Split a relative path on '\\' and reject anything that could escape the Console subtree
 * or confuse the Win32 layer.
 */
static int cbt_validate_relative_path(const wchar_t *path)
{
    size_t i = 0;
    size_t start = 0;

    if (path == NULL || path[0] == L'\0') {
        return 0;
    }

    for (;;) {
        if (path[i] == L'\\' || path[i] == L'\0') {
            size_t length = i - start;

            if (length == 0) {
                return 0; /* empty component: leading, trailing or doubled separator */
            }
            if (length == 1 && path[start] == L'.') {
                return 0; /* "." is never legitimate here */
            }
            if (length == 2 && path[start] == L'.' && path[start + 1] == L'.') {
                return 0; /* ".." would escape the subtree */
            }
            {
                wchar_t component[256];
                if (length >= 256) {
                    return 0;
                }
                memcpy(component, path + start, length * sizeof(wchar_t));
                component[length] = L'\0';
                if (cbt_has_forbidden_char(component)) {
                    return 0;
                }
            }

            if (path[i] == L'\0') {
                break;
            }
            start = i + 1;
        }
        i++;
    }
    return 1;
}

/*
 * Build the absolute HKCU-relative path for a caller-supplied path and prove that the result
 * stays inside HKCU\Console.
 *
 * Paths are relative to HKEY_CURRENT_USER, exactly as the backend interface documents, so the
 * console key itself is "Console" and its subkeys are "Console\\Name". Getting this wrong is not
 * a cosmetic bug: an earlier revision prepended "Console\\" unconditionally, so a caller passing
 * "Console" silently addressed HKCU\Console\Console and read and wrote the wrong key.
 *
 * Every path is therefore checked twice: first the caller's text, and then the fully assembled
 * absolute path, which must begin with "Console" followed by a separator or the end.
 */
static wchar_t *cbt_build_confined_path(const char *relative_path)
{
    wchar_t *wide;
    wchar_t *absolute;
    size_t relative_length;
    size_t root_length;

    if (relative_path == NULL) {
        return NULL;
    }

    /*
     * Reject hive-qualified, absolute and traversal paths before any Win32 call. Checking the
     * raw caller text here means these are refused even before component parsing.
     */
    if (strstr(relative_path, ":") != NULL) {
        return NULL;
    }
    if (cbt_path_names_a_hive(relative_path)) {
        return NULL;
    }

    wide = cbt_utf8_to_wide(relative_path);
    if (wide == NULL) {
        return NULL;
    }

    if (!cbt_validate_relative_path(wide)) {
        free(wide);
        return NULL;
    }

    relative_length = wcslen(wide);
    absolute = (wchar_t *)calloc(relative_length + 1, sizeof(wchar_t));
    if (absolute == NULL) {
        free(wide);
        return NULL;
    }
    memcpy(absolute, wide, (relative_length + 1) * sizeof(wchar_t));
    free(wide);

    /*
     * The assembled path must live under the console key. This is the check that actually
     * enforces the boundary; everything above only rejects malformed input.
     */
    root_length = wcslen(CBT_ROOT_KEY_PATH);
    if (relative_length < root_length ||
        wcsncmp(absolute, CBT_ROOT_KEY_PATH, root_length) != 0) {
        free(absolute);
        return NULL;
    }
    if (absolute[root_length] != L'\0' && absolute[root_length] != L'\\') {
        /* A name such as "ConsoleX" merely starts with the same letters; reject it. */
        free(absolute);
        return NULL;
    }

    return absolute;
}

/*
 * The public predicate. Exposed so the confinement rule can be tested directly, and so the
 * backend has exactly one place that decides whether a path is acceptable.
 */
int cbt_registry_path_is_confined(const char *relative_path)
{
    wchar_t *absolute;
    int confined;

    if (relative_path == NULL) {
        return 0;
    }

    absolute = cbt_build_confined_path(relative_path);
    confined = absolute != NULL;
    free(absolute);
    return confined;
}

static cbt_status cbt_from_win32_error(LSTATUS status)
{
    switch (status) {
    case ERROR_SUCCESS:
        return CBT_OK;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return CBT_E_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
        return CBT_E_ACCESS;
    case ERROR_MORE_DATA:
    case ERROR_INSUFFICIENT_BUFFER:
        return CBT_E_BUFFER_TOO_SMALL;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        return CBT_E_INVALID_ARG;
    default:
        return CBT_E_IO;
    }
}

/* ------------------------------------------------------------------------- */
/* Backend implementation                                                    */
/* ------------------------------------------------------------------------- */

static cbt_status cbt_win32_open(void *ctx, const char *path, int create, void **out_handle)
{
    wchar_t *absolute;
    HKEY key = NULL;
    LSTATUS status;

    (void)ctx;
    if (path == NULL || out_handle == NULL) {
        return CBT_E_INVALID_ARG;
    }
    *out_handle = NULL;

    absolute = cbt_build_confined_path(path);
    if (absolute == NULL) {
        return CBT_E_INVALID_ARG;
    }

    if (create) {
        DWORD disposition = 0;
        status = RegCreateKeyExW(HKEY_CURRENT_USER, absolute, 0, NULL,
                                 REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                                 NULL, &key, &disposition);
    } else {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, absolute, 0, KEY_READ | KEY_WRITE, &key);
    }

    free(absolute);
    if (status != ERROR_SUCCESS) {
        return cbt_from_win32_error(status);
    }
    *out_handle = (void *)key;
    return CBT_OK;
}

static void cbt_win32_close(void *ctx, void *handle)
{
    (void)ctx;
    if (handle != NULL) {
        RegCloseKey((HKEY)handle);
    }
}

/*
 * Read one value.
 *
 * A note on how the size is discovered. The obvious approach - call RegQueryValueEx with a NULL
 * data pointer to ask for the size, then allocate and call again - is wrong: for REG_DWORD and
 * several other types the API fails outright when lpData is NULL, which makes a perfectly
 * ordinary value look absent. (Measured under MinGW-w64 with GCC 16.2.0: the two-call form
 * returns ERROR_FILE_NOT_FOUND for ColorTable00, while passing a real buffer succeeds.)
 *
 * So the buffer is always allocated first and grown on ERROR_MORE_DATA, which is the documented
 * way to handle a value whose size is not yet known.
 */
static cbt_status cbt_win32_get_value(void *ctx, void *handle, const char *name, cbt_value *out_value)
{
    wchar_t *wide_name;
    DWORD type = 0;
    DWORD capacity = 512;
    BYTE *buffer = NULL;
    DWORD size = capacity;
    LSTATUS status;
    cbt_status result;

    (void)ctx;
    if (handle == NULL || name == NULL || out_value == NULL) {
        return CBT_E_INVALID_ARG;
    }

    out_value->type = CBT_VALUE_NONE;

    wide_name = cbt_utf8_to_wide(name);
    if (wide_name == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (cbt_has_forbidden_char(wide_name)) {
        free(wide_name);
        return CBT_E_INVALID_ARG;
    }

    buffer = (BYTE *)malloc(capacity);
    if (buffer == NULL) {
        free(wide_name);
        return CBT_E_INTERNAL;
    }

    /* Grow until the whole value fits, so a value added between calls cannot be truncated. */
    for (;;) {
        size = capacity;
        status = RegQueryValueExW((HKEY)handle, wide_name, NULL, &type, buffer, &size);
        if (status != ERROR_MORE_DATA) {
            break;
        }
        if (size > 16u * 1024u * 1024u) {
            /* Refuse to keep growing for a value that is not a console setting. */
            free(buffer);
            free(wide_name);
            return CBT_E_OUT_OF_RANGE;
        }
        {
            DWORD new_capacity = size > capacity ? size : capacity * 2u;
            BYTE *grown = (BYTE *)realloc(buffer, new_capacity);
            if (grown == NULL) {
                free(buffer);
                free(wide_name);
                return CBT_E_INTERNAL;
            }
            buffer = grown;
            capacity = new_capacity;
        }
    }

    free(wide_name);

    if (status != ERROR_SUCCESS) {
        free(buffer);
        return cbt_from_win32_error(status);
    }

    switch (type) {
    case REG_DWORD:
        if (size < sizeof(DWORD)) {
            free(buffer);
            return CBT_E_IO;
        }
        {
            DWORD value = 0;
            memcpy(&value, buffer, sizeof(value));
            free(buffer);
            return cbt_value_set_dword(out_value, (uint32_t)value);
        }

    case REG_SZ:
    case REG_EXPAND_SZ: {
        wchar_t *text;
        size_t characters;

        /*
         * The data is not guaranteed to be NUL terminated when it fills the buffer, so the
         * terminator is placed inside the allocation rather than trusting the source.
         */
        if (capacity < sizeof(wchar_t)) {
            free(buffer);
            return CBT_E_IO;
        }
        characters = size / sizeof(wchar_t);
        if (characters > (capacity / sizeof(wchar_t)) - 1u) {
            characters = (capacity / sizeof(wchar_t)) - 1u;
        }
        text = (wchar_t *)buffer;
        text[characters] = L'\0';

        if (type == REG_EXPAND_SZ) {
            DWORD needed = ExpandEnvironmentStringsW(text, NULL, 0);
            if (needed > 0 && needed < 64u * 1024u) {
                wchar_t *expanded = (wchar_t *)calloc(needed, sizeof(wchar_t));
                if (expanded != NULL) {
                    if (ExpandEnvironmentStringsW(text, expanded, needed) > 0) {
                        free(buffer);
                        buffer = (BYTE *)expanded;
                        text = expanded;
                    } else {
                        free(expanded);
                    }
                }
            }
        }

        {
            char *utf8 = cbt_wide_to_utf8(text);
            free(buffer);
            if (utf8 == NULL) {
                return CBT_E_INTERNAL;
            }
            result = cbt_value_set_string(out_value, utf8);
            free(utf8);
            return result;
        }
    }

    case REG_BINARY:
        result = cbt_value_set_binary(out_value, buffer, size);
        free(buffer);
        return result;

    default:
        /* REG_MULTI_SZ, REG_QWORD, REG_NONE and friends are reported as unsupported rather
         * than silently coerced into the wrong representation. */
        free(buffer);
        return CBT_E_UNSUPPORTED;
    }
}

static cbt_status cbt_win32_set_value(void *ctx, void *handle, const char *name, const cbt_value *value)
{
    wchar_t *wide_name;
    LSTATUS status;

    (void)ctx;
    if (handle == NULL || name == NULL || value == NULL) {
        return CBT_E_INVALID_ARG;
    }

    wide_name = cbt_utf8_to_wide(name);
    if (wide_name == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (cbt_has_forbidden_char(wide_name)) {
        free(wide_name);
        return CBT_E_INVALID_ARG;
    }

    switch (value->type) {
    case CBT_VALUE_DWORD: {
        DWORD dword = (DWORD)value->u.dword;
        status = RegSetValueExW((HKEY)handle, wide_name, 0, REG_DWORD, (const BYTE *)&dword, sizeof(dword));
        break;
    }
    case CBT_VALUE_STRING: {
        wchar_t *wide_value = cbt_utf8_to_wide(value->u.string.text);
        if (wide_value == NULL) {
            free(wide_name);
            return CBT_E_INVALID_ARG;
        }
        status = RegSetValueExW((HKEY)handle, wide_name, 0, REG_SZ, (const BYTE *)wide_value,
                                (DWORD)((wcslen(wide_value) + 1) * sizeof(wchar_t)));
        free(wide_value);
        break;
    }
    case CBT_VALUE_BINARY:
        status = RegSetValueExW((HKEY)handle, wide_name, 0, REG_BINARY,
                                value->u.binary.data, (DWORD)value->u.binary.size);
        break;
    default:
        free(wide_name);
        return CBT_E_INVALID_ARG;
    }

    free(wide_name);
    return cbt_from_win32_error(status);
}

static cbt_status cbt_win32_delete_value(void *ctx, void *handle, const char *name)
{
    wchar_t *wide_name;
    LSTATUS status;

    (void)ctx;
    if (handle == NULL || name == NULL) {
        return CBT_E_INVALID_ARG;
    }

    wide_name = cbt_utf8_to_wide(name);
    if (wide_name == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (cbt_has_forbidden_char(wide_name)) {
        free(wide_name);
        return CBT_E_INVALID_ARG;
    }

    status = RegDeleteValueW((HKEY)handle, wide_name);
    free(wide_name);

    /* A value that is already absent is the desired end state. */
    if (status == ERROR_FILE_NOT_FOUND) {
        return CBT_OK;
    }
    return cbt_from_win32_error(status);
}

static cbt_status cbt_win32_delete_key(void *ctx, const char *path)
{
    wchar_t *absolute;
    LSTATUS status;

    (void)ctx;
    if (path == NULL) {
        return CBT_E_INVALID_ARG;
    }

    absolute = cbt_build_confined_path(path);
    if (absolute == NULL) {
        return CBT_E_INVALID_ARG;
    }

    status = RegDeleteTreeW(HKEY_CURRENT_USER, absolute);
    free(absolute);

    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return CBT_OK;
    }
    return cbt_from_win32_error(status);
}

static int cbt_win32_key_exists(void *ctx, const char *path)
{
    wchar_t *absolute;
    HKEY key = NULL;
    LSTATUS status;

    (void)ctx;
    if (path == NULL) {
        return 0;
    }

    absolute = cbt_build_confined_path(path);
    if (absolute == NULL) {
        return 0;
    }
    status = RegOpenKeyExW(HKEY_CURRENT_USER, absolute, 0, KEY_READ, &key);
    free(absolute);
    if (status != ERROR_SUCCESS) {
        return 0;
    }
    RegCloseKey(key);
    return 1;
}

/*
 * Enumerate every value under a key. Uses the documented two-call pattern: the first
 * RegEnumValueW returns ERROR_MORE_DATA and the required sizes, the second fills the
 * buffers. The loop grows the buffer until the call succeeds, so a value added between the
 * two calls cannot be silently dropped.
 */
static cbt_status cbt_win32_snapshot_key(void *ctx, const char *path, cbt_key_snapshot *out_snapshot)
{
    wchar_t *absolute;
    HKEY key = NULL;
    LSTATUS status;
    DWORD index = 0;

    (void)ctx;
    if (path == NULL || out_snapshot == NULL) {
        return CBT_E_INVALID_ARG;
    }

    cbt_key_snapshot_init(out_snapshot);
    {
        size_t length = strlen(path);
        out_snapshot->key_path = (char *)malloc(length + 1);
        if (out_snapshot->key_path == NULL) {
            return CBT_E_INTERNAL;
        }
        memcpy(out_snapshot->key_path, path, length + 1);
    }

    absolute = cbt_build_confined_path(path);
    if (absolute == NULL) {
        cbt_key_snapshot_free(out_snapshot);
        return CBT_E_INVALID_ARG;
    }

    status = RegOpenKeyExW(HKEY_CURRENT_USER, absolute, 0, KEY_READ, &key);
    free(absolute);

    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        out_snapshot->existed = 0;
        return CBT_OK;
    }
    if (status != ERROR_SUCCESS) {
        cbt_key_snapshot_free(out_snapshot);
        return cbt_from_win32_error(status);
    }

    out_snapshot->existed = 1;

    for (;;) {
        DWORD name_capacity = 512;
        DWORD value_capacity = 512;
        wchar_t *name_buffer;
        BYTE *value_buffer;
        DWORD name_length = name_capacity;
        DWORD value_length = value_capacity;
        DWORD type = 0;

        name_buffer = (wchar_t *)calloc(name_capacity, sizeof(wchar_t));
        value_buffer = (BYTE *)calloc(value_capacity, 1);
        if (name_buffer == NULL || value_buffer == NULL) {
            free(name_buffer);
            free(value_buffer);
            cbt_key_snapshot_free(out_snapshot);
            return CBT_E_INTERNAL;
        }

        for (;;) {
            status = RegEnumValueW(key, index, name_buffer, &name_length, NULL, &type,
                                   value_buffer, &value_length);
            if (status == ERROR_MORE_DATA) {
                /* Grow to exactly what the registry asked for and retry this index. */
                if (value_length > value_capacity) {
                    BYTE *grown = (BYTE *)realloc(value_buffer, value_length);
                    if (grown == NULL) {
                        free(name_buffer);
                        free(value_buffer);
                        cbt_key_snapshot_free(out_snapshot);
                        return CBT_E_INTERNAL;
                    }
                    value_buffer = grown;
                    value_capacity = value_length;
                }
                if (name_length + 1 > name_capacity) {
                    wchar_t *grown_name = (wchar_t *)realloc(name_buffer, (name_length + 1) * sizeof(wchar_t));
                    if (grown_name == NULL) {
                        free(name_buffer);
                        free(value_buffer);
                        cbt_key_snapshot_free(out_snapshot);
                        return CBT_E_INTERNAL;
                    }
                    name_buffer = grown_name;
                    name_capacity = name_length + 1;
                }
                name_length = name_capacity;
                value_length = value_capacity;
                continue;
            }
            break;
        }

        if (status == ERROR_NO_MORE_ITEMS) {
            free(name_buffer);
            free(value_buffer);
            break;
        }
        if (status != ERROR_SUCCESS) {
            free(name_buffer);
            free(value_buffer);
            cbt_key_snapshot_free(out_snapshot);
            return cbt_from_win32_error(status);
        }

        /* Convert this entry into a snapshot row. */
        {
            cbt_value value;
            char *utf8_name;
            cbt_status add_status = CBT_OK;

            memset(&value, 0, sizeof(value));
            switch (type) {
            case REG_DWORD:
                if (value_length >= sizeof(DWORD)) {
                    DWORD dword = 0;
                    memcpy(&dword, value_buffer, sizeof(dword));
                    add_status = cbt_value_set_dword(&value, (uint32_t)dword);
                } else {
                    add_status = CBT_E_UNSUPPORTED;
                }
                break;
            case REG_SZ:
            case REG_EXPAND_SZ: {
                wchar_t *as_text = (wchar_t *)value_buffer;
                size_t characters = value_length / sizeof(wchar_t);
                size_t capacity_characters = value_capacity / sizeof(wchar_t);
                char *utf8;

                /*
                 * RegEnumValue does not guarantee a NUL terminator when the data fills the
                 * buffer, and the count it returns excludes any terminator. Reserve the last
                 * element so the string is always terminated inside the allocation.
                 */
                if (characters >= capacity_characters) {
                    characters = capacity_characters - 1;
                }
                as_text[characters] = L'\0';
                /* Expand now so the restore writes the same effective string. */
                if (type == REG_EXPAND_SZ) {
                    DWORD needed = ExpandEnvironmentStringsW(as_text, NULL, 0);
                    if (needed > 0) {
                        wchar_t *expanded = (wchar_t *)calloc(needed, sizeof(wchar_t));
                        if (expanded != NULL) {
                            if (ExpandEnvironmentStringsW(as_text, expanded, needed) > 0) {
                                utf8 = cbt_wide_to_utf8(expanded);
                                free(expanded);
                                if (utf8 != NULL) {
                                    add_status = cbt_value_set_string(&value, utf8);
                                    free(utf8);
                                } else {
                                    add_status = CBT_E_INTERNAL;
                                }
                                break;
                            }
                            free(expanded);
                        }
                    }
                }
                utf8 = cbt_wide_to_utf8(as_text);
                if (utf8 != NULL) {
                    add_status = cbt_value_set_string(&value, utf8);
                    free(utf8);
                } else {
                    add_status = CBT_E_INTERNAL;
                }
                break;
            }
            case REG_BINARY:
                add_status = cbt_value_set_binary(&value, value_buffer, value_length);
                break;
            default:
                /*
                 * An unmodelled type (REG_QWORD, REG_MULTI_SZ, REG_NONE) is recorded as
                 * unsupported. The transactional layer treats "cannot snapshot" as "do not
                 * write", because restoring an unknown type incorrectly would be worse than
                 * declining to change the key.
                 */
                add_status = CBT_E_UNSUPPORTED;
                break;
            }

            utf8_name = cbt_wide_to_utf8(name_buffer);
            free(name_buffer);
            free(value_buffer);

            if (utf8_name == NULL) {
                cbt_value_clear(&value);
                cbt_key_snapshot_free(out_snapshot);
                return CBT_E_INTERNAL;
            }
            if (add_status != CBT_OK) {
                free(utf8_name);
                cbt_value_clear(&value);
                cbt_key_snapshot_free(out_snapshot);
                return add_status;
            }

            add_status = cbt_snapshot_add_value(out_snapshot, utf8_name, &value);
            free(utf8_name);
            cbt_value_clear(&value);
            if (add_status != CBT_OK) {
                cbt_key_snapshot_free(out_snapshot);
                return add_status;
            }
        }

        index++;
    }

    RegCloseKey(key);
    return CBT_OK;
}

static cbt_status cbt_win32_restore_key(void *ctx, const cbt_key_snapshot *snapshot)
{
    HKEY key = NULL;
    cbt_status status;
    size_t i;

    if (snapshot == NULL || snapshot->key_path == NULL) {
        return CBT_E_INVALID_ARG;
    }

    if (!snapshot->existed) {
        return cbt_win32_delete_key(ctx, snapshot->key_path);
    }

    status = cbt_win32_open(ctx, snapshot->key_path, 1, (void **)&key);
    if (status != CBT_OK) {
        return status;
    }

    /*
     * Remove every value currently present so the key ends up exactly as snapshotted.
     * Failures are propagated: a partially restored key is a callback for the caller's
     * journal, not something to swallow.
     */
    {
        DWORD index = 0;
        for (;;) {
            wchar_t name_buffer[512];
            DWORD name_length = 512;
            LSTATUS enumerate_status = RegEnumValueW(key, index, name_buffer, &name_length,
                                                     NULL, NULL, NULL, NULL);
            if (enumerate_status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (enumerate_status != ERROR_SUCCESS) {
                RegCloseKey(key);
                return cbt_from_win32_error(enumerate_status);
            }
            enumerate_status = RegDeleteValueW(key, name_buffer);
            if (enumerate_status != ERROR_SUCCESS && enumerate_status != ERROR_FILE_NOT_FOUND) {
                RegCloseKey(key);
                return cbt_from_win32_error(enumerate_status);
            }
            /* Do not advance: deletion shifts the remaining values down. */
        }
    }

    for (i = 0; i < snapshot->count; i++) {
        status = cbt_win32_set_value(ctx, key, snapshot->names[i], &snapshot->values[i]);
        if (status != CBT_OK) {
            RegCloseKey(key);
            return status;
        }
    }

    RegCloseKey(key);
    return CBT_OK;
}

/* ------------------------------------------------------------------------- */
/* Public accessor                                                           */
/* ------------------------------------------------------------------------- */

cbt_registry_backend *cbt_registry_win32(void)
{
    static cbt_registry_backend backend;
    static cbt_win32_ctx context;
    static int initialized = 0;

    if (!initialized) {
        backend.name = "win32";
        backend.ctx = &context;
        backend.open = cbt_win32_open;
        backend.close = cbt_win32_close;
        backend.get_value = cbt_win32_get_value;
        backend.set_value = cbt_win32_set_value;
        backend.delete_value = cbt_win32_delete_value;
        backend.delete_key = cbt_win32_delete_key;
        backend.key_exists = cbt_win32_key_exists;
        backend.snapshot_key = cbt_win32_snapshot_key;
        backend.restore_key = cbt_win32_restore_key;
        backend.destroy = NULL; /* static context, nothing to free */
        initialized = 1;
    }
    return &backend;
}

void cbt_registry_win32_shutdown(void)
{
    /* Nothing to release: the backend owns no heap state. */
}
