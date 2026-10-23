#define WIN32_LEAN_AND_MEAN
#include "loader_environment_windows.h"

#include <string.h>

static const WCHAR *const LoaderEnvironmentNames[WaddleLoaderEnvironmentVariableCount] = {
    L"VK_DRIVER_FILES",
    L"VK_ICD_FILENAMES",
    L"VK_ADD_DRIVER_FILES",
    L"VK_LOADER_LAYERS_DISABLE"
};

static BOOL valid_api(const waddle_loader_environment_api_t *api)
{
    return api && api->get && api->set && api->get_last_error && api->set_last_error;
}

static waddle_loader_environment_status_t bounded_value_length(LPCWSTR value, DWORD *length)
{
    if (!value || !length) return WaddleLoaderEnvironmentInvalidArgument;
    for (DWORD index = 0; index < WaddleLoaderEnvironmentMaxValueUnits; ++index) {
        if (value[index] == L'\0') {
            *length = index;
            return WaddleLoaderEnvironmentOk;
        }
    }
    return WaddleLoaderEnvironmentValueTooLong;
}

static waddle_loader_environment_status_t capture_value(
    waddle_loader_environment_guard_t *guard, DWORD index)
{
    waddle_loader_environment_value_t *saved = &guard->saved[index];
    guard->api.set_last_error(ERROR_SUCCESS);
    DWORD length = guard->api.get(LoaderEnvironmentNames[index], saved->value,
                                  WaddleLoaderEnvironmentMaxValueUnits);
    if (length == 0) {
        DWORD error = guard->api.get_last_error();
        if (error == ERROR_ENVVAR_NOT_FOUND) {
            saved->present = FALSE;
            saved->length = 0;
            saved->value[0] = L'\0';
            return WaddleLoaderEnvironmentOk;
        }
        if (error == ERROR_SUCCESS) {
            saved->present = TRUE;
            saved->length = 0;
            saved->value[0] = L'\0';
            return WaddleLoaderEnvironmentOk;
        }
        return WaddleLoaderEnvironmentCaptureFailed;
    }
    if (length >= WaddleLoaderEnvironmentMaxValueUnits)
        return WaddleLoaderEnvironmentValueTooLong;
    if (saved->value[length] != L'\0')
        return WaddleLoaderEnvironmentCaptureFailed;
    saved->present = TRUE;
    saved->length = length;
    return WaddleLoaderEnvironmentOk;
}

void waddle_loader_environment_guard_init(waddle_loader_environment_guard_t *guard)
{
    if (guard) memset(guard, 0, sizeof *guard);
}

waddle_loader_environment_api_t waddle_loader_environment_windows_api(void)
{
    const waddle_loader_environment_api_t api = {
        .get = GetEnvironmentVariableW,
        .set = SetEnvironmentVariableW,
        .get_last_error = GetLastError,
        .set_last_error = SetLastError
    };
    return api;
}

waddle_loader_environment_status_t waddle_loader_environment_begin(
    waddle_loader_environment_guard_t *guard,
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount],
    const waddle_loader_environment_api_t *api)
{
    if (!guard || !overrides || !valid_api(api))
        return WaddleLoaderEnvironmentInvalidArgument;
    if (guard->active)
        return WaddleLoaderEnvironmentAlreadyActive;
    /* Preserve a caller-supplied table even when api points into guard.api. */
    const waddle_loader_environment_api_t api_copy = *api;

    DWORD override_lengths[WaddleLoaderEnvironmentVariableCount] = {0};
    for (DWORD index = 0; index < WaddleLoaderEnvironmentVariableCount; ++index) {
        if (overrides[index].present != FALSE && overrides[index].present != TRUE)
            return WaddleLoaderEnvironmentInvalidArgument;
        if (overrides[index].present) {
            waddle_loader_environment_status_t status =
                bounded_value_length(overrides[index].value, &override_lengths[index]);
            if (status != WaddleLoaderEnvironmentOk)
                return status;
        }
    }

    memset(guard, 0, sizeof *guard);
    guard->api = api_copy;
    for (DWORD index = 0; index < WaddleLoaderEnvironmentVariableCount; ++index) {
        waddle_loader_environment_status_t status = capture_value(guard, index);
        if (status != WaddleLoaderEnvironmentOk) {
            memset(guard, 0, sizeof *guard);
            return status;
        }
    }

    guard->active = TRUE;
    for (DWORD index = 0; index < WaddleLoaderEnvironmentVariableCount; ++index) {
        LPCWSTR value = overrides[index].present ? overrides[index].value : NULL;
        if (!guard->api.set(LoaderEnvironmentNames[index], value)) {
            waddle_loader_environment_status_t rollback =
                waddle_loader_environment_restore(guard);
            return rollback == WaddleLoaderEnvironmentOk ?
                WaddleLoaderEnvironmentApplyFailed : WaddleLoaderEnvironmentRollbackFailed;
        }
    }
    return WaddleLoaderEnvironmentOk;
}

