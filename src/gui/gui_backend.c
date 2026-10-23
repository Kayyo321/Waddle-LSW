/** @file gui_backend.c @brief Bounded asynchronous CLI output and schema adapter. */
#include "gui_backend.h"
#include <signal.h>
#include <string.h>
#include <unistd.h>

/** @brief Maximum retained output bytes; pipes continue draining after truncation. */
#define MaxOutputBytes (2U * 1024U * 1024U)
/** @brief Private job storage, freed only after both stream EOF and child reap. */
struct gui_job_t {
    GSubprocess *process;
    GCancellable *reader;
    GString *output;
    gui_output_fn_t on_output;
    gui_complete_fn_t on_complete;
    gpointer context;
    guint stop_timer;
    unsigned escalation;
    gboolean eof, waited, truncated;
    int status;
    char *error;
};

static void job_finish(gui_job_t *job) {
    if (!job->eof || !job->waited) return;
    if (job->stop_timer) g_source_remove(job->stop_timer);
    job->on_complete(job->status, job->output->str,
        job->error ? job->error : (job->truncated ? "Output exceeded 2 MiB; only its tail is retained." : NULL), job->context);
    g_clear_object(&job->process);
    g_clear_object(&job->reader);
    g_string_free(job->output, TRUE);
    g_free(job->error);
    g_free(job);
}
static void job_read(GObject *source, GAsyncResult *result, gpointer context) {
    gui_job_t *job = context;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    gsize count = 0;
    const char *data = bytes ? g_bytes_get_data(bytes, &count) : NULL;
    if (count) {
        g_string_append_len(job->output, data, (gssize)count);
        if (job->output->len > MaxOutputBytes) {
            g_string_erase(job->output, 0, (gssize)(job->output->len - MaxOutputBytes));
            job->truncated = TRUE;
        }
        if (job->on_output) {
            char *valid = g_utf8_make_valid(data, (gssize)count);
            job->on_output(valid, job->context);
            g_free(valid);
        }
        g_input_stream_read_bytes_async(G_INPUT_STREAM(source), 8192, G_PRIORITY_DEFAULT, job->reader, job_read, job);
    } else {
        job->eof = TRUE;
        if (error && !job->error) job->error = g_strdup(error->message);
    }
    g_clear_pointer(&bytes, g_bytes_unref);
    g_clear_error(&error);
    job_finish(job);
}
static void job_wait(GObject *source, GAsyncResult *result, gpointer context) {
    gui_job_t *job = context;
    GError *error = NULL;
    if (!g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error)) {
        job->status = 125;
        if (!job->error) job->error = g_strdup(error->message);
    } else if (g_subprocess_get_if_exited(job->process)) {
        job->status = g_subprocess_get_exit_status(job->process);
    } else job->status = 128 + g_subprocess_get_term_sig(job->process);
    job->waited = TRUE;
    g_clear_error(&error);
    job_finish(job);
}
gui_job_t *gui_job_start(const char *const *argv, gui_output_fn_t output,
                         gui_complete_fn_t complete, gpointer context, GError **error) {
    GSubprocess *process = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, error);
    if (!process) return NULL;
    gui_job_t *job = g_new0(gui_job_t, 1);
    job->process = process;
    job->reader = g_cancellable_new();
    job->output = g_string_new(NULL);
    job->on_output = output;
    job->on_complete = complete;
    job->context = context;
    g_input_stream_read_bytes_async(g_subprocess_get_stdout_pipe(process), 8192,
        G_PRIORITY_DEFAULT, job->reader, job_read, job);
    g_subprocess_wait_async(process, NULL, job_wait, job);
    return job;
}
static gboolean job_escalate(gpointer context) {
    gui_job_t *job = context;
    if (!job->waited) {
        if (job->escalation++ == 0) { g_subprocess_send_signal(job->process, SIGTERM); return G_SOURCE_CONTINUE; }
        g_subprocess_force_exit(job->process);
    }
    /* A descendant may still own the output pipe after its direct parent exits. */
    if (!job->eof) g_cancellable_cancel(job->reader);
    job->stop_timer = 0;
    return G_SOURCE_REMOVE;
}
void gui_job_stop(gui_job_t *job) {
    if (!job || job->stop_timer) return;
    if (!job->waited) g_subprocess_send_signal(job->process, SIGINT);
    job->stop_timer = g_timeout_add_seconds(3, job_escalate, job);
}
const char *gui_json_text(JsonObject *object, const char *key, const char *fallback) {
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : fallback;
}
JsonParser *gui_result_parse(const char *text, GError **error) {
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, error)) { g_object_unref(parser); return NULL; }
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto invalid;
    JsonObject *object = json_node_get_object(root);
    JsonNode *version = json_object_get_member(object, "schema_version");
    JsonNode *results = json_object_get_member(object, "results");
    JsonNode *ok = json_object_get_member(object, "ok");
    if (!version || json_node_get_value_type(version) != G_TYPE_INT64 || json_node_get_int(version) != 1 ||
        !ok || json_node_get_value_type(ok) != G_TYPE_BOOLEAN || !json_node_get_boolean(ok) ||
        !results || !JSON_NODE_HOLDS_ARRAY(results)) goto invalid;
    JsonArray *array = json_node_get_array(results);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonNode *item = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_OBJECT(item)) goto invalid;
        JsonObject *record = json_node_get_object(item);
        JsonNode *data = json_object_get_member(record, "data");
        JsonNode *success = json_object_get_member(record, "ok");
        const char *name = gui_json_text(record, "name", NULL);
        if (!name || !gui_name_valid(name) || !data || !JSON_NODE_HOLDS_OBJECT(data) ||
            !success || json_node_get_value_type(success) != G_TYPE_BOOLEAN || !json_node_get_boolean(success)) goto invalid;
    }
    return parser;
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid or unsuccessful Waddle management response (expected schema version 1).");
    g_object_unref(parser);
    return NULL;
}
gboolean gui_name_valid(const char *name) {
    if (!name || !*name || strlen(name) > 63) return FALSE;
    for (const char *p = name; *p; ++p)
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-') return FALSE;
    return TRUE;
}
char *gui_sibling_path(const char *name) {
    GError *error = NULL;
    char *executable = g_file_read_link("/proc/self/exe", &error);
    g_clear_error(&error);
    if (!executable) return NULL;
    char *directory = g_path_get_dirname(executable);
    char *path = g_build_filename(directory, name, NULL);
    g_free(directory);
    g_free(executable);
    return path;
}
