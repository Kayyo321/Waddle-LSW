/** @file backend.c @brief Adapter boundary, output bounds, cancellation and store tests. */
#include "gui_backend.h"
#include "gui_store.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
/** @brief Stack test context, lives through its main-loop completion. */
typedef struct result_t { gboolean done; int status; gsize length; gboolean truncated; guint chunks; } result_t;
static void output(const char *text, gpointer context) {
    result_t *result = context; g_assert_true(g_utf8_validate(text, -1, NULL)); ++result->chunks;
}
static void complete(int status, const char *text, const char *error, gpointer context) {
    result_t *result = context; result->status = status; result->length = strlen(text); result->done = TRUE; result->truncated = error != NULL;
}
static void drain(result_t *result) {
    gint64 deadline = g_get_monotonic_time() + 12 * G_USEC_PER_SEC;
    while (!result->done && g_get_monotonic_time() < deadline) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
    g_assert_true(result->done);
}
static void parse_test(void) {
    const char *valid = "{\"schema_version\":1,\"ok\":true,\"results\":[{\"name\":\"vm\",\"ok\":true,\"data\":{\"state\":\"stopped\"}}]}";
    GError *error = NULL; JsonParser *parser = gui_result_parse(valid, &error); g_assert_no_error(error); g_assert_nonnull(parser); g_object_unref(parser);
    const char *invalid[] = {"[]", "null", "{}", "{", "{\"schema_version\":2,\"ok\":true,\"results\":[]}",
        "{\"schema_version\":1,\"ok\":false,\"results\":[]}", "{\"schema_version\":1,\"ok\":true,\"results\":[null]}",
        "{\"schema_version\":1,\"ok\":true,\"results\":[{\"name\":\"../x\",\"ok\":true,\"data\":{}}]}",
        "{\"schema_version\":1,\"ok\":true,\"results\":[{\"name\":\"vm\",\"ok\":1,\"data\":{}}]}"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) { g_assert_null(gui_result_parse(invalid[i], &error)); g_assert_nonnull(error); g_clear_error(&error); }
    g_assert_false(gui_name_valid(NULL)); g_assert_false(gui_name_valid("")); g_assert_false(gui_name_valid("a/b"));
    g_assert_false(gui_name_valid("é")); g_assert_true(gui_name_valid("Abc_12-3"));
    g_assert_cmpstr(gui_json_text(NULL, "x", "fallback"), ==, "fallback");
}
static void jobs_test(void) {
    char *fixture = g_canonicalize_filename("tests/gui/fixture.py", NULL);
    GError *error = NULL; result_t result = {0}; const char *argv[] = {fixture, "large", NULL};
    g_assert_nonnull(gui_job_start(argv, output, complete, &result, &error)); g_assert_no_error(error); drain(&result);
    g_assert_cmpint(result.status, ==, 0); g_assert_cmpuint(result.length, ==, 2U * 1024U * 1024U); g_assert_true(result.truncated); g_assert_cmpuint(result.chunks, >, 1);
    result = (result_t){0}; const char *failure[] = {fixture, "fail", NULL};
    g_assert_nonnull(gui_job_start(failure, NULL, complete, &result, &error)); drain(&result); g_assert_cmpint(result.status, ==, 7);
    result = (result_t){0}; const char *long_run[] = {fixture, "long", NULL};
    gui_job_t *job = gui_job_start(long_run, output, complete, &result, &error); g_assert_nonnull(job);
    gui_job_stop(job); gui_job_stop(job); drain(&result); g_assert_cmpint(result.status, !=, 0);
    const char *missing[] = {"/waddle/no-such-executable", NULL};
    g_assert_null(gui_job_start(missing, NULL, complete, &result, &error)); g_assert_nonnull(error); g_clear_error(&error);
    gui_job_stop(NULL); char *sibling = gui_sibling_path("waddle"); g_assert_true(g_str_has_suffix(sibling, "/waddle")); g_free(sibling); g_free(fixture);
}
static void store_test(void) {
    GError *error = NULL; gui_store_t *store = gui_store_load(&error); g_assert_no_error(error);
    g_assert_cmpint(gui_store_number(store, "missing", 8, 1, 10), ==, 8);
    g_key_file_set_integer(store->keys, "Preferences", "number", 99); g_assert_cmpint(gui_store_number(store, "number", 8, 1, 10), ==, 10);
    const char *args[] = {"program path.exe", "a b", "$(literal)", "quote'\""};
    g_key_file_set_string_list(store->keys, "Launcher test", "argv", args, G_N_ELEMENTS(args));
    g_key_file_set_string(store->keys, "Session", "must_not_persist", "start");
    g_assert_true(gui_store_save(store, &error)); g_assert_no_error(error);
    struct stat status; g_assert_cmpint(stat(store->path, &status), ==, 0); g_assert_cmpint(status.st_mode & 0777, ==, 0600);
    gui_store_t *copy = gui_store_load(&error); g_assert_no_error(error);
    g_assert_false(g_key_file_has_group(copy->keys, "Session"));
    gsize count; char **loaded = g_key_file_get_string_list(copy->keys, "Launcher test", "argv", &count, &error);
    g_assert_no_error(error); g_assert_cmpuint(count, ==, G_N_ELEMENTS(args));
    for (gsize i = 0; i < count; ++i) g_assert_cmpstr(args[i], ==, loaded[i]);
    g_strfreev(loaded); gui_store_free(copy);
    g_assert_true(g_file_set_contents(store->path, "[broken", -1, &error)); g_assert_no_error(error);
    copy = gui_store_load(&error); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(gui_store_save(copy, &error)); g_assert_nonnull(error); g_clear_error(&error);
    gui_store_free(copy); gui_store_free(store);
}
/** @brief Run deterministic no-display adapter tests; returns GLib test status. */
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    char *root = g_dir_make_tmp("waddle-gui-backend-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", root, TRUE); g_setenv("WADDLE_GUI_TEST_DIR", root, TRUE);
    g_test_add_func("/gui/schema", parse_test); g_test_add_func("/gui/jobs", jobs_test); g_test_add_func("/gui/store", store_test);
    int status = g_test_run();
    char *config = g_build_filename(root, "waddle", "gui.ini", NULL); g_remove(config); g_free(config);
    char *directory = g_build_filename(root, "waddle", NULL); g_rmdir(directory); g_free(directory);
    char *calls = g_build_filename(root, "calls.jsonl", NULL); g_remove(calls); g_free(calls); g_rmdir(root); g_free(root);
    return status;
}