waddle_loader_environment_status_t waddle_loader_environment_restore(
    waddle_loader_environment_guard_t *guard)
{
    if (!guard)
        return WaddleLoaderEnvironmentInvalidArgument;
    if (!guard->active)
        return WaddleLoaderEnvironmentOk;
    if (!valid_api(&guard->api))
        return WaddleLoaderEnvironmentInvalidArgument;

    uint32_t failed_mask = 0;
    for (DWORD index = 0; index < WaddleLoaderEnvironmentVariableCount; ++index) {
        const waddle_loader_environment_value_t *saved = &guard->saved[index];
        LPCWSTR value = saved->present ? saved->value : NULL;
        if (!guard->api.set(LoaderEnvironmentNames[index], value))
            failed_mask |= (uint32_t)1u << index;
    }
    guard->restore_failed_mask = failed_mask;
    if (failed_mask != 0)
        return WaddleLoaderEnvironmentRestoreFailed;

    memset(guard, 0, sizeof *guard);
    return WaddleLoaderEnvironmentOk;
}

#if defined(WaddleLoaderEnvironmentTest)

#include <stdio.h>
#include <wchar.h>

static waddle_loader_environment_status_t restore_with_retries(
    waddle_loader_environment_guard_t *guard)
{
    waddle_loader_environment_status_t status = WaddleLoaderEnvironmentRestoreFailed;
    for (unsigned attempt = 0; attempt < 4 && guard->active; ++attempt) {
        status = waddle_loader_environment_restore(guard);
        if (status == WaddleLoaderEnvironmentOk) return status;
        fprintf(stderr,"native restore retry status=%d mask=%lu error=%lu\n",(int)status,(unsigned long)guard->restore_failed_mask,(unsigned long)GetLastError());
    }
    return guard->active ? WaddleLoaderEnvironmentRestoreFailed : WaddleLoaderEnvironmentOk;
}

typedef struct mock_value_t {
    BOOL present;
    WCHAR value[WaddleLoaderEnvironmentMaxValueUnits];
} mock_value_t;

static mock_value_t mock_values[WaddleLoaderEnvironmentVariableCount];
static DWORD mock_last_error;
static DWORD mock_get_calls;
static DWORD mock_set_calls;
static DWORD mock_fail_get_call;
static DWORD mock_oversized_get_call;
static DWORD mock_missing_nul_get_call;
static DWORD mock_fail_set_calls[2];
static DWORD mock_mutate_failed_set_call;

static int name_index(LPCWSTR name)
{
    for (int index = 0; index < (int)WaddleLoaderEnvironmentVariableCount; ++index)
        if (wcscmp(name, LoaderEnvironmentNames[index]) == 0) return index;
    return -1;
}

