/** @file gui_library.c @brief Saved launchers, preferences, command palette and exports. */
#include "gui_private.h"
#include <string.h>

void gui_store_flush(gui_t *gui) {
    GError *error = NULL;
    if (!gui_store_save(gui->store, &error)) { gui_message(gui, error->message); g_clear_error(&error); }
}
void gui_preferences_apply(gui_t *gui) {
    int appearance = gui_store_number(gui->store, "appearance", 0, 0, 2);
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), appearance == 1 ? ADW_COLOR_SCHEME_FORCE_LIGHT : appearance == 2 ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_DEFAULT);
    if (gui->devices) {
        if (gui_store_number(gui->store, "compact", 0, 0, 1)) gtk_widget_add_css_class(GTK_WIDGET(gui->devices), "data-table");
        else gtk_widget_remove_css_class(GTK_WIDGET(gui->devices), "data-table");
    }
    gui_terminal_preferences(gui);
}
static void preference_drop(GObject *object, GParamSpec *property, gpointer context) {
    (void)property; gui_t *gui = context;
    const char *key = g_object_get_data(object, "preference");
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (!strcmp(key, "landing")) {
        const char *values[] = {"devices", "applications", "activity", "diagnostics", "restore"};
        g_key_file_set_string(gui->store->keys, "Preferences", key, values[MIN(selected, 4)]);
    } else g_key_file_set_integer(gui->store->keys, "Preferences", key, (int)selected);
    gui_preferences_apply(gui); gui_store_flush(gui);
}
static void preference_switch(GObject *object, GParamSpec *property, gpointer context) {
    (void)property; gui_t *gui = context;
    g_key_file_set_integer(gui->store->keys, "Preferences", g_object_get_data(object, "preference"), gtk_switch_get_active(GTK_SWITCH(object)));
    gui_preferences_apply(gui); gui_store_flush(gui);
}
static void preference_entry(AdwEntryRow *entry, gpointer context) {
    gui_t *gui = context;
    const char *key = g_object_get_data(G_OBJECT(entry), "preference");
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!strcmp(key, "terminal_scrollback") || !strcmp(key, "activity_retention")) {
        char *end = NULL; gint64 number = g_ascii_strtoll(text, &end, 10);
        int minimum = !strcmp(key, "activity_retention") ? 5 : 100;
        int maximum = !strcmp(key, "activity_retention") ? 200 : 100000;
        if (!*text || !end || *end || number < minimum || number > maximum) { gui_message(gui, "Preference is outside its displayed range."); return; }
        g_key_file_set_integer(gui->store->keys, "Preferences", key, (int)number);
    } else g_key_file_set_string(gui->store->keys, "Preferences", key, text);
    gui_preferences_apply(gui); gui_store_flush(gui);
}
static AdwActionRow *preference_row(AdwPreferencesGroup *group, const char *title, const char *subtitle) {
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(row, subtitle);
    adw_preferences_group_add(group, GTK_WIDGET(row)); return row;
}
static void preference_toggle(gui_t *gui, AdwPreferencesGroup *group, const char *key, const char *title, const char *description) {
    AdwActionRow *row = preference_row(group, title, description);
    GtkWidget *toggle = gtk_switch_new(); gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(toggle), gui_store_number(gui->store, key, 0, 0, 1));
    g_object_set_data(G_OBJECT(toggle), "preference", (gpointer)key);
    g_signal_connect(toggle, "notify::active", G_CALLBACK(preference_switch), gui);
    adw_action_row_add_suffix(row, toggle); adw_action_row_set_activatable_widget(row, toggle);
}
static void preference_text(gui_t *gui, AdwPreferencesGroup *group, const char *key, const char *title, const char *fallback) {
    AdwEntryRow *row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    char *value = gui_store_text(gui->store, "Preferences", key, fallback);
    gtk_editable_set_text(GTK_EDITABLE(row), value); g_free(value);
    adw_entry_row_set_show_apply_button(row, TRUE);
    g_object_set_data(G_OBJECT(row), "preference", (gpointer)key);
    g_signal_connect(row, "apply", G_CALLBACK(preference_entry), gui);
    adw_preferences_group_add(group, GTK_WIDGET(row));
}
static void about_clicked(GtkButton *button, gpointer context) {
    (void)button; gui_t *gui = context;
    GtkWidget *about = adw_about_window_new();
    adw_about_window_set_application_name(ADW_ABOUT_WINDOW(about), "Waddle");
    adw_about_window_set_application_icon(ADW_ABOUT_WINDOW(about), "io.github.Kayyo321.Waddle");
    adw_about_window_set_developer_name(ADW_ABOUT_WINDOW(about), "Waddle contributors");
    adw_about_window_set_version(ADW_ABOUT_WINDOW(about), "0.1.0-alpha · CLI protocol v1");
    adw_about_window_set_license_type(ADW_ABOUT_WINDOW(about), GTK_LICENSE_GPL_3_0);
    adw_about_window_set_website(ADW_ABOUT_WINDOW(about), "https://github.com/Kayyo321/Waddle-LSW");
    adw_about_window_set_issue_url(ADW_ABOUT_WINDOW(about), "https://github.com/Kayyo321/Waddle-LSW/issues");
    gtk_window_set_transient_for(GTK_WINDOW(about), gui->window); gtk_window_set_destroy_with_parent(GTK_WINDOW(about), TRUE);
    gtk_window_present(GTK_WINDOW(about));
}
GtkWidget *gui_preferences_page(gui_t *gui) {
    GtkWidget *box; GtkWidget *page = gui_page(&box, "Settings", "Application preferences are separate from each device's Configuration page.");
    AdwPreferencesGroup *general = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(general, "Appearance and navigation"); gtk_box_append(GTK_BOX(box), GTK_WIDGET(general));
    const char *appearance[] = {"System", "Light", "Dark", NULL};
    AdwActionRow *row = preference_row(general, "Appearance", "Follows the desktop unless overridden");
    GtkWidget *drop = gtk_drop_down_new_from_strings(appearance);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), (guint)gui_store_number(gui->store, "appearance", 0, 0, 2));
    g_object_set_data(G_OBJECT(drop), "preference", "appearance"); g_signal_connect(drop, "notify::selected", G_CALLBACK(preference_drop), gui); adw_action_row_add_suffix(row, drop);
    preference_toggle(gui, general, "compact", "Compact device list", "Use a denser list for many devices");
    preference_toggle(gui, general, "notifications", "Desktop notifications", "Report operation completion and failure");
    row = preference_row(general, "Landing page", "Window size and last selected device are remembered");
    const char *landings[] = {"Devices", "Applications", "Activity", "Diagnostics", "Restore last page", NULL};
    const char *landing_values[] = {"devices", "applications", "activity", "diagnostics", "restore"};
    drop = gtk_drop_down_new_from_strings(landings); char *landing = gui_store_text(gui->store, "Preferences", "landing", "devices");
    for (guint i = 0; i < G_N_ELEMENTS(landing_values); ++i) if (!strcmp(landing_values[i], landing)) gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), i);
    g_free(landing); g_object_set_data(G_OBJECT(drop), "preference", "landing"); g_signal_connect(drop, "notify::selected", G_CALLBACK(preference_drop), gui); adw_action_row_add_suffix(row, drop);
    AdwPreferencesGroup *terminal = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(terminal, "Terminal and output"); gtk_box_append(GTK_BOX(box), GTK_WIDGET(terminal));
    preference_text(gui, terminal, "terminal_font", "Terminal font and size (Pango description)", "Monospace 11");
    preference_text(gui, terminal, "terminal_scrollback", "Terminal scrollback lines (100–100000)", "10000");
    preference_text(gui, terminal, "external_terminal", "External terminal executable (blank selects automatically)", "");
    preference_text(gui, terminal, "activity_retention", "Activity records to retain (5–200)", "50");
    preference_toggle(gui, terminal, "log_follow", "Follow logs by default", "Applies when the next Waddle window opens");
    gtk_box_append(GTK_BOX(box), gui_button("Clear persistent default device", "edit-clear-symbolic", "default-clear", FALSE));
    gui_label(box, "Keyboard shortcuts", "title-3");
    gui_label(box, "Ctrl+K — Command palette\nF5 — Refresh device states\nCtrl+Shift+T — New terminal for selected device\nTerminal: Ctrl+Shift+C / Ctrl+Shift+V — Copy / paste\nUse Tab and Shift+Tab to move through controls; Enter activates focused actions. Animations and reduced motion follow GTK system settings.", NULL);
    GtkWidget *about = gtk_button_new_with_label("About Waddle"); g_signal_connect(about, "clicked", G_CALLBACK(about_clicked), gui); gtk_box_append(GTK_BOX(box), about);
    return page;
}
/** @brief Launcher editor owns group and child widgets until its window is finalized. */
typedef struct launcher_editor_t {
    gui_t *gui; GtkWindow *window; char *group; AdwEntryRow *fields[8]; GtkDropDown *mode;
    GtkCheckButton *translate, *pin; GtkLabel *error;
} launcher_editor_t;
static const char *const launcher_keys[] = {"name", "device", "executable", "arguments", "cwd", "environment", "options", "path_maps"};
static const char *const launcher_labels[] = {"Launcher name", "Device name", "Executable", "Arguments (quoted argv)", "Guest working directory (optional)", "Environment assignments (quoted argv)", "Additional execution options (quoted argv)", "Path mappings SRC=DEST (quoted argv)"};
static void editor_free(gpointer context) { launcher_editor_t *editor = context; g_free(editor->group); g_free(editor); }
static void editor_save(GtkButton *button, gpointer context) {
    (void)button; launcher_editor_t *editor = context;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(editor->fields[0]));
    const char *device = gtk_editable_get_text(GTK_EDITABLE(editor->fields[1]));
    const char *exe = gtk_editable_get_text(GTK_EDITABLE(editor->fields[2]));
    if (!*name || !*exe || !gui_name_valid(device)) { gtk_label_set_text(editor->error, "Enter a launcher name, executable and valid device name."); return; }
    for (guint i = 0; i < G_N_ELEMENTS(launcher_keys); ++i)
        g_key_file_set_string(editor->gui->store->keys, editor->group, launcher_keys[i], gtk_editable_get_text(GTK_EDITABLE(editor->fields[i])));
    g_key_file_set_integer(editor->gui->store->keys, editor->group, "mode", (int)gtk_drop_down_get_selected(editor->mode));
    g_key_file_set_boolean(editor->gui->store->keys, editor->group, "translate", gtk_check_button_get_active(editor->translate));
    g_key_file_set_boolean(editor->gui->store->keys, editor->group, "pin", gtk_check_button_get_active(editor->pin));
    gui_store_flush(editor->gui); gui_library_refresh(editor->gui); gtk_window_destroy(editor->window);
}
void gui_launcher_edit(gui_t *gui, const char *group) {
    launcher_editor_t *editor = g_new0(launcher_editor_t, 1); editor->gui = gui;
    if (group) editor->group = g_strdup(group);
    else { char *id = g_uuid_string_random(); editor->group = g_strconcat("Launcher ", id, NULL); g_free(id); }
    editor->window = GTK_WINDOW(adw_window_new()); gtk_window_set_transient_for(editor->window, gui->window);
    gtk_window_set_destroy_with_parent(editor->window, TRUE); gtk_window_set_modal(editor->window, TRUE);
    gtk_window_set_title(editor->window, "Saved application launcher"); gtk_window_set_default_size(editor->window, 680, 760);
    GtkWidget *box; GtkWidget *page = gui_page(&box, group ? "Edit launcher" : "New launcher", "Launchers always name their device. Quoting splits arguments without executing a local shell.");
    AdwPreferencesGroup *fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new()); gtk_box_append(GTK_BOX(box), GTK_WIDGET(fields));
    for (guint i = 0; i < G_N_ELEMENTS(launcher_keys); ++i) {
        editor->fields[i] = ADW_ENTRY_ROW(adw_entry_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor->fields[i]), launcher_labels[i]);
        char *value = gui_store_text(gui->store, editor->group, launcher_keys[i], i == 1 ? gui->selected : "");
        gtk_editable_set_text(GTK_EDITABLE(editor->fields[i]), value ? value : ""); g_free(value);
        adw_preferences_group_add(fields, GTK_WIDGET(editor->fields[i]));
    }
    const char *modes[] = {"Command with output", "Embedded terminal", "Seamless graphical application (Windows path)", NULL};
    editor->mode = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes));
    int mode = g_key_file_get_integer(gui->store->keys, editor->group, "mode", NULL);
    gtk_drop_down_set_selected(editor->mode, (guint)CLAMP(mode, 0, 2)); gtk_box_append(GTK_BOX(box), GTK_WIDGET(editor->mode));
    editor->translate = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Translate Linux paths"));
    gtk_check_button_set_active(editor->translate, group ? g_key_file_get_boolean(gui->store->keys, group, "translate", NULL) : TRUE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(editor->translate));
    editor->pin = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Pin launcher")); gtk_check_button_set_active(editor->pin, group ? g_key_file_get_boolean(gui->store->keys, group, "pin", NULL) : FALSE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(editor->pin));
    gui_label(box, "Graphical mode starts the Windows executable through the existing guest execution bridge, obtains its PID, then attaches the existing managed AV viewer. Run AV setup first. Use Windows paths in graphical mode; the existing viewer owns display/audio/input. Output and failures appear in Activity.", "dim-label");
    editor->error = GTK_LABEL(gui_label(box, "", "warning"));
    GtkWidget *save = gtk_button_new_with_label("Save launcher"); gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(save, "clicked", G_CALLBACK(editor_save), editor); gtk_box_append(GTK_BOX(box), save);
    adw_window_set_content(ADW_WINDOW(editor->window), page); g_object_set_data_full(G_OBJECT(editor->window), "editor", editor, editor_free);
    gtk_window_present(editor->window);
}
static gboolean add_parsed(GPtrArray *argv, const char *text, const char *flag, GError **error) {
    if (!text || !*text) return TRUE;
    int count = 0; char **words = NULL;
    if (!g_shell_parse_argv(text, &count, &words, error)) return FALSE;
    for (int i = 0; i < count; ++i) { if (flag) gui_args_add(argv, flag); gui_args_add(argv, words[i]); }
    g_strfreev(words); return TRUE;
}
static char *powershell_quote(const char *value) {
    GString *quoted = g_string_new("'");
    for (const char *p = value; *p; ++p) { g_string_append_c(quoted, *p); if (*p == '\'') g_string_append_c(quoted, '\''); }
    g_string_append_c(quoted, '\''); return g_string_free(quoted, FALSE);
}
/* Quote one argument for Windows CommandLineToArgvW/CRT, then quote the resulting
 * entire command-line as a PowerShell literal. No caller text becomes script. */
