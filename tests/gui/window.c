/** @file window.c @brief Native GTK integration against deterministic streamed CLI peer. */
#include "gui_private.h"
#include <glib/gstdio.h>
#include <string.h>
#include <stdio.h>

static void pump(unsigned milliseconds) {
    gint64 until = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < until);
}
static void idle(gui_t *gui) {
    gint64 until = g_get_monotonic_time() + 12 * G_USEC_PER_SEC;
    do { pump(10); } while ((gui->job || gui->list_job || gui->detail_job) && g_get_monotonic_time() < until);
    g_assert_null(gui->job); g_assert_null(gui->list_job); g_assert_null(gui->detail_job);
}
static GtkWidget *find_widget(GtkWidget *root, GType type, const char *title) {
    if (g_type_is_a(G_OBJECT_TYPE(root), type)) {
        const char *text = NULL;
        if (ADW_IS_PREFERENCES_ROW(root)) text = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(root));
        else if (GTK_IS_BUTTON(root)) {
            text = gtk_button_get_label(GTK_BUTTON(root));
            if (!text) { GtkWidget *label = find_widget(gtk_button_get_child(GTK_BUTTON(root)), GTK_TYPE_LABEL, NULL); if (label) text = gtk_label_get_text(GTK_LABEL(label)); }
        }
        else if (GTK_IS_CHECK_BUTTON(root)) text = gtk_check_button_get_label(GTK_CHECK_BUTTON(root));
        if (!title || g_strcmp0(title, text) == 0) return root;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, type, title); if (found) return found;
    }
    return NULL;
}
static GtkWindow *other_window(gui_t *gui) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        GtkWindow *window = g_list_model_get_item(windows, i);
        gboolean match = window != gui->window && gtk_widget_get_visible(GTK_WIDGET(window));
        g_object_unref(window); if (match) return window;
    }
    return NULL;
}
static void click(GtkWindow *window, const char *label) {
    GtkWidget *button = find_widget(GTK_WIDGET(window), GTK_TYPE_BUTTON, label); g_assert_nonnull(button);
    g_signal_emit_by_name(button, "clicked"); pump(20);
}
static void set_field(GtkWindow *window, const char *title, const char *value) {
    GtkWidget *entry = find_widget(GTK_WIDGET(window), ADW_TYPE_ENTRY_ROW, title); g_assert_nonnull(entry);
    gtk_editable_set_text(GTK_EDITABLE(entry), value);
}
static char *view_text(GtkTextView *view) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view); GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer, &a, &b); return gtk_text_buffer_get_text(buffer, &a, &b, FALSE);
}
static void activate_action(GSimpleAction *action, GVariant *parameter, gpointer context) { (void)action; gui_dispatch(context, g_variant_get_string(parameter, NULL)); }
/** @brief Integration entry; display provided by Xvfb, no Windows/hypervisor required. */
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    char *root = g_dir_make_tmp("waddle-gui-window-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", root, TRUE); g_setenv("WADDLE_GUI_TEST_DIR", root, TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);
    adw_init();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    gui_t gui = {0}; gui.cli = g_canonicalize_filename("tests/gui/fixture.py", NULL);
    gui.store = gui_store_load(NULL); gui.terminals = g_ptr_array_new();
    gui.application = adw_application_new("io.github.Kayyo321.Waddle.Test", G_APPLICATION_NON_UNIQUE);
    GError *error = NULL; g_assert_true(g_application_register(G_APPLICATION(gui.application), NULL, &error)); g_assert_no_error(error);
    GSimpleAction *action = g_simple_action_new("action", G_VARIANT_TYPE_STRING);
    g_signal_connect(action, "activate", G_CALLBACK(activate_action), &gui); g_action_map_add_action(G_ACTION_MAP(gui.application), G_ACTION(action)); g_object_unref(action);
    g_print("Window construction and initial refresh\n");
    gui_window_present(&gui); idle(&gui);
    g_assert_cmpstr(gui.selected, ==, "alpha"); g_assert_nonnull(gui_device_data(&gui, "broken"));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(gui.settings[0])), ==, "4096");
    g_assert_nonnull(gtk_widget_get_first_child(GTK_WIDGET(gui.folders)));
    g_print("Initial state loaded\n");
    /* Polls preserve selection and unsaved settings, including external CLI changes. */
    gui_select(&gui, "beta"); idle(&gui);
    gtk_editable_set_text(GTK_EDITABLE(gui.settings[0]), "8192");
    gui_refresh(&gui); idle(&gui); g_assert_cmpstr(gui.selected, ==, "beta");
    gui_load_details(&gui); idle(&gui); g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(gui.settings[0])), ==, "8192");
    gui_dispatch(&gui, "config-apply");