static DWORD WINAPI mock_get(LPCWSTR name, LPWSTR value, DWORD capacity)
{
    ++mock_get_calls;
    if (mock_fail_get_call == mock_get_calls) {
        mock_last_error = ERROR_ACCESS_DENIED;
        return 0;
    }
    if (mock_oversized_get_call == mock_get_calls) {
        mock_last_error = ERROR_SUCCESS;
        return capacity;
    }
    if (mock_missing_nul_get_call == mock_get_calls) {
        if (capacity < 4) {
            mock_last_error = ERROR_ACCESS_DENIED;
            return capacity + 1;
        }
        value[0] = L'b';
        value[1] = L'a';
        value[2] = L'd';
        value[3] = L'!';
        mock_last_error = ERROR_SUCCESS;
        return 3;
    }
    int index = name_index(name);
    if (index < 0) {
        mock_last_error = ERROR_ENVVAR_NOT_FOUND;
        return 0;
    }
    mock_value_t *source = &mock_values[index];
    if (!source->present) {
        mock_last_error = ERROR_ENVVAR_NOT_FOUND;
        return 0;
    }
    size_t length = wcslen(source->value);
    if (length >= capacity) {
        mock_last_error = ERROR_SUCCESS;
        return (DWORD)length + 1;
    }
    if (length) memcpy(value, source->value, (length + 1) * sizeof *value);
    else value[0] = L'\0';
    mock_last_error = ERROR_SUCCESS;
    return (DWORD)length;
}

static BOOL mock_store(int index, LPCWSTR value)
{
    mock_value_t *target = &mock_values[index];
    if (!value) {
        target->present = FALSE;
        target->value[0] = L'\0';
        return TRUE;
    }
    DWORD length = 0;
    if (bounded_value_length(value, &length) != WaddleLoaderEnvironmentOk)
        return FALSE;
    memcpy(target->value, value, ((size_t)length + 1) * sizeof *value);
    target->present = TRUE;
    return TRUE;
}

static BOOL WINAPI mock_set(LPCWSTR name, LPCWSTR value)
{
    ++mock_set_calls;
    int index = name_index(name);
    if (index < 0) {
        mock_last_error = ERROR_ENVVAR_NOT_FOUND;
        return FALSE;
    }
    if (mock_fail_set_calls[0] == mock_set_calls || mock_fail_set_calls[1] == mock_set_calls) {
        if (mock_mutate_failed_set_call == mock_set_calls) (void)mock_store(index, value);
        mock_last_error = ERROR_ACCESS_DENIED;
        return FALSE;
    }
    if (!mock_store(index, value)) {
        mock_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    mock_last_error = ERROR_SUCCESS;
    return TRUE;
}

static DWORD WINAPI mock_get_last_error(void)
{
    return mock_last_error;
}

static void WINAPI mock_set_last_error(DWORD error)
{
    mock_last_error = error;
}

static void reset_mock(void)
{
    memset(mock_values, 0, sizeof mock_values);
    mock_last_error = ERROR_SUCCESS;
    mock_get_calls = 0;
    mock_set_calls = 0;
    mock_fail_get_call = 0;
    mock_oversized_get_call = 0;
    mock_missing_nul_get_call = 0;
    mock_fail_set_calls[0] = 0;
    mock_fail_set_calls[1] = 0;
    mock_mutate_failed_set_call = 0;
}

static BOOL mock_equal(DWORD index, BOOL present, LPCWSTR value)
{
    if (mock_values[index].present != present) return FALSE;
    if (!present) return TRUE;
    return wcscmp(mock_values[index].value, value) == 0;
}

static int test_mock_capture_failure(void)
{
    reset_mock();
    (void)mock_store(0, L"original-driver");
    mock_values[1].present = FALSE;
    (void)mock_store(2, L"");
    (void)mock_store(3, L"original-layers");
    mock_fail_get_call = 3;
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"new-driver"}, {FALSE, NULL}, {TRUE, L"new-empty"}, {TRUE, L"new-layers"}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentCaptureFailed)
        return 1;
    if (mock_set_calls != 0 || guard.active) return 1;
    return mock_equal(0, TRUE, L"original-driver") && mock_equal(1, FALSE, NULL) &&
        mock_equal(2, TRUE, L"") && mock_equal(3, TRUE, L"original-layers") ? 0 : 1;
}

