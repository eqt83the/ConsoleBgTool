/*
 * ConsoleBgTool - native core (C11)
 * cbt_registry.h - swappable registry backend.
 *
 * Every registry operation the tool performs goes through this interface. Two
 * implementations ship:
 *
 *   cbt_registry_win32()   real HKCU access through advapi32
 *   cbt_registry_memory()  an in-process store used by the unit tests
 *
 * The indirection exists so the transactional apply/restore logic can be tested for exact
 * behaviour - including failure injection - without ever touching the developer's real
 * console configuration.
 */
#ifndef CBT_REGISTRY_H
#define CBT_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "cbt_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values a backend must be able to store. Mirrors the Win32 types used by HKCU\Console. */
typedef enum cbt_value_type {
    CBT_VALUE_NONE = 0,
    CBT_VALUE_DWORD,
    CBT_VALUE_STRING, /* REG_SZ */
    CBT_VALUE_BINARY  /* REG_BINARY */
} cbt_value_type;

typedef struct cbt_value {
    cbt_value_type type;
    union {
        uint32_t dword;
        struct {
            char *text; /* UTF-8, NUL terminated */
        } string;
        struct {
            uint8_t *data;
            size_t   size;
        } binary;
    } u;
} cbt_value;

/* Free any heap memory owned by a value and reset it to CBT_VALUE_NONE. */
CBT_API void cbt_value_clear(cbt_value *value);

/* Duplicate a value (deep copy for strings/binary). Returns CBT_OK or CBT_E_INTERNAL. */
CBT_API cbt_status cbt_value_copy(cbt_value *dest, const cbt_value *src);

/* Convenience constructors; all may allocate, so all can fail. */
CBT_API cbt_status cbt_value_set_dword(cbt_value *value, uint32_t dword);
CBT_API cbt_status cbt_value_set_string(cbt_value *value, const char *text);
CBT_API cbt_status cbt_value_set_binary(cbt_value *value, const void *data, size_t size);

/* An opaque snapshot of one registry key, suitable for exact restore. */
typedef struct cbt_key_snapshot {
    /* UTF-8 path of the key this snapshot came from. Heap allocated. */
    char *key_path;
    int   existed; /* 0 when the key did not exist at snapshot time */

    char **names;   /* value names, heap allocated */
    cbt_value *values;
    size_t   count;
    size_t   capacity;
} cbt_key_snapshot;

CBT_API void cbt_key_snapshot_init(cbt_key_snapshot *snapshot);
CBT_API void cbt_key_snapshot_free(cbt_key_snapshot *snapshot);

/* Read-only access to a snapshot, for journaling and for the tests. */
CBT_API size_t cbt_key_snapshot_count(const cbt_key_snapshot *snapshot);
CBT_API const char *cbt_key_snapshot_name(const cbt_key_snapshot *snapshot, size_t index);
CBT_API const cbt_value *cbt_key_snapshot_value(const cbt_key_snapshot *snapshot, size_t index);

/* Append one value. Used by backends; deep copies `name` and `value`. */
CBT_API cbt_status cbt_snapshot_add_value(cbt_key_snapshot *snapshot, const char *name,
                                          const cbt_value *value);

typedef struct cbt_registry_backend cbt_registry_backend;

struct cbt_registry_backend {
    /* Human readable backend name, e.g. "win32" or "memory". */
    const char *name;

    /* Backend-private state, passed back as the first argument of every operation. */
    void *ctx;

    /*
     * Open (creating when `create` is non-zero) a key. `path` is UTF-8, uses '\\' as the
     * separator, and is relative to HKEY_CURRENT_USER: the console key itself is "Console" and
     * its subkeys are "Console\\Name".
     *
     * The path is validated against the confinement rule before any Win32 call, so an absolute
     * path, a hive-qualified path, a ".." component or a name that merely resembles the console
     * key is refused with CBT_E_INVALID_ARG. Use cbt_registry_path_is_confined() to test a path
     * without performing an operation.
     */
    cbt_status (*open)(void *ctx, const char *path, int create, void **out_handle);

    void (*close)(void *ctx, void *handle);

    /* Query a value. Returns CBT_E_NOT_FOUND when the value is absent. */
    cbt_status (*get_value)(void *ctx, void *handle, const char *name, cbt_value *out_value);

    cbt_status (*set_value)(void *ctx, void *handle, const char *name, const cbt_value *value);

    cbt_status (*delete_value)(void *ctx, void *handle, const char *name);

    /* Delete a key and everything below it. Succeeds when the key does not exist. */
    cbt_status (*delete_key)(void *ctx, const char *path);

    /* 1 when the key exists. */
    int (*key_exists)(void *ctx, const char *path);

    /*
     * Capture every value under a key so the exact prior state can be put back. Sets
     * snapshot->existed. Never fails merely because the key is missing.
     */
    cbt_status (*snapshot_key)(void *ctx, const char *path, cbt_key_snapshot *out_snapshot);

    /*
     * Restore a snapshot: recreate or remove the key, delete values that were absent and
     * rewrite values that were present.
     */
    cbt_status (*restore_key)(void *ctx, const cbt_key_snapshot *snapshot);

    void (*destroy)(void *ctx);
};

/*
 * The process-wide real backend. Valid until cbt_registry_win32_shutdown().
 *
 * Writes are always confined to HKEY_CURRENT_USER; the backend refuses any path that
 * contains a drive-qualified prefix or an HKEY_LOCAL_MACHINE reference, so an operator can
 * never be tricked into a machine-wide change.
 */
CBT_API cbt_registry_backend *cbt_registry_win32(void);
CBT_API void cbt_registry_win32_shutdown(void);

/*
 * Test backend. `fail_after_writes` (>= 0) makes the backend start returning CBT_E_ACCESS
 * after that many successful writes, which the transactional tests use to prove that a
 * half-applied configuration is rolled back. Pass -1 to disable injection.
 */
CBT_API cbt_registry_backend *cbt_registry_memory(int fail_after_writes);
CBT_API void cbt_registry_memory_destroy(cbt_registry_backend *backend);

/*
 * Test-only: re-arm the memory backend's failure injection and reset its write counter.
 * `fail_after_writes` < 0 disables injection. A no-op for any other backend.
 */
CBT_API void cbt_mem_store_arm(cbt_registry_backend *backend, int fail_after_writes);

/*
 * The path confinement rule, exposed so it can be unit tested directly rather than only
 * inferred from the effect of a whole operation.
 *
 * Returns 1 when `relative_path` may be resolved under HKCU\Console, 0 otherwise. The real
 * backend uses this same predicate before every Win32 call, so a test of this function is a test
 * of the actual boundary.
 */
CBT_API int cbt_registry_path_is_confined(const char *relative_path);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CBT_REGISTRY_H */