#if ADW_CHECK_VERSION(1, 5, 0)
    AdwDialog *alert = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(gui.window)); g_assert_nonnull(alert);
    g_signal_emit_by_name(alert, "response", "cancel"); adw_dialog_close(alert);
#else
    GtkWindow *confirm = other_window(&gui); g_assert_nonnull(confirm);
    adw_message_dialog_response(ADW_MESSAGE_DIALOG(confirm), "cancel"); gtk_window_destroy(confirm);
#endif
    pump(20); g_assert_null(gui.job);
    GtkWindow *dialog;
    /* Search/filter and unavailable backend controls are real native widgets. */
    gtk_editable_set_text(GTK_EDITABLE(gui.device_search), "alpha"); gtk_drop_down_set_selected(gui.filter, 2); pump(200);
    gui_dispatch(&gui, "desktop");
#if ADW_CHECK_VERSION(1, 5, 0)
    alert = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(gui.window)); g_assert_nonnull(alert);
    g_signal_emit_by_name(alert, "response", "close"); adw_dialog_close(alert);
#else
    dialog = other_window(&gui); g_assert_nonnull(dialog); adw_message_dialog_response(ADW_MESSAGE_DIALOG(dialog), "close"); gtk_window_destroy(dialog);
#endif
    pump(20);
    g_assert_null(gui.job);
    g_print("Create wizard\n");
    /* Create wizard validates name/resources and uses exact backend dry-run argv. */
    gui_dispatch(&gui, "create"); dialog = other_window(&gui); g_assert_nonnull(dialog);
    set_field(dialog, "Device name", "../bad"); click(dialog, "Continue"); g_assert_null(gui.job);
    set_field(dialog, "Device name", "new_vm");
    GtkWidget *source = find_widget(GTK_WIDGET(dialog), GTK_TYPE_DROP_DOWN, NULL); g_assert_nonnull(source); gtk_drop_down_set_selected(GTK_DROP_DOWN(source), 1);
    click(dialog, "Continue"); click(dialog, "Continue"); click(dialog, "Submit reviewed operation"); idle(&gui);
    g_assert_null(gui_device_data(&gui, "new_vm"));
    GtkWidget *preview = find_widget(GTK_WIDGET(dialog), GTK_TYPE_CHECK_BUTTON, "Preview only (backend dry run)"); g_assert_nonnull(preview);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(preview), FALSE); click(dialog, "Submit reviewed operation"); idle(&gui);
    g_assert_nonnull(gui_device_data(&gui, "new_vm")); gtk_window_destroy(dialog); pump(20);
    g_print("Launcher library\n");
    /* Export selects an existing parent and a new, nonexisting backup leaf. */
    gui_select(&gui, "alpha"); idle(&gui); gui_dispatch(&gui, "export"); dialog = other_window(&gui); g_assert_nonnull(dialog);
    set_field(dialog, "Parent directory for backup", root);
    set_field(dialog, "New backup folder name (letters, digits, - or _)", "portable");
    click(dialog, "Submit reviewed operation"); idle(&gui);
    char *destination = g_build_filename(root, "portable", NULL);
    gboolean destination_seen = FALSE;
    for (guint i = 0; gui.last_argv[i]; ++i) destination_seen |= !strcmp(gui.last_argv[i], destination);
    g_assert_true(destination_seen); g_free(destination); gtk_window_destroy(dialog); pump(20);
    /* Native launcher editor preserves argv and launches via unchanged exec. */
    gui_select(&gui, "alpha"); idle(&gui); gui_launcher_edit(&gui, NULL); dialog = other_window(&gui); g_assert_nonnull(dialog);
    set_field(dialog, "Launcher name", "Test launcher"); set_field(dialog, "Executable", "program path.exe");
    set_field(dialog, "Arguments (quoted argv)", "'a b' '$(literal)'"); click(dialog, "Save launcher"); pump(20);
    g_assert_nonnull(gtk_widget_get_first_child(GTK_WIDGET(gui.launchers)));
    click(gui.window, "Launch"); idle(&gui); g_assert_cmpstr(gui.last_argv[0], ==, "exec");
    gboolean literal = FALSE; for (guint i = 0; gui.last_argv[i]; ++i) literal |= !strcmp(gui.last_argv[i], "$(literal)"); g_assert_true(literal);
    /* Graphical mode obtains a validated PID and attaches the existing AV command. */
    GtkWidget *edit = find_widget(GTK_WIDGET(gui.launchers), GTK_TYPE_BUTTON, NULL);
    (void)edit;
    gsize group_count = 0; char **groups = g_key_file_get_groups(gui.store->keys, &group_count);
    for (gsize i = 0; i < group_count; ++i) if (g_str_has_prefix(groups[i], "Launcher ")) g_key_file_set_integer(gui.store->keys, groups[i], "mode", 2);
    g_strfreev(groups); click(gui.window, "Launch"); idle(&gui);
    g_assert_cmpstr(gui.last_argv[0], ==, "av"); g_assert_cmpstr(gui.last_argv[2], ==, "1234");
    /* Partial failure retains actual status and its originating device. */
    gui_dispatch(&gui, "doctor-all"); idle(&gui); char *text = view_text(gui.output); g_assert_nonnull(strstr(text, "exit 1")); g_free(text);
    /* Explicit batch selection continues after a per-device failure. */
    gui_dispatch(&gui, "batch"); dialog = other_window(&gui); g_assert_nonnull(dialog);
    const char *batch_names[] = {"alpha", "beta", "broken", "new_vm"};
    for (guint i = 0; i < G_N_ELEMENTS(batch_names); ++i) {
        GtkWidget *check = find_widget(GTK_WIDGET(dialog), GTK_TYPE_CHECK_BUTTON, batch_names[i]); g_assert_nonnull(check);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
    }
    click(dialog, "Run selected operation"); idle(&gui); g_assert_cmpstr(gui.operation_device, ==, "new_vm");
    const char *unsubmitted[] = {"alpha", "beta"};
    g_key_file_set_string_list(gui.store->keys, "Session", "batch_names", unsubmitted, 2);
    g_key_file_set_string(gui.store->keys, "Session", "batch_operation", "status");
    char *real_cli = gui.cli; gui.cli = g_strdup("/waddle/no-cli");
    gui_dispatch(&gui, "batch-next"); g_assert_false(g_key_file_has_group(gui.store->keys, "Session"));
    g_free(gui.cli); gui.cli = real_cli;
    /* Live log output streams while other UI remains responsive; stop only reader. */
    gtk_check_button_set_active(gui.log_follow, FALSE); gui_dispatch(&gui, "logs-read"); idle(&gui);
    text = view_text(gui.logs); g_assert_cmpstr(text, ==, "fixture log: ready\n"); g_free(text);
    gtk_check_button_set_active(gui.log_follow, TRUE); gui_dispatch(&gui, "logs-read"); pump(200);
    g_assert_nonnull(gui.job); gtk_stack_set_visible_child_name(gui.pages, "preferences");
    gui_dispatch(&gui, "logs-stop"); idle(&gui); text = view_text(gui.logs); g_assert_nonnull(strstr(text, "fixture log")); g_free(text);
    gui_dispatch(&gui, "logs-read"); pump(100); gui_select(&gui, "beta"); idle(&gui);
    text = view_text(gui.logs); g_assert_cmpstr(text, ==, ""); g_free(text); g_assert_cmpstr(gui.operation_device, ==, "alpha");
    gui_select(&gui, "alpha"); idle(&gui);
    g_print("VTE interaction\n");
    /* VTE owns PTY, supports input, resize, status and retains exited output. */
    gui_terminal_open(&gui); pump(500); g_assert_cmpuint(gui.terminals->len, ==, 1);
    GtkWidget *notebook = g_object_get_data(G_OBJECT(gui.window), "terminal_notebook");
    GtkWidget *terminal = find_widget(notebook, VTE_TYPE_TERMINAL, NULL); g_assert_nonnull(terminal);
    vte_terminal_set_size(VTE_TERMINAL(terminal), 100, 30); vte_terminal_feed_child(VTE_TERMINAL(terminal), "hello\nexit\n", -1); pump(500);