static int test_mock_malformed_capture(void)
{
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"replacement"}, {FALSE, NULL}, {FALSE, NULL}, {FALSE, NULL}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};

    reset_mock();
    (void)mock_store(0, L"original-driver");
    (void)mock_store(1, L"");
    (void)mock_store(3, L"original-layers");
    mock_oversized_get_call = 2;
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentValueTooLong ||
        guard.active || mock_set_calls != 0 ||
        !mock_equal(0, TRUE, L"original-driver") || !mock_equal(1, TRUE, L"") ||
        !mock_equal(2, FALSE, NULL) || !mock_equal(3, TRUE, L"original-layers")) return 1;

    mock_oversized_get_call = 0;
    mock_missing_nul_get_call = 3;
    mock_get_calls = 0;
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentCaptureFailed ||
        guard.active || mock_set_calls != 0 ||
        !mock_equal(0, TRUE, L"original-driver") || !mock_equal(1, TRUE, L"") ||
        !mock_equal(2, FALSE, NULL) || !mock_equal(3, TRUE, L"original-layers")) return 1;
    return 0;
}

static int test_mock_api_and_guard_edges(void)
{
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"new-driver"}, {FALSE, NULL}, {FALSE, NULL}, {FALSE, NULL}
    };
    const waddle_loader_environment_api_t complete = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard;

    reset_mock();
    memset(&guard, 0xa5, sizeof guard);
    waddle_loader_environment_guard_init(&guard);
    if (guard.active || guard.restore_failed_mask != 0 || guard.api.get || guard.api.set ||
        guard.api.get_last_error || guard.api.set_last_error) return 1;
    waddle_loader_environment_guard_init(NULL);
    if (waddle_loader_environment_restore(NULL) != WaddleLoaderEnvironmentInvalidArgument ||
        waddle_loader_environment_restore(&guard) != WaddleLoaderEnvironmentOk) return 1;

    waddle_loader_environment_api_t invalid = complete;
    invalid.get = NULL;
    if (waddle_loader_environment_begin(&guard, overrides, &invalid) != WaddleLoaderEnvironmentInvalidArgument)
        return 1;
    invalid = complete; invalid.set = NULL;
    if (waddle_loader_environment_begin(&guard, overrides, &invalid) != WaddleLoaderEnvironmentInvalidArgument)
        return 1;
    invalid = complete; invalid.get_last_error = NULL;
    if (waddle_loader_environment_begin(&guard, overrides, &invalid) != WaddleLoaderEnvironmentInvalidArgument)
        return 1;
    invalid = complete; invalid.set_last_error = NULL;
    if (waddle_loader_environment_begin(&guard, overrides, &invalid) != WaddleLoaderEnvironmentInvalidArgument)
        return 1;
    if (waddle_loader_environment_begin(NULL, overrides, &complete) != WaddleLoaderEnvironmentInvalidArgument ||
        waddle_loader_environment_begin(&guard, NULL, &complete) != WaddleLoaderEnvironmentInvalidArgument)
        return 1;

    waddle_loader_environment_override_t invalid_override[WaddleLoaderEnvironmentVariableCount] = {
        {2, L"invalid-bool"}, {FALSE, NULL}, {FALSE, NULL}, {FALSE, NULL}
    };
    if (waddle_loader_environment_begin(&guard, invalid_override, &complete) !=
        WaddleLoaderEnvironmentInvalidArgument) return 1;

    const waddle_loader_environment_api_t native = waddle_loader_environment_windows_api();
    if (!native.get || !native.set || !native.get_last_error || !native.set_last_error) return 1;
#if defined(WaddleLoaderEnvironmentFakeWindows)
    native.set_last_error(123u);
    if (native.get_last_error() != 123u) return 1;
#endif
    if (mock_get_calls != 0 || mock_set_calls != 0) return 1;
    return 0;
}

