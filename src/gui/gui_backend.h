/** @file gui_backend.h @brief Asynchronous argv adapter to the unchanged CLI. */
#ifndef WaddleGuiBackendH
#define WaddleGuiBackendH
#include <gio/gio.h>
#include <json-glib/json-glib.h>

/** @brief Opaque main-context-owned job; borrowed until its completion callback. */
typedef struct gui_job_t gui_job_t;
/** @brief Borrowed UTF-8 output, valid during callback; main context only. */
typedef void (*gui_output_fn_t)(const char *text, gpointer context);
/** @brief Completion callback; output/error borrowed for call, status is exit or 128+signal.
 * Job is destroyed after return; clear any borrowed job pointer here. */
typedef void (*gui_complete_fn_t)(int status, const char *output, const char *error, gpointer context);
/** @brief Spawn argv directly, merge/drain output with a 2 MiB retained tail.
 * @param[in] argv Nonnull NULL-terminated borrowed arguments, copied by GSubprocess.
 * @param[in] output Optional streamed-output callback, on main context.
 * @param[in] complete Nonnull once-only completion callback; not called on spawn failure.
 * @param[in] context Borrowed callback state; caller keeps it alive until completion.
 * @param[out] error Optional GError destination, caller frees.
 * @return Borrowed job or NULL on spawn failure. Main-context only.
 */
gui_job_t *gui_job_start(const char *const *argv, gui_output_fn_t output,
                         gui_complete_fn_t complete, gpointer context, GError **error);
/** @brief Signal an owned CLI job; SIGINT then bounded TERM/KILL escalation.
 * @param[in] job Nullable borrowed job; remains alive until completion. Main-context only.
 */
void gui_job_stop(gui_job_t *job);
/** @brief Parse and validate schema v1 management results using system JSON parser.
 * @param[in] text Nonnull UTF-8 CLI JSON, borrowed during call.
 * @param[out] error Optional owned error on invalid schema.
 * @return Owned JsonParser or NULL; result objects live as long as parser. Thread-safe.
 */
JsonParser *gui_result_parse(const char *text, GError **error);
/** @brief Return borrowed typed string or fallback; NULL-safe, no allocation, thread-safe. */
const char *gui_json_text(JsonObject *object, const char *key, const char *fallback);
/** @brief Validate a profile name (ASCII letters/digits/_/-, 1..63); pure, thread-safe. */
gboolean gui_name_valid(const char *name);
/** @brief Resolve adjacent executable from /proc/self/exe; caller frees, NULL on failure. */
char *gui_sibling_path(const char *name);
#endif
