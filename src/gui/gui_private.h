/** @file gui_private.h @brief Private main-context GUI ownership and cross-module API. */
#ifndef WaddleGuiPrivateH
#define WaddleGuiPrivateH
#include <adwaita.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <vte/vte.h>
#pragma GCC diagnostic pop
#include "gui_backend.h"
#include "gui_store.h"
/** @brief Maximum mutable settings exposed by the working CLI. */
#define GuiSettingCount 10
/** @brief Application state; stack-owned through g_application_run and all held jobs.
 * GTK children are parent-owned. Strings, parsers, store and session array are
 * explicitly freed after the application drains its holds. Main-context only. */
typedef struct gui_t {
    AdwApplication *application;
    GtkWindow *window;
    AdwToastOverlay *toasts;
    GtkStack *pages, *details;
    GtkListBox *devices, *launchers, *history, *folders;
    GtkSearchEntry *device_search;
    GtkDropDown *filter, *sort;
    AdwWindowTitle *title;
    GtkLabel *summary, *notice, *operation_label, *config_notice;
    GtkSpinner *spinner;
    GtkTextView *output, *logs;
    GtkEntry *log_search, *log_lines;
    GtkDropDown *log_target;
    GtkCheckButton *log_follow;
    GtkWidget *device_content, *empty, *config_apply;
    AdwEntryRow *settings[GuiSettingCount];
    char *originals[GuiSettingCount];
    char *selected, *requested, *cli, *operation, *operation_device;
    char **last_argv;
    char *pending_av_device, *config_device, *initial_page;
    gui_store_t *store;
    JsonParser *registry;
    gui_job_t *job, *list_job, *detail_job;
    GPtrArray *terminals;
    guint refresh_timer;
    gboolean closing, rebuilding, loading_config, stale, stream_logs, device_requested;
} gui_t;
/** @brief Keys and labels, immutable borrowed arrays of GuiSettingCount entries. */
extern const char *const gui_setting_keys[GuiSettingCount];
extern const char *const gui_setting_labels[GuiSettingCount];
/** @brief Build/present window once; borrowed gui valid through application lifetime. */
void gui_window_present(gui_t *gui);
/** @brief Dispatch registered native action; borrowed strings, no ownership transfer. */
void gui_dispatch(gui_t *gui, const char *action);
/** @brief Refresh observed devices asynchronously without booting; main-context only. */
void gui_refresh(gui_t *gui);
/** @brief Select borrowed profile name; copied, does not start a device. */
void gui_select(gui_t *gui, const char *name);
/** @brief Return borrowed current registry data or NULL; valid until refresh, main-context only. */
JsonObject *gui_device_data(gui_t *gui, const char *name);
/** @brief Native notification/toast; borrowed text copied, NULL-safe on close. */
void gui_message(gui_t *gui, const char *message);
/** @brief Start one serialized foreground CLI operation; copies argv/labels.
 * @param[in] gui Borrowed application state, held until completion.
 * @param[in] label Nonnull operation name. @param[in] device Optional explicit profile.
 * @param[in] arguments NULL-terminated CLI argv after executable; copied.
 * @return TRUE on start, FALSE on busy/spawn error; main-context only.
 */
gboolean gui_run(gui_t *gui, const char *label, const char *device, const char *const *arguments);
/** @brief Build heap argv; add copies text, free with g_ptr_array_unref. Main-context only. */
GPtrArray *gui_args_new(const char *first, const char *second);
/** @brief Add copied nonnull argument to owned array; no caller string retained. */
void gui_args_add(GPtrArray *arguments, const char *text);
/** @brief Submit owned arguments and release array, borrowed labels; returns run status. */
gboolean gui_run_args(gui_t *gui, const char *label, const char *device, GPtrArray *arguments);
/** @brief Parent-owned standard widgets; borrowed labels copied, main-context only. */
GtkWidget *gui_button(const char *label, const char *icon, const char *action, gboolean primary);
/** @brief Append accessible selectable wrapped label; box borrowed, returns parent-owned label. */
GtkWidget *gui_label(GtkWidget *box, const char *text, const char *css);
/** @brief Create titled scrollable page, returns parent-owned interior box via out. */
GtkWidget *gui_page(GtkWidget **box, const char *title, const char *description);
/** @brief Query settings and folder information for selected device; main-context only. */
void gui_load_details(gui_t *gui);
/** @brief Present requested operation form; borrowed gui/action, captures device name. */
void gui_form_open(gui_t *gui, const char *action);
/** @brief Present native confirmation for exact copied argv; no shell, main-context only. */
void gui_confirm(gui_t *gui, const char *title, const char *description,
                 const char *label, const char *device, const char *const *arguments);
/** @brief Open searchable action palette scoped to a captured device; main-context only. */
void gui_palette_open(gui_t *gui);
/** @brief Add embedded terminal session for selected device; main-context only. */
void gui_terminal_open(gui_t *gui);
/** @brief Launch existing repl in host terminal; strings borrowed, no shell. */
void gui_terminal_external(gui_t *gui, const char *device, const char *const *arguments);
/** @brief Detach/reap all owned PTY children on GUI close; leaves VMs intact. */
void gui_terminals_close(gui_t *gui);
/** @brief Free reaped session storage after application loop; main-context only. */
void gui_terminal_cleanup(gui_t *gui);
/** @brief Apply current font/scrollback to retained sessions; main-context only. */
void gui_terminal_preferences(gui_t *gui);
/** @brief Spawn explicit borrowed CLI argv in a PTY, copied by VTE; main-context only. */
void gui_terminal_command(gui_t *gui, const char *device, const char *const *arguments);
/** @brief Build parent-owned terminal page; session storage belongs to gui. */
GtkWidget *gui_terminal_page(gui_t *gui);
/** @brief Build parent-owned launcher library and preferences pages. Main-context only. */
GtkWidget *gui_library_page(gui_t *gui);
GtkWidget *gui_preferences_page(gui_t *gui);
/** @brief Rebuild launcher rows after store edits; main-context only. */
void gui_library_refresh(gui_t *gui);
/** @brief Open saved launcher editor; nullable borrowed group, copied by editor. */
void gui_launcher_edit(gui_t *gui, const char *group);
/** @brief Apply appearance/terminal preferences and report failed atomic saves. */
void gui_preferences_apply(gui_t *gui);
void gui_store_flush(gui_t *gui);
/** @brief Copy/save selected activity/log output with native clipboard/file picker. */
void gui_output_export(gui_t *gui, GtkTextView *view, gboolean save);
#endif