static int test_mock_invalid_active_api_retry(void)
{
    reset_mock();
    (void)mock_store(0, L"original-driver");
    (void)mock_store(1, L"");
    mock_values[2].present = FALSE;
    (void)mock_store(3, L"original-layers");
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"replacement"}, {FALSE, NULL}, {TRUE, L"added"}, {FALSE, NULL}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentOk) return 1;

    const waddle_loader_environment_value_t saved_before = guard.saved[0];
    DWORD writes_before = mock_set_calls;
    guard.api.set_last_error = NULL;
    if (waddle_loader_environment_restore(&guard) != WaddleLoaderEnvironmentInvalidArgument ||
        !guard.active || guard.restore_failed_mask != 0 || mock_set_calls != writes_before ||
        guard.saved[0].present != saved_before.present || guard.saved[0].length != saved_before.length ||
        wcscmp(guard.saved[0].value, saved_before.value) != 0 ||
        !mock_equal(0, TRUE, L"replacement") || !mock_equal(1, FALSE, NULL) ||
        !mock_equal(2, TRUE, L"added") || !mock_equal(3, FALSE, NULL)) return 1;

    guard.api = api;
    if (waddle_loader_environment_restore(&guard) != WaddleLoaderEnvironmentOk || guard.active) return 1;
    return mock_equal(0, TRUE, L"original-driver") && mock_equal(1, TRUE, L"") &&
        mock_equal(2, FALSE, NULL) && mock_equal(3, TRUE, L"original-layers") ? 0 : 1;
}

static int test_mock_bounded_length_arguments(void)
{
    DWORD length = 99;
    if (bounded_value_length(L"x", &length) != WaddleLoaderEnvironmentOk || length != 1) return 1;
    if (bounded_value_length(NULL, &length) != WaddleLoaderEnvironmentInvalidArgument) return 1;
    return bounded_value_length(L"x", NULL) == WaddleLoaderEnvironmentInvalidArgument ? 0 : 1;
}

static int test_mock_oversize_before_capture(void)
{
    static WCHAR oversized[WaddleLoaderEnvironmentMaxValueUnits];
    for (DWORD index = 0; index < WaddleLoaderEnvironmentMaxValueUnits; ++index)
        oversized[index] = L'x';
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, oversized}, {FALSE, NULL}, {FALSE, NULL}, {FALSE, NULL}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};
    reset_mock();
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentValueTooLong)
        return 1;
    return mock_get_calls == 0 && mock_set_calls == 0 && !guard.active ? 0 : 1;
}

static int test_mock_maximum_value_and_api_alias(void)
{
    static WCHAR maximum[WaddleLoaderEnvironmentMaxValueUnits];
    for (DWORD index = 0; index + 1 < WaddleLoaderEnvironmentMaxValueUnits; ++index)
        maximum[index] = L'm';
    maximum[WaddleLoaderEnvironmentMaxValueUnits - 1] = L'\0';
    reset_mock();
    (void)mock_store(0, maximum);
    mock_values[1].present = FALSE;
    mock_values[2].present = FALSE;
    mock_values[3].present = FALSE;
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, maximum}, {FALSE, NULL}, {FALSE, NULL}, {FALSE, NULL}
    };
    waddle_loader_environment_guard_t guard = {0};
    guard.api = (waddle_loader_environment_api_t){mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    if (waddle_loader_environment_begin(&guard, overrides, &guard.api) != WaddleLoaderEnvironmentOk)
        return 1;
    if (guard.saved[0].length != WaddleLoaderEnvironmentMaxValueUnits - 1 ||
        !mock_equal(0, TRUE, maximum)) return 1;
    if (waddle_loader_environment_restore(&guard) != WaddleLoaderEnvironmentOk || guard.active)
        return 1;
    return mock_equal(0, TRUE, maximum) && mock_equal(1, FALSE, NULL) &&
        mock_equal(2, FALSE, NULL) && mock_equal(3, FALSE, NULL) ? 0 : 1;
}

static int test_mock_invalid_and_active(void)
{
    reset_mock();
    (void)mock_store(0, L"original-driver");
    const waddle_loader_environment_override_t valid[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"new-driver"}, {FALSE, NULL}, {FALSE, NULL}, {FALSE, NULL}
    };
    const waddle_loader_environment_override_t invalid[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, NULL}, {FALSE, NULL}, {FALSE, NULL}, {FALSE, NULL}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};
    if (waddle_loader_environment_begin(&guard, invalid, &api) != WaddleLoaderEnvironmentInvalidArgument ||
        mock_get_calls || mock_set_calls) return 1;
    if (waddle_loader_environment_begin(&guard, valid, NULL) != WaddleLoaderEnvironmentInvalidArgument ||
        mock_get_calls || mock_set_calls) return 1;
    if (waddle_loader_environment_begin(&guard, valid, &api) != WaddleLoaderEnvironmentOk)
        return 1;
    DWORD reads = mock_get_calls, writes = mock_set_calls;
    if (waddle_loader_environment_begin(&guard, valid, &api) != WaddleLoaderEnvironmentAlreadyActive ||
        mock_get_calls != reads || mock_set_calls != writes) return 1;
    return waddle_loader_environment_restore(&guard) == WaddleLoaderEnvironmentOk ? 0 : 1;
}

