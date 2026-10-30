/*
 * ConsoleBgTool - native core (C11)
 * cbt_registry.c - value ownership helpers and the snapshot container shared by backends.
 */
#include "cbt_registry.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small internal helpers                                                    */
/* ------------------------------------------------------------------------- */

static char *cbt_strdup(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length);
    return copy;
}

void cbt_value_clear(cbt_value *value)
{
    if (value == NULL) {
        return;
    }
    if (value->type == CBT_VALUE_STRING) {
        free(value->u.string.text);
    } else if (value->type == CBT_VALUE_BINARY) {
        free(value->u.binary.data);
    }
    memset(value, 0, sizeof(*value));
    value->type = CBT_VALUE_NONE;
}

cbt_status cbt_value_set_dword(cbt_value *value, uint32_t dword)
{
    if (value == NULL) {
        return CBT_E_INVALID_ARG;
    }
    cbt_value_clear(value);
    value->type = CBT_VALUE_DWORD;
    value->u.dword = dword;
    return CBT_OK;
}

cbt_status cbt_value_set_string(cbt_value *value, const char *text)
{
    char *copy;

    if (value == NULL || text == NULL) {
        return CBT_E_INVALID_ARG;
    }
    copy = cbt_strdup(text);
    if (copy == NULL) {
        return CBT_E_INTERNAL;
    }
    cbt_value_clear(value);
    value->type = CBT_VALUE_STRING;
    value->u.string.text = copy;
    return CBT_OK;
}

cbt_status cbt_value_set_binary(cbt_value *value, const void *data, size_t size)
{
    uint8_t *copy = NULL;

    if (value == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (size > 0) {
        if (data == NULL) {
            return CBT_E_INVALID_ARG;
        }
        copy = (uint8_t *)malloc(size);
        if (copy == NULL) {
            return CBT_E_INTERNAL;
        }
        memcpy(copy, data, size);
    }
    cbt_value_clear(value);
    value->type = CBT_VALUE_BINARY;
    value->u.binary.data = copy;
    value->u.binary.size = size;
    return CBT_OK;
}

cbt_status cbt_value_copy(cbt_value *dest, const cbt_value *src)
{
    if (dest == NULL || src == NULL) {
        return CBT_E_INVALID_ARG;
    }
    if (dest == src) {
        return CBT_OK;
    }

    switch (src->type) {
    case CBT_VALUE_NONE:
        cbt_value_clear(dest);
        return CBT_OK;
    case CBT_VALUE_DWORD:
        return cbt_value_set_dword(dest, src->u.dword);
    case CBT_VALUE_STRING:
        return cbt_value_set_string(dest, src->u.string.text);
    case CBT_VALUE_BINARY:
        return cbt_value_set_binary(dest, src->u.binary.data, src->u.binary.size);
    default:
        cbt_value_clear(dest);
        return CBT_E_INVALID_ARG;
    }
}

/* ------------------------------------------------------------------------- */
/* Snapshot container                                                        */
/* ------------------------------------------------------------------------- */

void cbt_key_snapshot_init(cbt_key_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

void cbt_key_snapshot_free(cbt_key_snapshot *snapshot)
{
    size_t i;

    if (snapshot == NULL) {
        return;
    }
    free(snapshot->key_path);
    for (i = 0; i < snapshot->count; i++) {
        free(snapshot->names[i]);
        cbt_value_clear(&snapshot->values[i]);
    }
    free(snapshot->names);
    free(snapshot->values);
    memset(snapshot, 0, sizeof(*snapshot));
}

size_t cbt_key_snapshot_count(const cbt_key_snapshot *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->count;
}

const char *cbt_key_snapshot_name(const cbt_key_snapshot *snapshot, size_t index)
{
    if (snapshot == NULL || index >= snapshot->count) {
        return NULL;
    }
    return snapshot->names[index];
}

const cbt_value *cbt_key_snapshot_value(const cbt_key_snapshot *snapshot, size_t index)
{
    if (snapshot == NULL || index >= snapshot->count) {
        return NULL;
    }
    return &snapshot->values[index];
}

cbt_status cbt_snapshot_add_value(cbt_key_snapshot *snapshot, const char *name,
                                  const cbt_value *value)
{
    char *name_copy;
    size_t index;

    if (snapshot == NULL || name == NULL || value == NULL) {
        return CBT_E_INVALID_ARG;
    }

    if (snapshot->count == snapshot->capacity) {
        size_t new_capacity = snapshot->capacity == 0 ? 8 : snapshot->capacity * 2;
        char **new_names = (char **)realloc(snapshot->names, new_capacity * sizeof(char *));
        cbt_value *new_values;

        if (new_names == NULL) {
            return CBT_E_INTERNAL;
        }
        snapshot->names = new_names;

        new_values = (cbt_value *)realloc(snapshot->values, new_capacity * sizeof(cbt_value));
        if (new_values == NULL) {
            return CBT_E_INTERNAL;
        }
        snapshot->values = new_values;
        snapshot->capacity = new_capacity;
    }

    name_copy = cbt_strdup(name);
    if (name_copy == NULL) {
        return CBT_E_INTERNAL;
    }

    index = snapshot->count;
    snapshot->names[index] = name_copy;
    memset(&snapshot->values[index], 0, sizeof(cbt_value));

    {
        cbt_status status = cbt_value_copy(&snapshot->values[index], value);
        if (status != CBT_OK) {
            free(name_copy);
            snapshot->names[index] = NULL;
            return status;
        }
    }

    snapshot->count++;
    return CBT_OK;
}