static char *windows_arguments(char **words) {
    GString *line = g_string_new(NULL);
    for (guint i = 0; words && words[i]; ++i) {
        if (i) g_string_append_c(line, ' ');
        g_string_append_c(line, '"');
        guint slashes = 0;
        for (const char *p = words[i]; ; ++p) {
            if (*p == '\\') { ++slashes; continue; }
            if (*p == '"' || !*p) {
                for (guint j = 0; j < slashes * 2; ++j) g_string_append_c(line, '\\');
                if (*p == '"') g_string_append_c(line, '\\');
            } else for (guint j = 0; j < slashes; ++j) g_string_append_c(line, '\\');
            slashes = 0;
            if (!*p) break;
            g_string_append_c(line, *p);
        }
        g_string_append_c(line, '"');
    }
    return g_string_free(line, FALSE);
}
static void launcher_run(gui_t *gui, const char *group) {
    if (gui->job) { gui_message(gui, "Wait for the active operation or stop it in Activity."); return; }
    char *values[8];
    for (guint i = 0; i < G_N_ELEMENTS(values); ++i) values[i] = gui_store_text(gui->store, group, launcher_keys[i], "");
    if (!gui_name_valid(values[1]) || !gui_device_data(gui, values[1]) || !*values[2]) {
        gui_message(gui, "The launcher's device or executable is unavailable. Edit the launcher or refresh Devices."); goto cleanup;
    }
    int mode = g_key_file_get_integer(gui->store->keys, group, "mode", NULL);
    GPtrArray *args = gui_args_new("exec", "--device"); gui_args_add(args, values[1]);
    gui_args_add(args, mode == 1 ? "--interactive" : "--pipe");
    if (g_key_file_get_boolean(gui->store->keys, group, "translate", NULL)) gui_args_add(args, "--translate-path");
    if (*values[4]) { gui_args_add(args, "--cwd"); gui_args_add(args, values[4]); }
    GError *error = NULL;
    if (!add_parsed(args, values[5], "--env", &error) || !add_parsed(args, values[6], NULL, &error) || !add_parsed(args, values[7], "--path-map", &error)) goto invalid;
    gui_args_add(args, "--");
    if (mode == 2) {
        int count = 0; char **words = NULL;
        if (*values[3] && !g_shell_parse_argv(values[3], &count, &words, &error)) goto invalid;
        char *exe = powershell_quote(values[2]); char *command_line = windows_arguments(words);
        char *arguments = powershell_quote(command_line); char *cwd = *values[4] ? powershell_quote(values[4]) : NULL;
        char *script = g_strdup_printf("$ErrorActionPreference='Stop'; $p=Start-Process -FilePath %s %s%s%s%s -PassThru; @{pid=$p.Id}|ConvertTo-Json -Compress", exe,
            *command_line ? "-ArgumentList " : "", *command_line ? arguments : "",
            cwd ? " -WorkingDirectory " : "", cwd ? cwd : "");
        gui_args_add(args, "powershell.exe"); gui_args_add(args, "-NoProfile"); gui_args_add(args, "-NonInteractive");
        gui_args_add(args, "-Command"); gui_args_add(args, script);
        g_strfreev(words); g_free(exe); g_free(command_line); g_free(arguments); g_free(cwd); g_free(script);
        g_free(gui->pending_av_device); gui->pending_av_device = g_strdup(values[1]);
    } else {
        gui_args_add(args, values[2]); if (!add_parsed(args, values[3], NULL, &error)) goto invalid;
    }
    g_ptr_array_add(args, NULL);
    gboolean started = TRUE;
    if (mode == 1) gui_terminal_command(gui, values[1], (const char *const *)args->pdata);
    else started = gui_run(gui, values[0], values[1], (const char *const *)args->pdata);
    if (!started) g_clear_pointer(&gui->pending_av_device, g_free);
    if (started) { g_key_file_set_int64(gui->store->keys, group, "recent", (gint64)(g_get_real_time() / G_USEC_PER_SEC)); gui_store_flush(gui); gui_library_refresh(gui); }
    g_ptr_array_unref(args); goto cleanup;
invalid:
    gui_message(gui, error ? error->message : "Invalid launcher arguments."); g_clear_error(&error); g_ptr_array_unref(args);
cleanup:
    for (guint i = 0; i < G_N_ELEMENTS(values); ++i) g_free(values[i]);
}
static void library_button(GtkButton *button, gpointer context) {
    gui_t *gui = context;
    const char *group = g_object_get_data(G_OBJECT(button), "group");
    const char *action = g_object_get_data(G_OBJECT(button), "library_action");
    if (!strcmp(action, "run")) launcher_run(gui, group);
    else if (!strcmp(action, "edit")) gui_launcher_edit(gui, group);
    else if (!strcmp(action, "duplicate")) {
        char *id = g_uuid_string_random(); char *copy = g_strconcat("Launcher ", id, NULL); g_free(id);
        gsize count = 0; char **keys = g_key_file_get_keys(gui->store->keys, group, &count, NULL);
        for (gsize i = 0; keys && i < count; ++i) { char *value = g_key_file_get_value(gui->store->keys, group, keys[i], NULL); g_key_file_set_value(gui->store->keys, copy, keys[i], value); g_free(value); }
        g_strfreev(keys); gui_launcher_edit(gui, copy); g_free(copy);
    } else if (!strcmp(action, "pin")) {
        g_key_file_set_boolean(gui->store->keys, group, "pin", !g_key_file_get_boolean(gui->store->keys, group, "pin", NULL)); gui_store_flush(gui); gui_library_refresh(gui);
    } else if (!strcmp(action, "remove")) {
        /* Removes only a UI launcher definition; never any guest executable. */
        g_key_file_remove_group(gui->store->keys, group, NULL); gui_store_flush(gui); gui_library_refresh(gui);
    }
}
static int library_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer context) {
    gui_t *gui = context; const char *left = g_object_get_data(G_OBJECT(a), "group"), *right = g_object_get_data(G_OBJECT(b), "group");
    if (!left || !right) return 0;
    gboolean pin_left = g_key_file_get_boolean(gui->store->keys, left, "pin", NULL), pin_right = g_key_file_get_boolean(gui->store->keys, right, "pin", NULL);
    if (pin_left != pin_right) return pin_left ? -1 : 1;
    gint64 recent_left = g_key_file_get_int64(gui->store->keys, left, "recent", NULL), recent_right = g_key_file_get_int64(gui->store->keys, right, "recent", NULL);
    if (recent_left != recent_right) return recent_left > recent_right ? -1 : 1;
    return g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(a)), adw_preferences_row_get_title(ADW_PREFERENCES_ROW(b)));
}
void gui_library_refresh(gui_t *gui) {
    GtkWidget *child; while ((child = gtk_widget_get_first_child(GTK_WIDGET(gui->launchers)))) gtk_list_box_remove(gui->launchers, child);
    gsize count = 0; char **groups = g_key_file_get_groups(gui->store->keys, &count); guint found = 0;
    for (gsize i = 0; i < count; ++i) {
        if (!g_str_has_prefix(groups[i], "Launcher ")) continue;
        ++found; AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new()); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        char *name = gui_store_text(gui->store, groups[i], "name", "Unnamed launcher"); char *device = gui_store_text(gui->store, groups[i], "device", "Unavailable");
        char *exe = gui_store_text(gui->store, groups[i], "executable", "");
        gboolean pin = g_key_file_get_boolean(gui->store->keys, groups[i], "pin", NULL);
        char *title = g_strdup_printf("%s%s", name, pin ? " · Pinned" : "");
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title); g_free(title);
        char *subtitle = g_strdup_printf("%s · %s%s", device, exe, g_key_file_get_int64(gui->store->keys, groups[i], "recent", NULL) ? " · Recently launched" : "");
        adw_action_row_set_subtitle(row, subtitle); g_free(name); g_free(device); g_free(exe); g_free(subtitle);
        g_object_set_data_full(G_OBJECT(row), "group", g_strdup(groups[i]), g_free);
        const char *actions[] = {"run", "edit", "duplicate", "pin", "remove"};
        const char *labels[] = {"Launch", "Edit", "Duplicate", "Pin / unpin", "Remove launcher"};
        const char *icons[] = {"media-playback-start-symbolic", "document-edit-symbolic", "edit-copy-symbolic", "starred-symbolic", "user-trash-symbolic"};
        for (guint j = 0; j < G_N_ELEMENTS(actions); ++j) {
            GtkWidget *button = gui_button(j == 0 ? labels[j] : "", icons[j], NULL, j == 0);
            gtk_widget_set_tooltip_text(button, labels[j]);
            gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, labels[j], -1);
            g_object_set_data_full(G_OBJECT(button), "group", g_strdup(groups[i]), g_free);
            g_object_set_data(G_OBJECT(button), "library_action", (gpointer)actions[j]);
            g_signal_connect(button, "clicked", G_CALLBACK(library_button), gui); adw_action_row_add_suffix(row, button);
        }
        gtk_list_box_append(gui->launchers, GTK_WIDGET(row));
    }
    g_strfreev(groups);
    if (!found) { GtkWidget *empty = gtk_label_new("No saved launchers yet. Add a Windows application or a command you use often."); gtk_label_set_wrap(GTK_LABEL(empty), TRUE); gtk_list_box_append(gui->launchers, empty); }
}
GtkWidget *gui_library_page(gui_t *gui) {
    GtkWidget *box; GtkWidget *page = gui_page(&box, "Applications", "Your saved Windows applications and commands. Pinned launchers come first, followed by recent launches.");
    gtk_box_append(GTK_BOX(box), gui_button("New launcher…", "list-add-symbolic", "launcher-new", TRUE));
    gui->launchers = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(gui->launchers, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(gui->launchers), "boxed-list"); gtk_list_box_set_sort_func(gui->launchers, library_sort, gui, NULL);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->launchers)); gui_library_refresh(gui); return page;
}
/** @brief Palette captures explicit device; child rows own action strings. */
typedef struct palette_t { gui_t *gui; GtkWindow *window; GtkSearchEntry *search; GtkListBox *list; char *device; } palette_t;
static void palette_free(gpointer context) { palette_t *palette = context; g_free(palette->device); g_free(palette); }
static gboolean palette_filter(GtkListBoxRow *row, gpointer context) {
    palette_t *palette = context; const char *name = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row));
    char *lower = g_utf8_casefold(name, -1); char *query = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(palette->search)), -1);
    gboolean match = strstr(lower, query) != NULL; g_free(lower); g_free(query); return match;
}
static void palette_search(GtkSearchEntry *entry, gpointer context) { (void)entry; gtk_list_box_invalidate_filter(((palette_t *)context)->list); }
static void palette_activate(GtkListBox *list, GtkListBoxRow *row, gpointer context) {
    (void)list; palette_t *palette = context; if (!row) return;
    char *action = g_strdup(g_object_get_data(G_OBJECT(row), "action"));
    gui_t *gui = palette->gui; char *device = g_strdup(palette->device);
    gtk_window_destroy(palette->window);
    if (device) {
        if (!gui_device_data(gui, device)) { gui_message(gui, "The palette's device is no longer available. Open it again after selecting a device."); g_free(device); g_free(action); return; }
        gui_select(gui, device);
    }
    gui_dispatch(gui, action); g_free(device); g_free(action);
}
static void palette_enter(GtkSearchEntry *entry, gpointer context) {
    (void)entry; palette_t *palette = context;
    for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(palette->list)); row; row = gtk_widget_get_next_sibling(row))
        if (gtk_widget_get_child_visible(row)) { palette_activate(palette->list, GTK_LIST_BOX_ROW(row), palette); break; }
}
static gboolean palette_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType modifiers, gpointer context) {
    (void)controller; (void)code; (void)modifiers;
    if (key != GDK_KEY_Escape) return FALSE;
    gtk_window_destroy(((palette_t *)context)->window); return TRUE;
}
void gui_palette_open(gui_t *gui) {
    palette_t *palette = g_new0(palette_t, 1); palette->gui = gui; palette->device = g_strdup(gui->selected);
    palette->window = GTK_WINDOW(adw_window_new()); gtk_window_set_transient_for(palette->window, gui->window);
    gtk_window_set_destroy_with_parent(palette->window, TRUE); gtk_window_set_modal(palette->window, TRUE);
    gtk_window_set_title(palette->window, "Command palette"); gtk_window_set_default_size(palette->window, 540, 560);
    GtkWidget *box; char *scope = g_strdup_printf("Device actions target: %s", palette->device ? palette->device : "none — select a device first");
    GtkWidget *page = gui_page(&box, "Commands", scope); g_free(scope);
    palette->search = GTK_SEARCH_ENTRY(gtk_search_entry_new()); gtk_box_append(GTK_BOX(box), GTK_WIDGET(palette->search));
    palette->list = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_filter_func(palette->list, palette_filter, palette, NULL);
    gtk_widget_add_css_class(GTK_WIDGET(palette->list), "boxed-list"); gtk_box_append(GTK_BOX(box), GTK_WIDGET(palette->list));
    const char *actions[] = {"devices", "applications", "activity", "diagnostics", "preferences", "create", "import", "refresh", "start", "stop", "restart", "reboot", "pause", "resume", "desktop", "terminal", "logs", "doctor", "config-reload", "clone", "export", "advanced"};
    const char *titles[] = {"Go to Devices", "Go to Applications", "Go to Activity", "Go to Diagnostics", "Go to Settings", "Create device", "Import backup", "Refresh states", "Start device", "Shut down device", "Restart subsystem", "Reboot Windows", "Pause device · backend unavailable", "Resume device · backend unavailable", "View Full Desktop · backend unavailable", "Open terminal", "View logs", "Check device health", "Reload configuration", "Clone device", "Export backup", "Advanced CLI"};
    for (guint i = 0; i < G_N_ELEMENTS(actions); ++i) {
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), titles[i]);
        g_object_set_data_full(G_OBJECT(row), "action", g_strdup(actions[i]), g_free); gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        gtk_list_box_append(palette->list, GTK_WIDGET(row));
    }
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(palette_key), palette);
    gtk_widget_add_controller(GTK_WIDGET(palette->window), keys);
    g_signal_connect(palette->search, "search-changed", G_CALLBACK(palette_search), palette);
    g_signal_connect(palette->search, "activate", G_CALLBACK(palette_enter), palette);
    g_signal_connect(palette->list, "row-activated", G_CALLBACK(palette_activate), palette);
    adw_window_set_content(ADW_WINDOW(palette->window), page); g_object_set_data_full(G_OBJECT(palette->window), "palette", palette, palette_free);
    gtk_window_present(palette->window); gtk_widget_grab_focus(GTK_WIDGET(palette->search));
}
/** @brief Export owns snapshot and parent/application refs during async file IO. */
typedef struct export_t { char *text; GtkWindow *parent; GApplication *application; } export_t;
static void export_free(export_t *export) {
    g_free(export->text); g_object_unref(export->parent); g_object_unref(export->application); g_free(export);
}
static void export_written(GObject *source, GAsyncResult *result, gpointer context) {
    export_t *export = context; GError *error = NULL;
    if (!g_file_replace_contents_finish(G_FILE(source), result, NULL, &error)) {
        if (gtk_widget_get_visible(GTK_WIDGET(export->parent))) {
#if ADW_CHECK_VERSION(1, 5, 0)
            AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Could not save output", error->message));
            adw_alert_dialog_add_response(dialog, "close", "Close"); adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(export->parent));
#else
            GtkWidget *dialog = adw_message_dialog_new(export->parent, "Could not save output", error->message);
            gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
            adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "close", "Close"); gtk_window_present(GTK_WINDOW(dialog));
#endif
        }
        g_clear_error(&error);
    }
    g_application_release(export->application); export_free(export);
}
static void export_done(GtkNativeDialog *dialog, int response, gpointer context) {
    export_t *export = context;
    GFile *file = response == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog)) : NULL;
    gtk_native_dialog_destroy(dialog); g_object_unref(dialog);
    if (file) {
        g_application_hold(export->application);
        g_file_replace_contents_async(file, export->text, strlen(export->text), NULL, FALSE,
            G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, export_written, export);
        g_object_unref(file);
    } else export_free(export);
}
void gui_output_export(gui_t *gui, GtkTextView *view, gboolean save) {
    GtkTextIter start, end; GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    gtk_text_buffer_get_bounds(buffer, &start, &end); char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    if (!save) { gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(gui->window)), text); g_free(text); return; }
    GtkFileChooserNative *picker = gtk_file_chooser_native_new("Save reviewed output", gui->window, GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(picker), "waddle-diagnostics.txt");
    export_t *export = g_new0(export_t, 1); export->text = text;
    export->parent = g_object_ref(gui->window); export->application = g_object_ref(G_APPLICATION(gui->application));
    g_signal_connect(picker, "response", G_CALLBACK(export_done), export); gtk_native_dialog_show(GTK_NATIVE_DIALOG(picker));
}