static int test_mock_apply_rollback(void)
{
    reset_mock();
    (void)mock_store(0, L"original-driver");
    mock_values[1].present = FALSE;
    (void)mock_store(2, L"");
    (void)mock_store(3, L"original-layers");
    mock_fail_set_calls[0] = 3;
    mock_mutate_failed_set_call = 3;
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"new-driver"}, {TRUE, L"new-icd"}, {TRUE, L"new-add"}, {TRUE, L"new-layers"}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentApplyFailed)
        return 1;
    if (mock_set_calls != 7 || guard.active) return 1;
    return mock_equal(0, TRUE, L"original-driver") && mock_equal(1, FALSE, NULL) &&
        mock_equal(2, TRUE, L"") && mock_equal(3, TRUE, L"original-layers") ? 0 : 1;
}

static int test_mock_restore_retry(void)
{
    reset_mock();
    (void)mock_store(0, L"original-driver");
    (void)mock_store(1, L"original-icd");
    mock_values[2].present = FALSE;
    (void)mock_store(3, L"");
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"new-driver"}, {TRUE, L"new-icd"}, {TRUE, L"new-add"}, {FALSE, NULL}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentOk)
        return 1;
    mock_fail_set_calls[0] = mock_set_calls + 2;
    if (waddle_loader_environment_restore(&guard) != WaddleLoaderEnvironmentRestoreFailed || !guard.active)
        return 1;
    if (mock_set_calls != 8 || guard.restore_failed_mask != ((uint32_t)1u << 1)) return 1;
    if (!mock_equal(0, TRUE, L"original-driver") || mock_equal(1, TRUE, L"original-icd") ||
        !mock_equal(2, FALSE, NULL) || !mock_equal(3, TRUE, L"")) return 1;
    mock_fail_set_calls[0] = 0;
    if (waddle_loader_environment_restore(&guard) != WaddleLoaderEnvironmentOk || guard.active)
        return 1;
    if(restore_with_retries(&guard)!=WaddleLoaderEnvironmentOk)return 1;
    return mock_equal(0, TRUE, L"original-driver") && mock_equal(1, TRUE, L"original-icd") &&
        mock_equal(2, FALSE, NULL) && mock_equal(3, TRUE, L"") ? 0 : 1;
}

static int test_mock_apply_rollback_retry(void)
{
    reset_mock();
    (void)mock_store(0, L"original-driver");
    mock_values[1].present = FALSE;
    (void)mock_store(2, L"");
    (void)mock_store(3, L"original-layers");
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"new-driver"}, {TRUE, L"new-icd"}, {TRUE, L"new-add"}, {TRUE, L"new-layers"}
    };
    const waddle_loader_environment_api_t api = {mock_get, mock_set, mock_get_last_error, mock_set_last_error};
    waddle_loader_environment_guard_t guard = {0};
    mock_fail_set_calls[0] = 2;
    mock_fail_set_calls[1] = 4;
    mock_mutate_failed_set_call = 2;
    if (waddle_loader_environment_begin(&guard, overrides, &api) != WaddleLoaderEnvironmentRollbackFailed)
        return 1;
    if (!guard.active || guard.restore_failed_mask != ((uint32_t)1u << 1) || mock_set_calls != 6)
        return 1;
    if (!mock_equal(0, TRUE, L"original-driver") || !mock_equal(1, TRUE, L"new-icd") ||
        !mock_equal(2, TRUE, L"") || !mock_equal(3, TRUE, L"original-layers")) return 1;
    mock_fail_set_calls[0] = 0;
    mock_fail_set_calls[1] = 0;
    mock_mutate_failed_set_call = 0;
    if (waddle_loader_environment_restore(&guard) != WaddleLoaderEnvironmentOk || guard.active)
        return 1;
    if(restore_with_retries(&guard)!=WaddleLoaderEnvironmentOk)return 1;
    return mock_equal(0, TRUE, L"original-driver") && mock_equal(1, FALSE, NULL) &&
        mock_equal(2, TRUE, L"") && mock_equal(3, TRUE, L"original-layers") ? 0 : 1;
}

