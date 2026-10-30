/*
 * ConsoleBgTool - native core (C11)
 * cbt_registry_memory.c - in-process registry backend used by the unit tests.
 *
 * Purpose: let the snapshot / restore / transactional logic be exercised, including
 * injected write failures, without a single byte of the developer's real console
 * configuration being touched. The production path never uses this backend.
 */
#include "cbt_registry.h"

#include <stdlib.h>
#include <string.h>

typedef struct cbt_mem_value {
    char      *name;
    cbt_value  value;
} cbt_mem_value;

typedef struct cbt_mem_key {
    char          *path;
    cbt_mem_value *values;
    size_t         count;
    size_t         capacity;
} cbt_mem_key;

typedef struct cbt_mem_store {
    cbt_mem_key *keys;
    size_t       count;
    size_t       capacity;

    /* Failure injection: after this many successful writes, every write fails. */
    int fail_after_writes;
    int writes;
} cbt_mem_store;

/* ------------------------------------------------------------------------- */
/* Internals                                                                 */
/* ------------------------------------------------------------------------- */

/* Case-insensitive compare, matching the registry's own name semantics. */
static int cbt_name_equal(const char *a, const char *b)
{
    size_t i = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }
    for (;;) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
        i++;
    }
}

static char *cbt_dup(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

/* Case-insensitive comparison of the first `length` characters. */
static int cbt_prefix_equal(const char *a, const char *b, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static cbt_mem_key *cbt_mem_find(cbt_mem_store *store, const char *path)
{
    size_t i;

    for (i = 0; i < store->count; i++) {
        if (cbt_name_equal(store->keys[i].path, path)) {
            return &store->keys[i];
        }
    }
    return NULL;
}

static cbt_mem_key *cbt_mem_create(cbt_mem_store *store, const char *path)
{
    cbt_mem_key *key;

    if (store->count == store->capacity) {
        size_t new_capacity = store->capacity == 0 ? 8 : store->capacity * 2;
        cbt_mem_key *grown = (cbt_mem_key *)realloc(store->keys, new_capacity * sizeof(cbt_mem_key));
        if (grown == NULL) {
            return NULL;
        }
        store->keys = grown;
        store->capacity = new_capacity;
    }

    key = &store->keys[store->count];
    memset(key, 0, sizeof(*key));
    key->path = cbt_dup(path);
    if (key->path == NULL) {
        return NULL;
    }
    store->count++;
    return key;
}

static void cbt_mem_free_key(cbt_mem_key *key)
{
    size_t i;

    free(key->path);
    for (i = 0; i < key->count; i++) {
        free(key->values[i].name);
        cbt_value_clear(&key->values[i].value);
    }
    free(key->values);
    memset(key, 0, sizeof(*key));
}

static cbt_mem_value *cbt_mem_find_value(cbt_mem_key *key, const char *name)
{
    size_t i;

    for (i = 0; i < key->count; i++) {
        if (cbt_name_equal(key->values[i].name, name)) {
            return &key->values[i];
        }
    }
    return NULL;
}

static cbt_status cbt_mem_put(cbt_mem_store *store, cbt_mem_key *key, const char *name,
                              const cbt_value *value)
{
    cbt_mem_value *entry;

    if (store->fail_after_writes >= 0 && store->writes >= store->fail_after_writes) {
        store->writes++;
        return CBT_E_ACCESS;
    }
    store->writes++;

    entry = cbt_mem_find_value(key, name);
    if (entry == NULL) {
        if (key->count == key->capacity) {
            size_t new_capacity = key->capacity == 0 ? 8 : key->capacity * 2;
            cbt_mem_value *grown = (cbt_mem_value *)realloc(key->values, new_capacity * sizeof(cbt_mem_value));
            if (grown == NULL) {
                return CBT_E_INTERNAL;
            }
            key->values = grown;
            key->capacity = new_capacity;
        }
        entry = &key->values[key->count];
        memset(entry, 0, sizeof(*entry));
        entry->name = cbt_dup(name);
        if (entry->name == NULL) {
            return CBT_E_INTERNAL;
        }
        key->count++;
    }
    return cbt_value_copy(&entry->value, value);
}

/* ------------------------------------------------------------------------- */
/* Backend operations                                                        */
/* ------------------------------------------------------------------------- */

static cbt_status cbt_mem_open(void *ctx, const char *path, int create, void **out_handle)
{
    cbt_mem_store *store = (cbt_mem_store *)ctx;
    cbt_mem_key *key;

    if (store == NULL || path == NULL || out_handle == NULL) {
        return CBT_E_INVALID_ARG;
    }
    *out_handle = NULL;

    key = cbt_mem_find(store, path);
    if (key == NULL) {
        if (!create) {
            return CBT_E_NOT_FOUND;
        }
        key = cbt_mem_create(store, path);
        if (key == NULL) {
            return CBT_E_INTERNAL;
        }
    }
    *out_handle = key;
    return CBT_OK;
}

static void cbt_mem_close(void *ctx, void *handle)
{
    (void)ctx;
    (void)handle;
}

static cbt_status cbt_mem_get_value(void *ctx, void *handle, const char *name, cbt_value *out_value)
{
    cbt_mem_key *key = (cbt_mem_key *)handle;
    cbt_mem_value *entry;

    (void)ctx;
    if (key == NULL || name == NULL || out_value == NULL) {
        return CBT_E_INVALID_ARG;
    }
    entry = cbt_mem_find_value(key, name);
    if (entry == NULL) {
        cbt_value_clear(out_value);
        return CBT_E_NOT_FOUND;
    }
    return cbt_value_copy(out_value, &entry->value);
}

static cbt_status cbt_mem_set_value(void *ctx, void *handle, const char *name, const cbt_value *value)
{
    if (ctx == NULL || handle == NULL || name == NULL || value == NULL) {
        return CBT_E_INVALID_ARG;
    }
    return cbt_mem_put((cbt_mem_store *)ctx, (cbt_mem_key *)handle, name, value);
}

static cbt_status cbt_mem_delete_value(void *ctx, void *handle, const char *name)
{
    cbt_mem_key *key = (cbt_mem_key *)handle;
    size_t i;

    (void)ctx;
    if (key == NULL || name == NULL) {
        return CBT_E_INVALID_ARG;
    }

    for (i = 0; i < key->count; i++) {
        if (cbt_name_equal(key->values[i].name, name)) {
            free(key->values[i].name);
            cbt_value_clear(&key->values[i].value);
            if (i + 1 < key->count) {
                memmove(&key->values[i], &key->values[i + 1],
                        (key->count - i - 1) * sizeof(cbt_mem_value));
            }
            key->count--;
            return CBT_OK;
        }
    }
    /* Deleting an absent value is not an error: the desired state is already true. */
    return CBT_OK;
}

static cbt_status cbt_mem_delete_key(void *ctx, const char *path)
{
    cbt_mem_store *store = (cbt_mem_store *)ctx;
    size_t prefix_length;
    size_t i;

    if (store == NULL || path == NULL) {
        return CBT_E_INVALID_ARG;
    }

    prefix_length = strlen(path);

    for (i = 0; i < store->count;) {
        int matches = cbt_name_equal(store->keys[i].path, path);

        /* Also remove strict subkeys: path + "\\...". */
        if (!matches && strlen(store->keys[i].path) > prefix_length + 1 &&
            store->keys[i].path[prefix_length] == '\\' &&
            cbt_prefix_equal(store->keys[i].path, path, prefix_length)) {
            matches = 1;
        }

        if (matches) {
            cbt_mem_free_key(&store->keys[i]);
            if (i + 1 < store->count) {
                memmove(&store->keys[i], &store->keys[i + 1],
                        (store->count - i - 1) * sizeof(cbt_mem_key));
            }
            store->count--;
        } else {
            i++;
        }
    }
    return CBT_OK;
}

static int cbt_mem_key_exists(void *ctx, const char *path)
{
    cbt_mem_store *store = (cbt_mem_store *)ctx;

    if (store == NULL || path == NULL) {
        return 0;
    }
    return cbt_mem_find(store, path) != NULL;
}

static cbt_status cbt_mem_snapshot_key(void *ctx, const char *path, cbt_key_snapshot *out_snapshot)
{
    cbt_mem_store *store = (cbt_mem_store *)ctx;
    cbt_mem_key *key;
    size_t i;

    if (store == NULL || path == NULL || out_snapshot == NULL) {
        return CBT_E_INVALID_ARG;
    }

    cbt_key_snapshot_init(out_snapshot);
    out_snapshot->key_path = cbt_dup(path);
    if (out_snapshot->key_path == NULL) {
        return CBT_E_INTERNAL;
    }

    key = cbt_mem_find(store, path);
    if (key == NULL) {
        out_snapshot->existed = 0;
        return CBT_OK;
    }

    out_snapshot->existed = 1;
    for (i = 0; i < key->count; i++) {
        cbt_status status = cbt_snapshot_add_value(out_snapshot, key->values[i].name,
                                                   &key->values[i].value);
        if (status != CBT_OK) {
            cbt_key_snapshot_free(out_snapshot);
            return status;
        }
    }
    return CBT_OK;
}

static cbt_status cbt_mem_restore_key(void *ctx, const cbt_key_snapshot *snapshot)
{
    cbt_mem_store *store = (cbt_mem_store *)ctx;
    cbt_mem_key *key;
    size_t i;

    if (store == NULL || snapshot == NULL || snapshot->key_path == NULL) {
        return CBT_E_INVALID_ARG;
    }

    if (!snapshot->existed) {
        return cbt_mem_delete_key(store, snapshot->key_path);
    }

    key = cbt_mem_find(store, snapshot->key_path);
    if (key == NULL) {
        key = cbt_mem_create(store, snapshot->key_path);
        if (key == NULL) {
            return CBT_E_INTERNAL;
        }
    } else {
        /* The snapshot is authoritative: drop everything currently present. */
        for (i = 0; i < key->count; i++) {
            free(key->values[i].name);
            cbt_value_clear(&key->values[i].value);
        }
        key->count = 0;
    }

    for (i = 0; i < snapshot->count; i++) {
        cbt_status status = cbt_mem_put(store, key, snapshot->names[i], &snapshot->values[i]);
        if (status != CBT_OK) {
            return status;
        }
    }
    return CBT_OK;
}

static void cbt_mem_destroy(void *ctx)
{
    cbt_mem_store *store = (cbt_mem_store *)ctx;
    size_t i;

    if (store == NULL) {
        return;
    }
    for (i = 0; i < store->count; i++) {
        cbt_mem_free_key(&store->keys[i]);
    }
    free(store->keys);
    free(store);
}

/* ------------------------------------------------------------------------- */
/* Public accessor                                                           */
/* ------------------------------------------------------------------------- */

cbt_registry_backend *cbt_registry_memory(int fail_after_writes)
{
    cbt_registry_backend *backend;
    cbt_mem_store *store = (cbt_mem_store *)calloc(1, sizeof(cbt_mem_store));

    if (store == NULL) {
        return NULL;
    }
    store->fail_after_writes = fail_after_writes;

    backend = (cbt_registry_backend *)calloc(1, sizeof(cbt_registry_backend));
    if (backend == NULL) {
        free(store);
        return NULL;
    }

    backend->name = "memory";
    backend->ctx = store;
    backend->open = cbt_mem_open;
    backend->close = cbt_mem_close;
    backend->get_value = cbt_mem_get_value;
    backend->set_value = cbt_mem_set_value;
    backend->delete_value = cbt_mem_delete_value;
    backend->delete_key = cbt_mem_delete_key;
    backend->key_exists = cbt_mem_key_exists;
    backend->snapshot_key = cbt_mem_snapshot_key;
    backend->restore_key = cbt_mem_restore_key;
    backend->destroy = cbt_mem_destroy;
    return backend;
}

void cbt_registry_memory_destroy(cbt_registry_backend *backend)
{
    if (backend != NULL && backend->destroy != NULL) {
        backend->destroy(backend->ctx);
    }
    free(backend);
}

void cbt_mem_store_arm(cbt_registry_backend *backend, int fail_after_writes)
{
    cbt_mem_store *store;

    if (backend == NULL || backend->name == NULL || strcmp(backend->name, "memory") != 0) {
        return;
    }

    store = (cbt_mem_store *)backend->ctx;
    if (store == NULL) {
        return;
    }
    store->fail_after_writes = fail_after_writes;
    store->writes = 0;
}
