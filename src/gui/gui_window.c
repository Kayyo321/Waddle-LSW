/** @file gui_window.c @brief Device dashboard, asynchronous state and native navigation. */
#include "gui_private.h"
#include <string.h>
#include <time.h>

const char *const gui_setting_keys[GuiSettingCount] = {
    "memory_mb", "vcpus", "default_shell", "vsock_port", "start_timeout", "stop_timeout",
    "enabled", "uefi", "shm_path", "gpu_bdf"
};
const char *const gui_setting_labels[GuiSettingCount] = {
    "Memory (MiB)", "Virtual CPUs", "Default shell", "VSOCK port", "Start timeout (seconds)",
    "Stop timeout (seconds)", "AV enabled (0 or 1)", "UEFI (0 or 1)", "Shared memory path", "GPU parent PCI address"
};
static const char *const setting_defaults[GuiSettingCount] = {
    "4096", "4", "powershell.exe", "5242", "60", "15", "0", "0", "/dev/kvmfr0", ""
};

GPtrArray *gui_args_new(const char *first, const char *second) {
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
    if (first) gui_args_add(arguments, first);
    if (second) gui_args_add(arguments, second);
    return arguments;
}
void gui_args_add(GPtrArray *arguments, const char *text) { g_ptr_array_add(arguments, g_strdup(text)); }
gboolean gui_run_args(gui_t *gui, const char *label, const char *device, GPtrArray *arguments) {
    g_ptr_array_add(arguments, NULL);
    gboolean result = gui_run(gui, label, device, (const char *const *)arguments->pdata);
    g_ptr_array_unref(arguments);
    return result;
}
GtkWidget *gui_button(const char *label, const char *icon, const char *action, gboolean primary) {
    GtkWidget *button = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (icon) gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
    if (label && *label) gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_widget_set_tooltip_text(button, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    if (action) {
        gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "app.action");
        gtk_actionable_set_action_target_value(GTK_ACTIONABLE(button), g_variant_new_string(action));
    }
    if (primary) gtk_widget_add_css_class(button, "suggested-action");
    return button;
}
GtkWidget *gui_label(GtkWidget *box, const char *text, const char *css) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    if (css) gtk_widget_add_css_class(label, css);
    gtk_box_append(GTK_BOX(box), label);
    return label;
}
GtkWidget *gui_page(GtkWidget **box, const char *title, const char *description) {
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 1000);
    *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(*box, 24); gtk_widget_set_margin_end(*box, 24);
    gtk_widget_set_margin_top(*box, 24); gtk_widget_set_margin_bottom(*box, 24);
    gui_label(*box, title, "title-1");
    if (description) gui_label(*box, description, "dim-label");
    adw_clamp_set_child(ADW_CLAMP(clamp), *box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    return scroll;
}
void gui_message(gui_t *gui, const char *message) {
    if (!gui->closing && gui->toasts) {
        char *safe = g_markup_escape_text(message ? message : "Unknown error", -1);
        adw_toast_overlay_add_toast(gui->toasts, adw_toast_new(safe)); g_free(safe);
    }
}
static void text_append(GtkTextView *view, const char *text) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text, -1);
    int count = gtk_text_buffer_get_char_count(buffer);
    if (count > 200000) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(buffer, &start);
        gtk_text_buffer_get_iter_at_offset(buffer, &cut, count - 200000);
        gtk_text_buffer_delete(buffer, &start, &cut);
    }
}
static void stream_output(const char *text, gpointer context) {
    gui_t *gui = context;
    if (gui->closing) return;
    text_append(gui->output, text);
    if (gui->stream_logs && g_strcmp0(gui->selected, gui->operation_device) == 0) text_append(gui->logs, text);
}
static void history_add(gui_t *gui, const char *record) {
    if (gui->closing) return;
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), record);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    gtk_list_box_prepend(gui->history, GTK_WIDGET(row));
    int retention = gui_store_number(gui->store, "activity_retention", 50, 5, 200);
    GtkWidget *child;
    while ((child = GTK_WIDGET(gtk_list_box_get_row_at_index(gui->history, retention)))) gtk_list_box_remove(gui->history, child);
    GPtrArray *records = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; (child = GTK_WIDGET(gtk_list_box_get_row_at_index(gui->history, i))); ++i)
        g_ptr_array_add(records, g_strdup(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(child))));
    g_key_file_set_string_list(gui->store->keys, "Activity", "records", (const char *const *)records->pdata, records->len);
    g_ptr_array_unref(records);
    gui_store_flush(gui);
}
static void operation_done(int status, const char *output, const char *error, gpointer context) {
    gui_t *gui = context;
    gui->job = NULL;
    if (!gui->closing) {
        if (status == 0 && gui->stream_logs && g_strcmp0(gui->selected, gui->operation_device) == 0) {
            gboolean json = FALSE;
            for (guint i = 0; gui->last_argv && gui->last_argv[i]; ++i) json |= !strcmp(gui->last_argv[i], "--json");
            if (json) {
                JsonParser *parser = gui_result_parse(output, NULL);
                if (parser) {
                    JsonArray *results = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "results");
                    if (json_array_get_length(results)) {
                        JsonObject *data = json_object_get_object_member(json_array_get_object_element(results, 0), "data");
                        gtk_text_buffer_set_text(gtk_text_view_get_buffer(gui->logs), gui_json_text(data, "text", ""), -1);
                    }
                    g_object_unref(parser);
                }
            }
        }
        char *result = g_strdup_printf("%s · %s · %s (exit %d)", gui->operation,
            gui->operation_device ? gui->operation_device : "explicit command", status == 0 ? "Completed" : "Failed / interrupted", status);
        gtk_spinner_stop(gui->spinner);
        gtk_label_set_text(gui->operation_label, result);
        text_append(gui->output, "\n"); text_append(gui->output, result);
        if (error) { text_append(gui->output, "\n"); text_append(gui->output, error); }
        GDateTime *now = g_date_time_new_now_local();
        char *stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
        char *record = g_strdup_printf("%s  %s", stamp, result);
        history_add(gui, record);
        gui_message(gui, result);
        if (gui_store_number(gui->store, "notifications", 0, 0, 1)) {
            GNotification *notification = g_notification_new("Waddle operation finished");
            g_notification_set_body(notification, result);
            g_application_send_notification(G_APPLICATION(gui->application), "operation", notification);
            g_object_unref(notification);
        }
        g_date_time_unref(now); g_free(stamp); g_free(record); g_free(result);
        gui_refresh(gui);
        if (status == 0 && (strstr(gui->operation, "configuration") || strstr(gui->operation, "setting")))
            g_clear_pointer(&gui->config_device, g_free);
        gui_load_details(gui);
        char *av_device = g_steal_pointer(&gui->pending_av_device);
        if (av_device && status == 0) {
            JsonParser *parser = json_parser_new();
            gboolean valid = json_parser_load_from_data(parser, output, -1, NULL);
            JsonNode *root = valid ? json_parser_get_root(parser) : NULL;
            JsonNode *pid = root && JSON_NODE_HOLDS_OBJECT(root) ? json_object_get_member(json_node_get_object(root), "pid") : NULL;
            if (pid && json_node_get_value_type(pid) == G_TYPE_INT64 && json_node_get_int(pid) > 0 && json_node_get_int(pid) <= G_MAXUINT32) {
                char *number = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(pid));
                const char *args[] = {"av", "run", number, "--device", av_device, NULL};
                gui_run(gui, "Seamless application view", av_device, args); g_free(number);
            } else gui_message(gui, "Application launch returned no valid PID. Inspect output before attaching an AV view manually.");
            g_object_unref(parser);
        }
        g_free(av_device);
        char *start_device = gui_store_text(gui->store, "Session", "start_after_create", NULL);
        g_key_file_remove_key(gui->store->keys, "Session", "start_after_create", NULL);
        if (start_device && status == 0 && !gui->job) {
            const char *args[] = {"start", start_device, "--json", NULL}; gui_run(gui, "Start created device", start_device, args);
        }
        g_free(start_device);
        if (!gui->job) gui_dispatch(gui, "batch-next");
    }
    if (!gui->job) gui->stream_logs = FALSE;
    g_application_release(G_APPLICATION(gui->application));
}
gboolean gui_run(gui_t *gui, const char *label, const char *device, const char *const *arguments) {
    if (gui->closing) return FALSE;
    if (gui->job) { gui_message(gui, "An operation is active. Open Activity to view or stop it before starting another."); return FALSE; }
    GPtrArray *argv = gui_args_new(gui->cli, NULL);
    for (guint i = 0; arguments[i]; ++i) gui_args_add(argv, arguments[i]);
    g_ptr_array_add(argv, NULL);
    GError *error = NULL;
    gui->job = gui_job_start((const char *const *)argv->pdata, stream_output, operation_done, gui, &error);
    if (!gui->job) { gui_message(gui, error->message); g_clear_error(&error); g_ptr_array_unref(argv); return FALSE; }
    g_strfreev(gui->last_argv);
    gui->last_argv = g_strdupv((char **)arguments);
    g_free(gui->operation); gui->operation = g_strdup(label);
    g_free(gui->operation_device); gui->operation_device = g_strdup(device);
    gui->stream_logs = !strcmp(arguments[0], "logs");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(gui->output), "", -1);
    if (gui->stream_logs) gtk_text_buffer_set_text(gtk_text_view_get_buffer(gui->logs), "", -1);
    char *heading = g_strdup_printf("%s · %s — in progress", label, device ? device : "explicit command");
    gtk_label_set_text(gui->operation_label, heading);
    gtk_spinner_start(gui->spinner);
    g_free(heading);
    g_application_hold(G_APPLICATION(gui->application));
    if (!gui->stream_logs) gtk_stack_set_visible_child_name(gui->pages, "activity");
    g_ptr_array_unref(argv);
    return TRUE;
}
JsonObject *gui_device_data(gui_t *gui, const char *name) {
    if (!gui->registry || !name) return NULL;
    JsonObject *root = json_node_get_object(json_parser_get_root(gui->registry));
    JsonArray *results = json_object_get_array_member(root, "results");
    for (guint i = 0; i < json_array_get_length(results); ++i) {
        JsonObject *item = json_array_get_object_element(results, i);
        if (!strcmp(gui_json_text(item, "name", ""), name)) return json_object_get_object_member(item, "data");
    }
    return NULL;
}
static char *scalar_text(JsonObject *object, const char *key) {
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    if (!node || JSON_NODE_HOLDS_NULL(node)) return g_strdup("");
    if (json_node_get_value_type(node) == G_TYPE_STRING) return g_strdup(json_node_get_string(node));
    if (json_node_get_value_type(node) == G_TYPE_INT64) return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
    if (json_node_get_value_type(node) == G_TYPE_BOOLEAN) return g_strdup(json_node_get_boolean(node) ? "1" : "0");
    return g_strdup("");
}
static gboolean json_flag(JsonObject *object, const char *key) {
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static void selection_summary(gui_t *gui) {
    JsonObject *data = gui_device_data(gui, gui->selected);
    gtk_widget_set_visible(gui->device_content, data != NULL);
    gtk_widget_set_visible(gui->empty, data == NULL);
    if (!data) {
        adw_window_title_set_title(gui->title, "Waddle"); adw_window_title_set_subtitle(gui->title, "Windows devices");
        gtk_window_set_title(gui->window, "Waddle"); return;
    }
    const char *state = gui_json_text(data, "state", "unknown");
    char *memory = scalar_text(data, "memory_mb"), *cpus = scalar_text(data, "vcpus");
    char *summary = g_strdup_printf("%s%s\n%s MiB memory · %s virtual CPUs\n%s",
        state, gui->stale ? " · observation stale" : "", memory, cpus,
        !json_flag(data, "config_valid") ? "Configuration needs attention. Run a doctor check before starting." :
        !strcmp(state, "running") ? "The supervisor reports guest readiness. Closing Waddle leaves Windows running." :
        !strcmp(state, "starting") ? "Processes are starting; guest readiness has not been reported yet." :
        !strcmp(state, "stopped") ? "Device is stopped. Start it or open a terminal when you are ready." :
        "Readiness is unknown. Refresh or inspect diagnostics before acting.");
    gtk_label_set_text(gui->summary, summary);
    char *window_title = g_strdup_printf("%s — Waddle", gui->selected);
    gtk_window_set_title(gui->window, window_title); g_free(window_title);
    adw_window_title_set_title(gui->title, gui->selected);
    adw_window_title_set_subtitle(gui->title, state);
    g_free(summary); g_free(memory); g_free(cpus);
}
void gui_select(gui_t *gui, const char *name) {
    if (gui->closing) return;
    if (!gui_device_data(gui, name)) {
        char *message = g_strdup_printf("Device '%s' is not in the registry. Refresh or create/import it.", name ? name : "");
        gui_message(gui, message); g_free(message);
        return;
    }
    gboolean changed = g_strcmp0(gui->selected, name) != 0;
    if (changed && gui->stream_logs) gui_job_stop(gui->job);
    char *copy = g_strdup(name);
    g_free(gui->selected); gui->selected = copy;
    selection_summary(gui);
    for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(gui->devices)); row; row = gtk_widget_get_next_sibling(row))
        if (g_strcmp0(g_object_get_data(G_OBJECT(row), "device"), name) == 0) gtk_list_box_select_row(gui->devices, GTK_LIST_BOX_ROW(row));
    gtk_stack_set_visible_child_name(gui->pages, "devices");
    if (changed) {
        gui->loading_config = TRUE;
        for (guint i = 0; i < GuiSettingCount; ++i) {
            g_clear_pointer(&gui->originals[i], g_free);
            gtk_editable_set_text(GTK_EDITABLE(gui->settings[i]), "");
        }
        g_clear_pointer(&gui->config_device, g_free);
        gui->loading_config = FALSE;
        g_key_file_set_int64(gui->store->keys, "RecentDevices", name, (gint64)time(NULL));
        g_key_file_set_string(gui->store->keys, "Preferences", "last_device", name);
        gui_store_flush(gui);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(gui->logs), "", -1);
        gui_load_details(gui);
    }
}
static gboolean device_filter(GtkListBoxRow *row, gpointer context) {
    gui_t *gui = context;
    const char *name = g_object_get_data(G_OBJECT(row), "device");
    JsonObject *data = gui_device_data(gui, name);
    if (!data) return FALSE;
    char *lower = g_ascii_strdown(name, -1);
    char *query = g_ascii_strdown(gtk_editable_get_text(GTK_EDITABLE(gui->device_search)), -1);
    gboolean match = strstr(lower, query) != NULL;
    g_free(lower); g_free(query);
    guint filter = gtk_drop_down_get_selected(gui->filter);
    const char *state = gui_json_text(data, "state", "unknown");
    if (filter == 1) match &= !strcmp(state, "running");
    if (filter == 2) match &= !strcmp(state, "stopped");
    if (filter == 3) match &= !json_flag(data, "config_valid") || !strcmp(state, "unknown") || !strcmp(state, "failed");
    return match;
}
static int device_sort(GtkListBoxRow *left, GtkListBoxRow *right, gpointer context) {
    gui_t *gui = context;
    const char *a = g_object_get_data(G_OBJECT(left), "device"), *b = g_object_get_data(G_OBJECT(right), "device");
    gboolean pin_a = g_key_file_get_boolean(gui->store->keys, "PinnedDevices", a, NULL);
    gboolean pin_b = g_key_file_get_boolean(gui->store->keys, "PinnedDevices", b, NULL);
    if (pin_a != pin_b) return pin_a ? -1 : 1;
    guint sort = gtk_drop_down_get_selected(gui->sort);
    if (sort == 1) {
        int order = g_strcmp0(gui_json_text(gui_device_data(gui, a), "state", "unknown"), gui_json_text(gui_device_data(gui, b), "state", "unknown"));
        if (order) return order;
    } else if (sort == 2) {
        gint64 recent_a = g_key_file_get_int64(gui->store->keys, "RecentDevices", a, NULL);
        gint64 recent_b = g_key_file_get_int64(gui->store->keys, "RecentDevices", b, NULL);
        if (recent_a != recent_b) return recent_a > recent_b ? -1 : 1;
    }
    return g_strcmp0(a, b);
}
static void device_controls_changed(GObject *object, GParamSpec *property, gpointer context) {
    (void)object; (void)property;
    gui_t *gui = context;
    gtk_list_box_invalidate_filter(gui->devices); gtk_list_box_invalidate_sort(gui->devices);
}
static void device_search_changed(GtkSearchEntry *entry, gpointer context) {
    (void)entry; device_controls_changed(NULL, NULL, context);
}
static void row_open(GtkListBox *box, GtkListBoxRow *row, gpointer context) {
    (void)box;
    if (row) gui_select(context, g_object_get_data(G_OBJECT(row), "device"));
}
static void list_done(int status, const char *output, const char *error, gpointer context) {
    gui_t *gui = context;
    gui->list_job = NULL;
    if (!gui->closing) {
        GError *issue = NULL;
        JsonParser *parser = status == 0 && !error ? gui_result_parse(output, &issue) : NULL;
        if (parser) {
            g_clear_object(&gui->registry); gui->registry = parser;
            gui->stale = FALSE;
            GtkWidget *child;
            while ((child = gtk_widget_get_first_child(GTK_WIDGET(gui->devices)))) gtk_list_box_remove(gui->devices, child);
            JsonArray *results = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "results");
            const char *first = NULL, *default_name = NULL;
            for (guint i = 0; i < json_array_get_length(results); ++i) {
                JsonObject *record = json_array_get_object_element(results, i);
                const char *name = gui_json_text(record, "name", "");
                JsonObject *data = json_object_get_object_member(record, "data");
                if (!first) first = name;
                if (json_flag(data, "is_default")) default_name = name;
                AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
                adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
                char *title = g_strdup_printf("%s%s%s", name, json_flag(data, "is_default") ? " · Default" : "",
                    g_key_file_get_boolean(gui->store->keys, "PinnedDevices", name, NULL) ? " · Pinned" : "");
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title); g_free(title);
                const char *state = gui_json_text(data, "state", "unknown");
                char *memory = scalar_text(data, "memory_mb"), *cpus = scalar_text(data, "vcpus");
                char *subtitle = g_strdup_printf("%s · %s MiB · %s CPUs%s", state, memory, cpus,
                    json_flag(data, "config_valid") ? "" : " · Invalid configuration");
                adw_action_row_set_subtitle(row, subtitle);
                g_free(subtitle); g_free(memory); g_free(cpus);
                adw_action_row_add_prefix(row, gtk_image_new_from_icon_name(!strcmp(state, "running") ? "media-playback-start-symbolic" : "computer-symbolic"));
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
                g_object_set_data_full(G_OBJECT(row), "device", g_strdup(name), g_free);
                gtk_list_box_append(gui->devices, GTK_WIDGET(row));
            }
            if (gui->requested) {
                char *requested = g_steal_pointer(&gui->requested);
                gui_select(gui, requested); g_free(requested);
            } else if (!gui_device_data(gui, gui->selected)) {
                g_clear_pointer(&gui->selected, g_free);
                if (default_name || first) gui_select(gui, default_name ? default_name : first);
            }
            if (gui->initial_page) {
                if (gtk_stack_get_child_by_name(gui->pages, gui->initial_page)) gtk_stack_set_visible_child_name(gui->pages, gui->initial_page);
                g_clear_pointer(&gui->initial_page, g_free);
            }
            gtk_label_set_text(gui->notice, json_array_get_length(results) ? "Select a device to inspect it. Opening never starts Windows." : "No devices yet. Create one from a Windows image or import a backup.");
        } else {
            gui->stale = TRUE;
            gtk_label_set_text(gui->notice, "Device refresh failed. Previous observations are stale; open Diagnostics for recovery checks.");
            gui_message(gui, issue ? issue->message : (error ? error : output));
        }
        selection_summary(gui);
        g_clear_error(&issue);
    }
    g_application_release(G_APPLICATION(gui->application));
}
void gui_refresh(gui_t *gui) {
    if (gui->closing || gui->list_job) return;
    const char *argv[] = {gui->cli, "device", "list", "--json", NULL};
    GError *error = NULL;
    gui->list_job = gui_job_start(argv, NULL, list_done, gui, &error);
    if (gui->list_job) g_application_hold(G_APPLICATION(gui->application));
    else { gui_message(gui, error->message); g_clear_error(&error); }
}
/** @brief One settings/folders observation, owns target name until callback. */
typedef struct detail_query_t { gui_t *gui; char *name; gboolean folders; } detail_query_t;
static void folder_open(GtkButton *button, gpointer context) {
    gui_t *gui = context;
    const char *path = g_object_get_data(G_OBJECT(button), "path");
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) { gtk_show_uri(gui->window, uri, GDK_CURRENT_TIME); g_free(uri); }
    else gui_message(gui, "The host mapping is not an absolute local path.");
}
static void folder_copy(GtkButton *button, gpointer context) {
    gui_t *gui = context;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(gui->window)), g_object_get_data(G_OBJECT(button), "path"));
}
static void query_folders(gui_t *gui);
static void detail_done(int status, const char *output, const char *error, gpointer context) {
    detail_query_t *query = context;
    gui_t *gui = query->gui;
    gui->detail_job = NULL;
    gboolean current = !gui->closing && g_strcmp0(gui->selected, query->name) == 0;
    if (current) {
        GError *issue = NULL;
        JsonParser *parser = status == 0 && !error ? gui_result_parse(output, &issue) : NULL;
        if (parser) {
            JsonArray *results = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "results");
            JsonObject *data = json_array_get_length(results) ? json_object_get_object_member(json_array_get_object_element(results, 0), "data") : NULL;
            if (!query->folders) {
                gboolean dirty = FALSE;
                if (g_strcmp0(gui->config_device, query->name) == 0)
                    for (guint i = 0; i < GuiSettingCount; ++i)
                        dirty |= g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(gui->settings[i])), gui->originals[i]) != 0;
                if (!dirty) {
                g_free(gui->config_device); gui->config_device = g_strdup(query->name);
                gui->loading_config = TRUE;
                for (guint i = 0; i < GuiSettingCount; ++i) {
                    char *value = scalar_text(data, gui_setting_keys[i]);
                    g_free(gui->originals[i]); gui->originals[i] = value;
                    gtk_editable_set_text(GTK_EDITABLE(gui->settings[i]), value);
                }
                gui->loading_config = FALSE;
                gtk_label_set_text(gui->config_notice, "Changes require a stopped device and are validated/applied together. Blank advanced values are unchanged; use Reset to clear a value.");
                }
            } else {
                GtkWidget *child;
                while ((child = gtk_widget_get_first_child(GTK_WIDGET(gui->folders)))) gtk_list_box_remove(gui->folders, child);
                JsonNode *mount_node = data ? json_object_get_member(data, "mounts") : NULL;
                JsonArray *mounts = mount_node && JSON_NODE_HOLDS_ARRAY(mount_node) ? json_node_get_array(mount_node) : NULL;
                if (mounts) for (guint i = 0; i < json_array_get_length(mounts); ++i) {
                    JsonNode *item = json_array_get_element(mounts, i);
                    if (!JSON_NODE_HOLDS_OBJECT(item)) continue;
                    JsonObject *mount = json_node_get_object(item);
                    const char *host = gui_json_text(mount, "host_path", "Unavailable");
                    const char *guest = gui_json_text(mount, "guest_drive", "Unavailable");
                    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
                    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
                    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), host);
                    char *subtitle = g_strdup_printf("%s · %s", guest, json_flag(mount, "read_only") ? "Read only" : "Read and write");
                    adw_action_row_set_subtitle(row, subtitle); g_free(subtitle);
                    GtkWidget *open = gui_button("Open host folder", "folder-open-symbolic", NULL, FALSE);
                    g_object_set_data_full(G_OBJECT(open), "path", g_strdup(host), g_free);
                    g_signal_connect(open, "clicked", G_CALLBACK(folder_open), gui);
                    adw_action_row_add_suffix(row, open);
                    GtkWidget *copy = gui_button("Copy guest path", "edit-copy-symbolic", NULL, FALSE);
                    g_object_set_data_full(G_OBJECT(copy), "path", g_strdup(guest), g_free);
                    g_signal_connect(copy, "clicked", G_CALLBACK(folder_copy), gui);
                    adw_action_row_add_suffix(row, copy);
                    gtk_list_box_append(gui->folders, GTK_WIDGET(row));
                }
                if (!mounts || !json_array_get_length(mounts)) {
                    GtkWidget *label = gtk_label_new("No shared folders. Imported devices keep host sharing disabled.");
                    gtk_list_box_append(gui->folders, label);
                }
            }
            g_object_unref(parser);
        } else {
            if (!query->folders) gtk_label_set_text(gui->config_notice, "Could not load effective settings. Run doctor; blank fields do not represent backend defaults.");
            gui_message(gui, issue ? issue->message : (error ? error : output));
        }
        g_clear_error(&issue);
    }
    gboolean folders_next = current && !query->folders;
    gboolean reload = !gui->closing && !current;
    g_free(query->name); g_free(query);
    if (folders_next) query_folders(gui);
    else if (reload) gui_load_details(gui);
    g_application_release(G_APPLICATION(gui->application));
}
static void detail_start(gui_t *gui, gboolean folders) {
    if (gui->closing || !gui->selected || gui->detail_job) return;
    detail_query_t *query = g_new0(detail_query_t, 1);
    query->gui = gui; query->name = g_strdup(gui->selected); query->folders = folders;
    const char *config_argv[] = {gui->cli, "device", "config", "get", query->name, "--json", NULL};
    const char *folder_argv[] = {gui->cli, "fs", query->name, "--json", NULL};
    GError *error = NULL;
    gui->detail_job = gui_job_start(folders ? folder_argv : config_argv, NULL, detail_done, query, &error);
    if (gui->detail_job) g_application_hold(G_APPLICATION(gui->application));
    else { gui_message(gui, error->message); g_clear_error(&error); g_free(query->name); g_free(query); }
}
void gui_load_details(gui_t *gui) { detail_start(gui, FALSE); }
static void query_folders(gui_t *gui) { detail_start(gui, TRUE); }
static gboolean refresh_tick(gpointer context) { gui_refresh(context); return G_SOURCE_CONTINUE; }
static gboolean close_window(GtkWindow *window, gpointer context) {
    gui_t *gui = context;
    gui->closing = TRUE;
    if (gui->refresh_timer) { g_source_remove(gui->refresh_timer); gui->refresh_timer = 0; }
    g_key_file_set_integer(gui->store->keys, "Preferences", "width", gtk_widget_get_width(GTK_WIDGET(window)));
    g_key_file_set_integer(gui->store->keys, "Preferences", "height", gtk_widget_get_height(GTK_WIDGET(window)));
    const char *page = gtk_stack_get_visible_child_name(gui->pages);
    if (page) g_key_file_set_string(gui->store->keys, "Preferences", "last_page", page);
    gui_store_flush(gui);
    gui_job_stop(gui->job); gui_job_stop(gui->list_job); gui_job_stop(gui->detail_job);
    gui_terminals_close(gui);
    return FALSE;
}
static GtkWidget *button_row(GtkWidget *box) {
    GtkWidget *row = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(row), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(row), 5);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(row), 8); gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(row), 8);
    gtk_box_append(GTK_BOX(box), row);
    return row;
}
static void add_button(GtkWidget *row, const char *text, const char *icon, const char *action, gboolean primary) {
    gtk_flow_box_insert(GTK_FLOW_BOX(row), gui_button(text, icon, action, primary), -1);
}
static GtkTextView *output_view(GtkWidget *box, int height) {
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), height);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkTextView *view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(view, FALSE); gtk_text_view_set_monospace(view, TRUE);
    gtk_text_view_set_wrap_mode(view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(view, 12); gtk_text_view_set_right_margin(view, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(view));
    gtk_box_append(GTK_BOX(box), scroll);
    return view;
}
static void config_changed(GtkEditable *entry, gpointer context) {
    (void)entry;
    gui_t *gui = context;
    if (gui->loading_config) return;
    gboolean dirty = FALSE;
    for (guint i = 0; i < GuiSettingCount; ++i)
        dirty |= g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(gui->settings[i])), gui->originals[i]) != 0;
    gtk_label_set_text(gui->config_notice, dirty ? "Unsaved changes · Review before applying. Device must be stopped." : "No unsaved changes. Changes require a stopped device.");
}
static void config_reset_one(GtkButton *button, gpointer context) {
    gui_t *gui = context;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "index"));
    if (!gui->selected) return;
    const char *args[] = {"device", "config", "reset", gui->selected, gui_setting_keys[index], "--json", NULL};
    char *description = g_strdup_printf("Reset %s for %s to its backend default?", gui_setting_keys[index], gui->selected);
    gui_confirm(gui, "Reset device setting?", description, "Reset setting", gui->selected, args);
    g_free(description);
}
static GtkWidget *configuration_page(gui_t *gui) {
    GtkWidget *box;
    GtkWidget *page = gui_page(&box, "Configuration", "Effective values from Waddle. Edits are atomic and require a stopped device.");
    gui->config_notice = GTK_LABEL(gui_label(box, "Loading effective settings…", "dim-label"));
    AdwPreferencesGroup *basic = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(basic, "Resources and terminal"); gtk_box_append(GTK_BOX(box), GTK_WIDGET(basic));
    GtkWidget *expander = gtk_expander_new("Transport and graphics · Advanced");
    AdwPreferencesGroup *advanced = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    gtk_expander_set_child(GTK_EXPANDER(expander), GTK_WIDGET(advanced)); gtk_box_append(GTK_BOX(box), expander);
    for (guint i = 0; i < GuiSettingCount; ++i) {
        gui->settings[i] = ADW_ENTRY_ROW(adw_entry_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(gui->settings[i]), gui_setting_labels[i]);
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(gui->settings[i]), FALSE);
        GtkWidget *reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
        char *tip = g_strdup_printf("Reset %s to %s", gui_setting_keys[i], setting_defaults[i]);
        gtk_widget_set_tooltip_text(reset, tip); g_free(tip);
        g_object_set_data(G_OBJECT(reset), "index", GUINT_TO_POINTER(i));
        g_signal_connect(reset, "clicked", G_CALLBACK(config_reset_one), gui);
        adw_entry_row_add_suffix(gui->settings[i], reset);
        adw_preferences_group_add(i < 3 ? basic : advanced, GTK_WIDGET(gui->settings[i]));
        g_signal_connect(gui->settings[i], "changed", G_CALLBACK(config_changed), gui);
    }
    gui_label(box, "The current CLI omits effective AV/UEFI/path/GPU values from config get. Those fields stay blank until edited; reset uses the documented backend defaults.", "dim-label");
    GtkWidget *row = button_row(box);
    add_button(row, "Review changes", "emblem-ok-symbolic", "config-apply", TRUE);
    add_button(row, "Reload values", "view-refresh-symbolic", "config-reload", FALSE);
    add_button(row, "Inspect profile", "document-properties-symbolic", "inspect", FALSE);
    return page;
}
static void log_search_changed(GtkEditable *entry, gpointer context) {
    gui_t *gui = context;
    const char *text = gtk_editable_get_text(entry);
    if (!*text) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(gui->logs);
    GtkTextIter start, found, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    if (gtk_text_iter_forward_search(&start, text, GTK_TEXT_SEARCH_CASE_INSENSITIVE | GTK_TEXT_SEARCH_TEXT_ONLY, &found, &end, NULL)) {
        gtk_text_buffer_select_range(buffer, &found, &end);
        gtk_text_view_scroll_to_iter(gui->logs, &found, 0.1, FALSE, 0, 0);
    }
}
static GtkWidget *logs_page(gui_t *gui) {
    GtkWidget *box;
    GtkWidget *page = gui_page(&box, "Logs", "Logs remain scoped to the device captured when Read is pressed. Pause following stops only the log reader.");
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *targets[] = {"all", "daemon", "qemu", "virtiofsd", NULL};
    gui->log_target = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(targets));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->log_target), "Log source");
    gui->log_lines = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->log_lines, "Lines (1–10000)");
    gtk_editable_set_text(GTK_EDITABLE(gui->log_lines), "200");
    gui->log_follow = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow"));
    gtk_check_button_set_active(gui->log_follow, gui_store_number(gui->store, "log_follow", 0, 0, 1));
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(gui->log_target)); gtk_box_append(GTK_BOX(controls), GTK_WIDGET(gui->log_lines));
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(gui->log_follow));
    gtk_box_append(GTK_BOX(box), controls);
    GtkWidget *row = button_row(box);
    add_button(row, "Read logs", "view-list-symbolic", "logs-read", TRUE);
    add_button(row, "Pause following", "media-playback-pause-symbolic", "logs-stop", FALSE);
    add_button(row, "Copy", "edit-copy-symbolic", "logs-copy", FALSE);
    add_button(row, "Save…", "document-save-symbolic", "logs-save", FALSE);
    gui->log_search = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->log_search, "Search captured logs");
    g_signal_connect(gui->log_search, "changed", G_CALLBACK(log_search_changed), gui);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->log_search));
    gui->logs = output_view(box, 320);
    return page;
}
static GtkWidget *device_details(gui_t *gui) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *switch_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(switch_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gui->details = GTK_STACK(gtk_stack_new()); gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), gui->details);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(switch_scroll), switcher);
    gtk_box_append(GTK_BOX(outer), switch_scroll); gtk_box_append(GTK_BOX(outer), GTK_WIDGET(gui->details));
    gtk_widget_set_vexpand(GTK_WIDGET(gui->details), TRUE);
    GtkWidget *box;
    GtkWidget *page = gui_page(&box, "Device overview", "Start Windows only when you choose an operation that needs it.");
    gui->summary = GTK_LABEL(gui_label(box, "Select a device", NULL));
    GtkWidget *row = button_row(box);
    add_button(row, "Start", "media-playback-start-symbolic", "start", TRUE);
    add_button(row, "Shut down", "system-shutdown-symbolic", "stop", FALSE);
    add_button(row, "Open terminal", "utilities-terminal-symbolic", "terminal", FALSE);
    add_button(row, "View Full Desktop", "video-display-symbolic", "desktop", FALSE);
    row = button_row(box);
    add_button(row, "Restart subsystem", "view-refresh-symbolic", "restart", FALSE);
    add_button(row, "Reboot Windows", "system-reboot-symbolic", "reboot", FALSE);
    add_button(row, "Pause", "media-playback-pause-symbolic", "pause", FALSE);
    add_button(row, "Resume", "media-playback-start-symbolic", "resume", FALSE);
    row = button_row(box);
    add_button(row, "Set default", "emblem-ok-symbolic", "default", FALSE);
    add_button(row, "Clear default", "edit-clear-symbolic", "default-clear", FALSE);
    add_button(row, "Pin / unpin", "starred-symbolic", "pin", FALSE);
    add_button(row, "Inspect runtime", "dialog-information-symbolic", "status", FALSE);
    GtkWidget *expander = gtk_expander_new("Device management and backups");
    GtkWidget *management = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gui_label(management, "Rename, clone, export and removal require a stopped device. Host shared folders are never owned device data.", "dim-label");
    row = button_row(management);
    add_button(row, "Rename…", "document-edit-symbolic", "rename", FALSE);
    add_button(row, "Clone…", "edit-copy-symbolic", "clone", FALSE);
    add_button(row, "Export backup…", "document-save-symbolic", "export", FALSE);
    add_button(row, "Force stop…", "process-stop-symbolic", "kill", FALSE);
    add_button(row, "Remove device…", "user-trash-symbolic", "remove", FALSE);
    gtk_expander_set_child(GTK_EXPANDER(expander), management); gtk_box_append(GTK_BOX(box), expander);
    gtk_stack_add_titled(gui->details, page, "overview", "Overview");
    page = gui_page(&box, "Device applications", "Saved launchers keep an explicit device, arguments and execution options.");
    row = button_row(box);
    add_button(row, "Application library", "view-app-grid-symbolic", "applications", TRUE);
    add_button(row, "New launcher…", "list-add-symbolic", "launcher-new", FALSE);
    add_button(row, "Run command…", "system-run-symbolic", "exec", FALSE);
    gtk_stack_add_titled(gui->details, page, "applications", "Applications");
    gtk_stack_add_titled(gui->details, gui_terminal_page(gui), "terminal", "Terminal");
    page = gui_page(&box, "Shared folders", "Mappings come from the existing device configuration. Mapping edits have no supported CLI interface; imported devices keep sharing disabled.");
    gui->folders = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(gui->folders, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(gui->folders), "boxed-list"); gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->folders));
    row = button_row(box); add_button(row, "Run filesystem test", "emblem-ok-symbolic", "fs-test", TRUE);
    add_button(row, "Refresh mappings", "view-refresh-symbolic", "folders", FALSE);
    gtk_stack_add_titled(gui->details, page, "folders", "Shared Folders");
    page = gui_page(&box, "Graphics & Display", "Use the existing native seamless viewer for a guest process. Software contexts and quotas do not guarantee hardware partitions, dedicated VRAM or exclusive GPU ownership.");
    row = button_row(box);
    add_button(row, "Attach to process…", "video-display-symbolic", "av-run", TRUE);
    add_button(row, "Probe AV", "dialog-information-symbolic", "av-probe", FALSE);
    add_button(row, "Set up AV…", "preferences-system-symbolic", "av-setup", FALSE);
    add_button(row, "View Full Desktop", "video-display-symbolic", "desktop", FALSE);
    gui_label(box, "The native AV viewer owns its window, input, audio and presentation. Closing it disconnects the view and leaves Windows running. Probe and latency results appear in Activity.", "dim-label");
    GtkWidget *dependency = gtk_expander_new("Full desktop and pause/resume availability");
    GtkWidget *dependency_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gui_label(dependency_box, "Full desktop: the current managed AV API accepts a process ID only. It has no desktop capture target. Pause/resume: the daemon protocol has no execution pause/resume operation. These controls cannot be enabled until those backend interfaces exist.", NULL);
    gtk_expander_set_child(GTK_EXPANDER(dependency), dependency_box); gtk_box_append(GTK_BOX(box), dependency);
    gtk_stack_add_titled(gui->details, page, "graphics", "Graphics & Display");
    gtk_stack_add_titled(gui->details, logs_page(gui), "logs", "Logs");
    gtk_stack_add_titled(gui->details, configuration_page(gui), "configuration", "Configuration");
    return outer;
}
static GtkWidget *dashboard(gui_t *gui) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(outer, 12); gtk_widget_set_margin_end(outer, 12);
    gtk_widget_set_margin_top(outer, 12);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gui->device_search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(gui->device_search), TRUE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->device_search), "Search devices by name");
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(gui->device_search));
    const char *filters[] = {"All states", "Running", "Stopped", "Unhealthy", NULL};
    const char *sorts[] = {"Name", "State", "Recent use", NULL};
    gui->filter = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    gui->sort = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sorts));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->filter), "Filter by state"); gtk_widget_set_tooltip_text(GTK_WIDGET(gui->sort), "Sort devices");
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(gui->filter)); gtk_box_append(GTK_BOX(controls), GTK_WIDGET(gui->sort));
    gtk_box_append(GTK_BOX(outer), controls);
    GtkWidget *actions = button_row(outer);
    add_button(actions, "Create device…", "list-add-symbolic", "create", TRUE);
    add_button(actions, "Import backup…", "document-open-symbolic", "import", FALSE);
    add_button(actions, "Batch actions…", "view-list-symbolic", "batch", FALSE);
    add_button(actions, "Refresh", "view-refresh-symbolic", "refresh", FALSE);
    gui->notice = GTK_LABEL(gui_label(outer, "Loading device registry…", "dim-label"));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 100);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 210);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gui->devices = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(gui->devices), "boxed-list");
    gtk_list_box_set_selection_mode(gui->devices, GTK_SELECTION_SINGLE);
    gtk_list_box_set_filter_func(gui->devices, device_filter, gui, NULL);
    gtk_list_box_set_sort_func(gui->devices, device_sort, gui, NULL);
    g_signal_connect(gui->devices, "row-activated", G_CALLBACK(row_open), gui);
    g_signal_connect(gui->device_search, "search-changed", G_CALLBACK(device_search_changed), gui);
    g_signal_connect(gui->filter, "notify::selected", G_CALLBACK(device_controls_changed), gui);
    g_signal_connect(gui->sort, "notify::selected", G_CALLBACK(device_controls_changed), gui);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(gui->devices)); gtk_box_append(GTK_BOX(outer), scroll);
    gui->empty = adw_status_page_new();
    adw_status_page_set_title(ADW_STATUS_PAGE(gui->empty), "Make room for Windows");
    adw_status_page_set_description(ADW_STATUS_PAGE(gui->empty), "Create a device from a Windows disk or import a Waddle backup. Existing files stay under your control.");
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(gui->empty), "computer-symbolic");
    gtk_widget_set_vexpand(gui->empty, TRUE); gtk_box_append(GTK_BOX(outer), gui->empty);
    gui->device_content = device_details(gui); gtk_widget_set_visible(gui->device_content, FALSE);
    gtk_widget_set_vexpand(gui->device_content, TRUE); gtk_box_append(GTK_BOX(outer), gui->device_content);
    return outer;
}
static GtkWidget *activity_page(gui_t *gui) {
    GtkWidget *box;
    GtkWidget *page = gui_page(&box, "Activity", "Operations keep their originating device and exit status. Output is bounded; devices continue running when Waddle closes.");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gui->spinner = GTK_SPINNER(gtk_spinner_new()); gtk_box_append(GTK_BOX(row), GTK_WIDGET(gui->spinner));
    gui->operation_label = GTK_LABEL(gui_label(row, "No active operation", "heading")); gtk_box_append(GTK_BOX(box), row);
    row = button_row(box);
    add_button(row, "Stop operation", "process-stop-symbolic", "cancel", FALSE);
    add_button(row, "Retry explicitly", "view-refresh-symbolic", "retry", FALSE);
    add_button(row, "Copy output", "edit-copy-symbolic", "output-copy", FALSE);
    add_button(row, "Save output…", "document-save-symbolic", "output-save", FALSE);
    add_button(row, "Copy CLI command", "utilities-terminal-symbolic", "command-copy", FALSE);
    gui->output = output_view(box, 280);
    gui_label(box, "Recent results", "title-3");
    gui->history = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(gui->history, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(gui->history), "boxed-list"); gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->history));
    gsize count = 0;
    char **records = g_key_file_get_string_list(gui->store->keys, "Activity", "records", &count, NULL);
    for (gsize i = 0; records && i < MIN(count, 200); ++i) {
        AdwActionRow *item = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(item), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(item), records[i]);
        gtk_list_box_append(gui->history, GTK_WIDGET(item));
    }
    g_strfreev(records);
    return page;
}
static GtkWidget *diagnostics_page(void) {
    GtkWidget *box;
    GtkWidget *page = gui_page(&box, "Diagnostics", "Checks inspect the selected device without starting it. Review repair plans before applying changes.");
    GtkWidget *row = button_row(box);
    add_button(row, "Check selected device", "emblem-ok-symbolic", "doctor", TRUE);
    add_button(row, "Check all devices", "view-list-symbolic", "doctor-all", FALSE);
    add_button(row, "Preview repairs", "document-properties-symbolic", "repair-preview", FALSE);
    add_button(row, "Apply repairs…", "preferences-system-symbolic", "repair", FALSE);
    row = button_row(box);
    add_button(row, "Filesystem test", "folder-symbolic", "fs-test", FALSE);
    add_button(row, "Versions", "dialog-information-symbolic", "version", FALSE);
    add_button(row, "Device logs", "view-list-symbolic", "logs", FALSE);
    add_button(row, "Advanced CLI…", "utilities-terminal-symbolic", "advanced", FALSE);
    gui_label(box, "Support export: run the checks you want, review their output in Activity, then Save output. Only that visible output is exported; no files or secrets are collected automatically.", "dim-label");
    gui_label(box, "If a refresh fails, previous states are marked stale. Doctor findings, batch failures and backend timeout messages appear with their actual exit status in Activity. After a shutdown timeout, Force stop is an explicit separate action.", NULL);
    return page;
}
void gui_window_present(gui_t *gui) {
    if (gui->closing) return;
    if (gui->window) { gtk_window_present(gui->window); return; }
    gui->window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(gui->application)));
    gtk_window_set_title(gui->window, "Waddle");
    gtk_window_set_default_size(gui->window, gui_store_number(gui->store, "width", 1180, 720, 2400), gui_store_number(gui->store, "height", 820, 540, 1600));
    g_signal_connect(gui->window, "close-request", G_CALLBACK(close_window), gui);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
    gui->title = ADW_WINDOW_TITLE(adw_window_title_new("Waddle", "Windows devices"));
    adw_header_bar_set_title_widget(header, GTK_WIDGET(gui->title));
    adw_header_bar_pack_start(header, gui_button("Commands", "system-search-symbolic", "palette", FALSE));
    adw_header_bar_pack_end(header, gui_button("Settings", "preferences-system-symbolic", "preferences", FALSE));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(header));
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gui->pages = GTK_STACK(gtk_stack_new());
    GtkWidget *sidebar = gtk_stack_sidebar_new(); gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), gui->pages);
    gtk_widget_set_size_request(sidebar, 160, -1); gtk_widget_add_css_class(sidebar, "navigation-sidebar");
    gtk_paned_set_start_child(GTK_PANED(paned), sidebar); gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_end_child(GTK_PANED(paned), GTK_WIDGET(gui->pages));
    gtk_stack_add_titled(gui->pages, dashboard(gui), "devices", "Devices");
    gtk_stack_add_titled(gui->pages, gui_library_page(gui), "applications", "Applications");
    gtk_stack_add_titled(gui->pages, activity_page(gui), "activity", "Activity");
    gtk_stack_add_titled(gui->pages, diagnostics_page(), "diagnostics", "Diagnostics");
    gtk_stack_add_titled(gui->pages, gui_preferences_page(gui), "preferences", "Settings");
    gtk_box_append(GTK_BOX(root), paned);
    gui->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new()); adw_toast_overlay_set_child(gui->toasts, root);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(gui->window), GTK_WIDGET(gui->toasts));
    gui_preferences_apply(gui);
    char *landing = gui_store_text(gui->store, "Preferences", "landing", "devices");
    if (!strcmp(landing, "restore")) { g_free(landing); landing = gui_store_text(gui->store, "Preferences", "last_page", "devices"); }
    if (gtk_stack_get_child_by_name(gui->pages, landing)) gtk_stack_set_visible_child_name(gui->pages, landing);
    if (!gui->device_requested) gui->initial_page = g_strdup(landing);
    g_free(landing);
    if (!gui->requested) gui->requested = gui_store_text(gui->store, "Preferences", "last_device", NULL);
    gtk_window_present(gui->window);
    gui_refresh(gui);
    gui->refresh_timer = g_timeout_add_seconds(5, refresh_tick, gui);
}