#if !defined(WaddleLoaderEnvironmentFakeWindows)
static BOOL read_real_value(LPCWSTR name, waddle_loader_environment_value_t *value)
{
    memset(value, 0, sizeof *value);
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(name, value->value, WaddleLoaderEnvironmentMaxValueUnits);
    if (length == 0) {
        DWORD error = GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND) return TRUE;
        if (error != ERROR_SUCCESS) return FALSE;
        value->present = TRUE;
        value->length = 0;
        return TRUE;
    }
    if (length >= WaddleLoaderEnvironmentMaxValueUnits || value->value[length] != L'\0') return FALSE;
    value->present = TRUE;
    value->length = length;
    return TRUE;
}

static BOOL real_value_equal(LPCWSTR name, BOOL present, LPCWSTR expected)
{
    waddle_loader_environment_value_t actual;
    BOOL read=read_real_value(name,&actual);
    BOOL equal=read && actual.present==present && (!present || wcscmp(actual.value,expected)==0);
    if(!equal)fprintf(stderr,"real environment mismatch expected_present=%d actual_present=%d length=%lu read=%d error=%lu\n",(int)present,(int)actual.present,(unsigned long)actual.length,(int)read,(unsigned long)GetLastError());
    return equal;
}

static BOOL restore_unrelated(LPCWSTR name, const waddle_loader_environment_value_t *saved)
{
    return SetEnvironmentVariableW(name, saved->present ? saved->value : NULL);
}


static int test_real_environment_round_trip(void)
{
    static const WCHAR unrelated_name[] = L"WADDLE_LOADER_ENVIRONMENT_UNRELATED_SENTINEL";
    static waddle_loader_environment_value_t original_values[WaddleLoaderEnvironmentVariableCount];
    waddle_loader_environment_value_t unrelated_saved;
    waddle_loader_environment_guard_t outer_guard = {0};
    waddle_loader_environment_guard_t inner_guard = {0};
    const waddle_loader_environment_api_t api = waddle_loader_environment_windows_api();
    const waddle_loader_environment_override_t fixture[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"fixture-driver"}, {FALSE, NULL}, {TRUE, L""}, {TRUE, L"~all~"}
    };
    const waddle_loader_environment_override_t nested[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, L"nested-driver"}, {TRUE, L""}, {FALSE, NULL}, {TRUE, L"日本語"}
    };
    int result = 1;
    const char *stage="snapshot original values";
    BOOL unrelated_saved_valid = FALSE;
    BOOL original_values_captured = FALSE;
    for (DWORD index = 0; index < WaddleLoaderEnvironmentVariableCount; ++index)
        if (!read_real_value(LoaderEnvironmentNames[index], &original_values[index])) goto cleanup;
    original_values_captured = TRUE;
    stage="snapshot unrelated";
    if (!read_real_value(unrelated_name, &unrelated_saved)) goto cleanup;
    unrelated_saved_valid = TRUE;
    stage="set unrelated";
    if (!SetEnvironmentVariableW(unrelated_name, L"unrelated-kept")) goto cleanup;
    stage="begin outer";
    waddle_loader_environment_status_t observed=waddle_loader_environment_begin(&outer_guard,fixture,&api);
    if(observed!=WaddleLoaderEnvironmentOk){fprintf(stderr,"native outer status=%d error=%lu active=%d mask=%lu\n",(int)observed,(unsigned long)GetLastError(),(int)outer_guard.active,(unsigned long)outer_guard.restore_failed_mask);goto cleanup;}
    if (!real_value_equal(LoaderEnvironmentNames[0], TRUE, L"fixture-driver") ||
        !real_value_equal(LoaderEnvironmentNames[1], FALSE, NULL) ||
        !real_value_equal(LoaderEnvironmentNames[2], TRUE, L"") ||
        !real_value_equal(LoaderEnvironmentNames[3], TRUE, L"~all~")) goto cleanup;
    stage="begin inner";
    observed=waddle_loader_environment_begin(&inner_guard,nested,&api);
    if(observed!=WaddleLoaderEnvironmentOk){fprintf(stderr,"native inner status=%d error=%lu active=%d mask=%lu\n",(int)observed,(unsigned long)GetLastError(),(int)inner_guard.active,(unsigned long)inner_guard.restore_failed_mask);goto cleanup;}
    if (!real_value_equal(unrelated_name, TRUE, L"unrelated-kept")) goto cleanup;
    if (waddle_loader_environment_restore(&inner_guard) != WaddleLoaderEnvironmentOk) goto cleanup;
    if (!real_value_equal(LoaderEnvironmentNames[0], TRUE, L"fixture-driver") ||
        !real_value_equal(LoaderEnvironmentNames[1], FALSE, NULL) ||
        !real_value_equal(LoaderEnvironmentNames[2], TRUE, L"") ||
        !real_value_equal(LoaderEnvironmentNames[3], TRUE, L"~all~") ||
        !real_value_equal(unrelated_name, TRUE, L"unrelated-kept")) goto cleanup;
    result = 0;