#if VTE_CHECK_VERSION(0, 80, 0)
    char *terminal_text = vte_terminal_get_text_format(VTE_TERMINAL(terminal), VTE_FORMAT_TEXT);
#else
    char *terminal_text = vte_terminal_get_text(VTE_TERMINAL(terminal), NULL, NULL, NULL);
#endif
    g_assert_nonnull(strstr(terminal_text, "TERMINAL_ECHO:hello")); g_assert_nonnull(strstr(terminal_text, "FIXTURE_TERMINAL_EXIT")); g_free(terminal_text);
    GtkWidget *terminal_page = gtk_stack_get_child_by_name(gui.details, "terminal");
    GtkWidget *search = find_widget(terminal_page, GTK_TYPE_SEARCH_ENTRY, NULL); g_assert_nonnull(search);
    gtk_editable_set_text(GTK_EDITABLE(search), "TERMINAL_ECHO"); pump(250);
    click(gui.window, "Zoom in"); click(gui.window, "Zoom out");
    /* Malformed observations retain the old list and mark states stale. */
    char *malformed = g_build_filename(root, "malformed", NULL); g_file_set_contents(malformed, "1", -1, NULL);
    gui_refresh(&gui); idle(&gui); g_assert_true(gui.stale); g_assert_nonnull(gui_device_data(&gui, "alpha")); g_remove(malformed); g_free(malformed);
    /* Light/dark preferences and palette scope. */
    g_key_file_set_integer(gui.store->keys, "Preferences", "appearance", 2); gui_preferences_apply(&gui);
    g_assert_true(adw_style_manager_get_dark(adw_style_manager_get_default()));
    gui_palette_open(&gui); dialog = other_window(&gui); g_assert_nonnull(dialog); gtk_window_destroy(dialog);
    /* Closing with an active operation and PTY leaves all VM commands untouched. */
    const char *long_run[] = {"long", NULL}; gui_run(&gui, "Long operation", "alpha", long_run);
    gui_terminal_open(&gui); pump(100); gtk_window_close(gui.window); idle(&gui); pump(200);
    g_assert_true(gui.closing); gui_terminal_cleanup(&gui); g_ptr_array_unref(gui.terminals);
    g_clear_object(&gui.registry); gui_store_free(gui.store); g_strfreev(gui.last_argv);
    for (guint i = 0; i < GuiSettingCount; ++i) g_free(gui.originals[i]);
    g_free(gui.cli); g_free(gui.selected); g_free(gui.requested); g_free(gui.operation); g_free(gui.operation_device); g_free(gui.pending_av_device); g_free(gui.config_device); g_free(gui.initial_page);
    g_object_unref(gui.application);
    g_print("Native GTK workflow, streaming, VTE and cleanup checks passed.\n");
    g_free(root); return 0;
}
