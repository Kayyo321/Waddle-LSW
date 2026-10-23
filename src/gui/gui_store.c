/** @file gui_store.c @brief Bounded preferences with atomic private writes. */
#include "gui_store.h"
#include <glib/gstdio.h>
#include <sys/stat.h>

gui_store_t *gui_store_load(GError **error) {
    gui_store_t *store = g_new0(gui_store_t, 1);
    store->keys = g_key_file_new();
    store->writable = TRUE;
    store->path = g_build_filename(g_get_user_config_dir(), "waddle", "gui.ini", NULL);
    GStatBuf status;
    if (g_stat(store->path, &status) == 0 && status.st_size > 1024 * 1024) {
        store->writable = FALSE;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "GUI preferences exceed 1 MiB; move gui.ini aside to recover.");
    } else {
        GError *local = NULL;
        if (!g_key_file_load_from_file(store->keys, store->path, G_KEY_FILE_NONE, &local)) {
            if (!g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { store->writable = FALSE; g_propagate_error(error, local); }
            else g_clear_error(&local);
        }
    }
    return store;
}
gboolean gui_store_save(gui_store_t *store, GError **error) {
    if (!store->writable) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Preferences are not saved because gui.ini could not be loaded. Move the damaged file aside, then reopen Waddle.");
        return FALSE;
    }
    char *directory = g_path_get_dirname(store->path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Cannot create Waddle preference directory.");
        g_free(directory);
        return FALSE;
    }
    g_free(directory);
    gsize length;
    char *snapshot = g_key_file_to_data(store->keys, &length, error);
    if (!snapshot) return FALSE;
    GKeyFile *persistent = g_key_file_new();
    if (!g_key_file_load_from_data(persistent, snapshot, length, G_KEY_FILE_NONE, error)) {
        g_free(snapshot); g_key_file_unref(persistent); return FALSE;
    }
    g_free(snapshot);
    g_key_file_remove_group(persistent, "Session", NULL);
    char *data = g_key_file_to_data(persistent, &length, error);
    g_key_file_unref(persistent);
    if (!data) return FALSE;
    gboolean ok = length <= 1024 * 1024 && g_file_set_contents_full(store->path, data, (gssize)length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    if (length > 1024 * 1024) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "GUI preferences exceed 1 MiB.");
    g_free(data);
    return ok;
}
void gui_store_free(gui_store_t *store) {
    if (!store) return;
    g_key_file_unref(store->keys);
    g_free(store->path);
    g_free(store);
}
char *gui_store_text(gui_store_t *store, const char *group, const char *key, const char *fallback) {
    char *text = g_key_file_get_string(store->keys, group, key, NULL);
    return text ? text : g_strdup(fallback);
}
int gui_store_number(gui_store_t *store, const char *key, int fallback, int minimum, int maximum) {
    GError *error = NULL;
    int value = g_key_file_get_integer(store->keys, "Preferences", key, &error);
    if (error) { g_clear_error(&error); return fallback; }
    return CLAMP(value, minimum, maximum);
}