cleanup:
    if(result)fprintf(stderr,"native environment stage=%s error=%lu\n",stage,(unsigned long)GetLastError());
    if (restore_with_retries(&inner_guard) != WaddleLoaderEnvironmentOk) result = 1;
    if (restore_with_retries(&outer_guard) != WaddleLoaderEnvironmentOk) result = 1;
    if (unrelated_saved_valid) {
        waddle_loader_environment_value_t actual;
        BOOL restored=restore_unrelated(unrelated_name,&unrelated_saved);
        BOOL read=read_real_value(unrelated_name,&actual);
        if(!restored||!read||actual.present!=unrelated_saved.present||
           (actual.present&&wcscmp(actual.value,unrelated_saved.value)!=0)){
            fprintf(stderr,"unrelated restoration mismatch restored=%d read=%d expected_present=%d actual_present=%d error=%lu\n",(int)restored,(int)read,(int)unrelated_saved.present,(int)actual.present,(unsigned long)GetLastError());result=1;
        }
    }
    for (DWORD index = 0; original_values_captured && index < WaddleLoaderEnvironmentVariableCount; ++index) {
        const waddle_loader_environment_value_t *saved = &original_values[index];
        waddle_loader_environment_value_t actual;
        BOOL read=read_real_value(LoaderEnvironmentNames[index],&actual);
        if(!read||actual.present!=saved->present||
           (saved->present&&wcscmp(actual.value,saved->value)!=0)){
            fprintf(stderr,"original loader restoration mismatch key=%lu read=%d expected_present=%d actual_present=%d expected_length=%lu actual_length=%lu error=%lu\n",(unsigned long)index,(int)read,(int)saved->present,(int)actual.present,(unsigned long)saved->length,(unsigned long)actual.length,(unsigned long)GetLastError());result=1;
        }
    }
    return result;
}
#endif

int main(void)
{
#define RunCase(function) do { if(function()) {fprintf(stderr,"loader environment case failed: %s\n",#function);return 1;} } while(0)
    RunCase(test_mock_capture_failure);
    RunCase(test_mock_malformed_capture);
    RunCase(test_mock_oversize_before_capture);
    RunCase(test_mock_maximum_value_and_api_alias);
    RunCase(test_mock_invalid_and_active);
    RunCase(test_mock_api_and_guard_edges);
    RunCase(test_mock_invalid_active_api_retry);
    RunCase(test_mock_bounded_length_arguments);
    RunCase(test_mock_apply_rollback);
    RunCase(test_mock_apply_rollback_retry);
    RunCase(test_mock_restore_retry);
#if !defined(WaddleLoaderEnvironmentFakeWindows)
    RunCase(test_real_environment_round_trip);
#endif
#undef RunCase
    puts("loader environment guard test passed");
    return 0;
}
#endif
