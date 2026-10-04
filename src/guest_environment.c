/** @file guest_environment.c @brief Copy and merge Win32 child environment. */
#include "guest_environment.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

wchar_t *guest_utf16(const uint8_t *source, size_t length) {
    if (length > 1048576 || memchr(source, 0, length) != NULL) {
        SetLastError(ERROR_INVALID_DATA);
        return NULL;
    }
    if (length == 0) {
        wchar_t *empty = calloc(1, sizeof(*empty));
        if (empty == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); }
        return empty;
    }
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    (const char *)source, (int)length, NULL, 0);
    if (units == 0) { return NULL; }
    wchar_t *result = calloc((size_t)units + 1, sizeof(*result));
    if (result == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)source,
                           (int)length, result, units) == 0) {
        DWORD error = GetLastError();
        free(result);
        result = NULL;
        SetLastError(error);
    }
    return result;
}

static size_t key_length(const wchar_t *item) {
    const wchar_t *equal = wcschr(item + (item[0] == L'='), L'=');
    return equal == NULL ? wcslen(item) : (size_t)(equal - item);
}

static int same_key(const wchar_t *left, const wchar_t *right) {
    size_t left_len = key_length(left), right_len = key_length(right);
    return left_len == right_len && CompareStringOrdinal(left, (int)left_len,
               right, (int)right_len, TRUE) == CSTR_EQUAL;
}

static int compare_entries(const void *left, const void *right) {
    const wchar_t *a = *(const wchar_t *const *)left;
    const wchar_t *b = *(const wchar_t *const *)right;
    return CompareStringOrdinal(a, -1, b, -1, TRUE) - CSTR_EQUAL;
}

wchar_t *guest_environment(const guest_spawn_t *spawn) {
    wchar_t *inherited = GetEnvironmentStringsW();
    if (inherited == NULL) { return NULL; }
    // The OS owns inherited until FreeEnvironmentStringsW; every copied entry
    // below belongs to this function, and is freed after block creation or error.
    size_t count = 0, capacity = 16, total = 1;
    wchar_t **items = calloc(capacity, sizeof(*items));
    wchar_t *block = NULL;
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    if (items == NULL) { goto done; }
    for (const wchar_t *item = inherited; *item != 0; item += wcslen(item) + 1) {
        size_t length = wcslen(item) + 1;
        if (length > 1048576 || total > 1048576 - length) {
            error = ERROR_ENVVAR_NOT_FOUND;
            goto done;
        }
        total += length;
        if (count == capacity) {
            capacity *= 2;
            wchar_t **grown = realloc(items, capacity * sizeof(*items));
            if (grown == NULL) { goto done; }
            items = grown;
        }
        items[count] = malloc(length * sizeof(wchar_t));
        if (items[count] == NULL) { goto done; }
        memcpy(items[count++], item, length * sizeof(wchar_t));
    }
    for (size_t offset = 0; offset < spawn->environment_length;) {
        const uint8_t *item = spawn->environment + offset;
        size_t length = strlen((const char *)item);
        wchar_t *wide = guest_utf16(item, length);
        if (wide == NULL) { error = GetLastError(); goto done; }
        size_t index = 0;
        while (index < count && !same_key(items[index], wide)) { index++; }
        if (index < count) {
            free(items[index]);
            items[index] = NULL;
        } else {
            if (count == capacity) {
                capacity *= 2;
                wchar_t **grown = realloc(items, capacity * sizeof(*items));
                if (grown == NULL) { free(wide); wide = NULL; goto done; }
                items = grown;
            }
            count++;
        }
        items[index] = wide;
        offset += length + 1;
    }
    qsort(items, count, sizeof(*items), compare_entries);
    total = 1;
    for (size_t i = 0; i < count; i++) {
        size_t length = wcslen(items[i]) + 1;
        if (length > 2097152 || total > 2097152 - length) { goto done; }
        total += length;
    }
    if (total < 2) { total = 2; }
    block = calloc(total, sizeof(*block));
    if (block != NULL) {
        size_t offset = 0;
        for (size_t i = 0; i < count; i++) {
            size_t length = wcslen(items[i]) + 1;
            memcpy(block + offset, items[i], length * sizeof(*block));
            offset += length;
        }
    }
done:
    FreeEnvironmentStringsW(inherited);
    inherited = NULL;
    for (size_t i = 0; i < count; i++) { free(items[i]); items[i] = NULL; }
    free(items);
    items = NULL;
    if (block == NULL) { SetLastError(error); }
    return block;
}
