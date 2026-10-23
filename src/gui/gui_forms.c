/** @file gui_forms.c @brief Native reviewed forms and actions using existing CLI argv. */
#include "gui_private.h"
#include <string.h>

/** @brief Form owns captured identity and action; parent owns all widget pointers. */
typedef struct form_t {
    gui_t *gui;
    GtkWindow *window;
    char *device, *action;
    AdwEntryRow *entries[16];
    GtkDropDown *choice;
    GtkCheckButton *checks[5];
    GtkStack *steps;
    GtkLabel *review, *error;
    GtkWidget *next, *back;
    guint step;
} form_t;
static void form_free(gpointer context) {
    form_t *form = context;
    g_free(form->device); g_free(form->action); g_free(form);
}
static const char *field(form_t *form, guint index) { return gtk_editable_get_text(GTK_EDITABLE(form->entries[index])); }
static AdwEntryRow *form_entry(form_t *form, GtkWidget *box, guint index, const char *title, const char *value) {
    AdwEntryRow *entry = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), title);
    if (value) gtk_editable_set_text(GTK_EDITABLE(entry), value);
    GtkWidget *list = gtk_list_box_new(); gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list"); gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(entry));
    gtk_box_append(GTK_BOX(box), list); form->entries[index] = entry;
    return entry;
}
static GtkCheckButton *form_check(form_t *form, GtkWidget *box, guint index, const char *title, gboolean active) {
    GtkCheckButton *check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(title));
    gtk_check_button_set_active(check, active); gtk_box_append(GTK_BOX(box), GTK_WIDGET(check));
    form->checks[index] = check; return check;
}
static gboolean checked(form_t *form, guint index) { return form->checks[index] && gtk_check_button_get_active(form->checks[index]); }
static void form_close(GtkButton *button, gpointer context) {
    (void)button; form_t *form = context; gtk_window_destroy(form->window);
}
/** @brief Picker context owns entry reference; independent of closed form lifetime. */
typedef struct picker_t { GtkEditable *entry; } picker_t;
static void picker_done(GtkNativeDialog *dialog, int response, gpointer context) {
    picker_t *picker = context;
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        char *path = file ? g_file_get_path(file) : NULL;
        if (path) gtk_editable_set_text(picker->entry, path);
        g_free(path); g_clear_object(&file);
    }
    gtk_native_dialog_destroy(dialog); g_object_unref(dialog);
    g_object_unref(picker->entry); g_free(picker);
}
static void pick_path(GtkButton *button, gpointer context) {
    form_t *form = context;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "index"));
    gboolean folder = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "folder"));
    GtkFileChooserNative *picker = gtk_file_chooser_native_new(folder ? "Choose directory" : "Choose file", form->window,
        folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN, "Select", "Cancel");
    picker_t *state = g_new0(picker_t, 1); state->entry = g_object_ref(GTK_EDITABLE(form->entries[index]));
    g_signal_connect(picker, "response", G_CALLBACK(picker_done), state);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(picker));
}
static void form_picker(form_t *form, guint index, gboolean folder) {
    GtkWidget *button = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_tooltip_text(button, folder ? "Choose directory" : "Choose file");
    g_object_set_data(G_OBJECT(button), "index", GUINT_TO_POINTER(index));
    g_object_set_data(G_OBJECT(button), "folder", GINT_TO_POINTER(folder));
    g_signal_connect(button, "clicked", G_CALLBACK(pick_path), form);
    adw_entry_row_add_suffix(form->entries[index], button);
}
static gboolean form_error(form_t *form, const char *message) { gtk_label_set_text(form->error, message); return FALSE; }
static gboolean valid_name(form_t *form, guint index) {
    return gui_name_valid(field(form, index)) || form_error(form, "Use 1–63 ASCII letters, digits, underscores or hyphens for the device name.");
}
static gboolean valid_path(form_t *form, guint index, gboolean directory) {
    const char *path = field(form, index);
    if (!g_path_is_absolute(path)) return form_error(form, "Choose an absolute local path.");
    if (!g_file_test(path, directory ? G_FILE_TEST_IS_DIR : G_FILE_TEST_IS_REGULAR))
        return form_error(form, directory ? "The selected directory is unavailable." : "The selected source file is unavailable.");
    return TRUE;
}
static gboolean valid_uint(form_t *form, guint index, guint minimum, guint maximum) {
    const char *value = field(form, index);
    char *end = NULL;
    guint64 number = g_ascii_strtoull(value, &end, 10);
    if (!*value || value[0] < '0' || value[0] > '9' || !end || *end || number < minimum || number > maximum) {
        char *message = g_strdup_printf("Enter a whole number from %u to %u.", minimum, maximum);
        form_error(form, message); g_free(message); return FALSE;
    }
    return TRUE;
}
static GPtrArray *creation_args(form_t *form) {
    if (!valid_name(form, 0)) return NULL;
    guint source = gtk_drop_down_get_selected(form->choice);
    if (source == 0 && !valid_path(form, 1, FALSE)) return NULL;
    if (source == 2 && !valid_path(form, 1, TRUE)) return NULL;
    if (source == 1) {
        const char *size = field(form, 2); char *end = NULL;
        guint64 amount = g_ascii_strtoull(size, &end, 10);
        gboolean valid = *size >= '0' && *size <= '9' && amount > 0 && end &&
            ((*end == 'M' && amount <= 2097152) || (*end == 'G' && amount <= 2048)) && end[1] == '\0';
        if (!valid) { form_error(form, "Blank disk size must be a decimal M/G size between 1 MiB and 2 TiB."); return NULL; }
    }
    if (source != 2 && (!valid_uint(form, 3, 512, 65536) || !valid_uint(form, 4, 1, 128))) return NULL;
    GPtrArray *args = gui_args_new("device", source == 2 ? "import" : "init");
    gui_args_add(args, field(form, 0));
    if (source == 0 || source == 2) { gui_args_add(args, source == 2 ? "--input" : "--base-disk"); gui_args_add(args, field(form, 1)); }
    if (source == 1) { gui_args_add(args, "--blank-disk"); gui_args_add(args, field(form, 2)); }
    if (source != 2) {
        gui_args_add(args, "--memory-mb"); gui_args_add(args, field(form, 3));
        gui_args_add(args, "--vcpus"); gui_args_add(args, field(form, 4));
        gui_args_add(args, "--shell"); gui_args_add(args, field(form, 5));
    }
    gui_args_add(args, "--json");
    return args;
}
static void creation_back(GtkButton *button, gpointer context) {
    (void)button; form_t *form = context;
    if (form->step) --form->step;
    char *step = g_strdup_printf("%u", form->step); gtk_stack_set_visible_child_name(form->steps, step); g_free(step);
    gtk_button_set_label(GTK_BUTTON(form->next), "Continue");
    gtk_widget_set_sensitive(form->back, form->step > 0);
}
static void creation_submit(GtkButton *button, gpointer context) {
    (void)button; form_t *form = context;
    gtk_label_set_text(form->error, "");
    if (form->step == 0) {
        if (!valid_name(form, 0)) return;
        guint source = gtk_drop_down_get_selected(form->choice);
        if (source != 1 && !valid_path(form, 1, source == 2)) return;
        form->step = 1;
    } else if (form->step == 1) {
        GPtrArray *args = creation_args(form); if (!args) return;
        g_ptr_array_unref(args);
        char *destination = g_build_filename(g_get_user_state_dir(), "waddle", "devices", field(form, 0), NULL);
        char *review = g_strdup_printf("Device: %s\nSource: %s\nOwned state destination: %s\nMemory: %s MiB · CPUs: %s\nShell: %s\n\n%s\nImported devices retain their exported resources and disable host sharing.\nPreview runs the backend dry run without creating anything.",
            field(form, 0), gtk_drop_down_get_selected(form->choice) == 1 ? field(form, 2) : field(form, 1), destination,
            field(form, 3), field(form, 4), field(form, 5), gtk_drop_down_get_selected(form->choice) == 1 ? "A blank disk has no Windows installation." : "Base images remain separate from this device's owned overlay.");
        gtk_label_set_text(form->review, review); g_free(review); g_free(destination); form->step = 2;
    } else {
        GPtrArray *args = creation_args(form); if (!args) return;
        gboolean preview = checked(form, 1);
        if (preview) gui_args_add(args, "--dry-run");
        if (gui_run_args(form->gui, preview ? "Preview creation" : "Create device", field(form, 0), args)) {
            gtk_label_set_text(form->error, preview ? "Dry run submitted. Review Activity, then uncheck Preview to create." : "Creation submitted. Review Activity for the actual result. This wizard keeps your entries for recovery.");
            gtk_window_set_modal(form->window, FALSE);
            if (!preview && checked(form, 0)) {
                /* Start is a separate explicit follow-up, only after successful creation. */
                g_key_file_set_string(form->gui->store->keys, "Session", "start_after_create", field(form, 0));
            }
        }
        return;
    }
    char *step = g_strdup_printf("%u", form->step); gtk_stack_set_visible_child_name(form->steps, step); g_free(step);
    gtk_widget_set_sensitive(form->back, TRUE);
    gtk_button_set_label(GTK_BUTTON(form->next), form->step == 2 ? "Submit reviewed operation" : "Continue");
}
static void source_changed(GObject *object, GParamSpec *property, gpointer context) {
    (void)object; (void)property; form_t *form = context;
    guint source = gtk_drop_down_get_selected(form->choice);
    gtk_widget_set_sensitive(GTK_WIDGET(form->entries[1]), source != 1);
    gtk_widget_set_sensitive(GTK_WIDGET(form->entries[2]), source == 1);
    for (guint i = 3; i <= 5; ++i) gtk_widget_set_sensitive(GTK_WIDGET(form->entries[i]), source != 2);
}
static void creation_build(form_t *form, GtkWidget *box) {
    form->steps = GTK_STACK(gtk_stack_new()); gtk_box_append(GTK_BOX(box), GTK_WIDGET(form->steps));
    GtkWidget *source = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gui_label(source, "1 · Name and source", "title-3");
    form_entry(form, source, 0, "Device name", "");
    const char *sources[] = {"Existing Windows base disk", "Blank disk (no Windows installed)", "Exported Waddle device", NULL};
    form->choice = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sources)); gtk_box_append(GTK_BOX(source), GTK_WIDGET(form->choice));
    form_entry(form, source, 1, "Base image or exported backup directory", ""); form_picker(form, 1, FALSE);
    /* Separate folder picker makes both sources accessible without guessing chooser type. */
    GtkWidget *folder = gtk_button_new_with_label("Choose exported backup directory…");
    g_object_set_data(G_OBJECT(folder), "index", GUINT_TO_POINTER(1)); g_object_set_data(G_OBJECT(folder), "folder", GINT_TO_POINTER(1));
    g_signal_connect(folder, "clicked", G_CALLBACK(pick_path), form); gtk_box_append(GTK_BOX(source), folder);
    form_entry(form, source, 2, "Blank disk size (decimal M or G)", "64G");
    gui_label(source, "Blank disks do not include Windows. You need your own valid Windows installation or base image.", "dim-label");
    gtk_stack_add_named(form->steps, source, "0");
    GtkWidget *resources = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gui_label(resources, "2 · Resources", "title-3");
    form_entry(form, resources, 3, "Memory (512–65536 MiB)", "4096");
    form_entry(form, resources, 4, "Virtual CPUs (1–128)", "4");
    form_entry(form, resources, 5, "Default shell", "powershell.exe");
    gui_label(resources, "An imported backup retains its resource settings. You can edit them after import while the device is stopped.", "dim-label");
    gtk_stack_add_named(form->steps, resources, "1");
    GtkWidget *review = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gui_label(review, "3 · Review and create", "title-3");
    form->review = GTK_LABEL(gui_label(review, "", NULL));
    form_check(form, review, 1, "Preview only (backend dry run)", TRUE);
    form_check(form, review, 0, "Start the device after successful creation", FALSE);
    gtk_stack_add_named(form->steps, review, "2");
    g_signal_connect(form->choice, "notify::selected", G_CALLBACK(source_changed), form);
    source_changed(NULL, NULL, form);
}
static void append_option(GPtrArray *args, const char *flag, const char *value) {
    if (*value) { gui_args_add(args, flag); gui_args_add(args, value); }
}
static gboolean append_words(GPtrArray *args, const char *text, form_t *form) {
    if (!*text) return TRUE;
    int count = 0; char **words = NULL; GError *error = NULL;
    if (!g_shell_parse_argv(text, &count, &words, &error)) {
        form_error(form, error->message); g_clear_error(&error); return FALSE;
    }
    for (int i = 0; i < count; ++i) gui_args_add(args, words[i]);
    g_strfreev(words); return TRUE;
}
static void operation_submit(GtkButton *button, gpointer context) {
    (void)button; form_t *form = context; gui_t *gui = form->gui;
    gtk_label_set_text(form->error, "");
    GPtrArray *args = NULL;
    const char *action = form->action;
    if (!strcmp(action, "rename") || !strcmp(action, "clone")) {
        if (!valid_name(form, 0)) return;
        args = gui_args_new("device", action); gui_args_add(args, form->device); gui_args_add(args, field(form, 0));
    } else if (!strcmp(action, "export") || !strcmp(action, "import")) {
        if (!valid_path(form, 0, TRUE) || !valid_name(form, 1)) return;
        args = gui_args_new("device", action); gui_args_add(args, !strcmp(action, "import") ? field(form, 1) : form->device);
        gui_args_add(args, !strcmp(action, "import") ? "--input" : "--output");
        char *destination = !strcmp(action, "export") ? g_build_filename(field(form, 0), field(form, 1), NULL) : g_strdup(field(form, 0));
        if (!strcmp(action, "export") && g_file_test(destination, G_FILE_TEST_EXISTS)) {
            form_error(form, "The backup directory already exists. Choose a new name; exports never overwrite a backup.");
            g_free(destination); g_ptr_array_unref(args); return;
        }
        gui_args_add(args, destination); g_free(destination);
    } else if (!strcmp(action, "remove")) {
        if (strcmp(field(form, 0), form->device)) { form_error(form, "Type the exact device name before previewing or removing it."); return; }
        args = gui_args_new("device", "remove"); gui_args_add(args, form->device); gui_args_add(args, "--yes");
        if (checked(form, 0)) gui_args_add(args, "--keep-data");
    } else if (!strcmp(action, "av-setup")) {
        args = gui_args_new("av", "setup"); gui_args_add(args, "--device"); gui_args_add(args, form->device);
        if (checked(form, 0)) gui_args_add(args, "--kvmfr");
        if (checked(form, 1) && !*field(form, 0)) gui_args_add(args, "--uefi");
        append_option(args, "--firmware-vars", field(form, 0)); append_option(args, "--gpu", field(form, 1)); append_option(args, "--gpu-mdev", field(form, 2));
    } else if (!strcmp(action, "av-run")) {
        if (!valid_uint(form, 0, 1, G_MAXUINT32)) return;
        args = gui_args_new("av", "run"); gui_args_add(args, field(form, 0)); gui_args_add(args, "--device"); gui_args_add(args, form->device);
        if (checked(form, 0)) gui_args_add(args, "--latency");
    } else if (!strcmp(action, "exec")) {
        if (!*field(form, 0)) { form_error(form, "Enter an executable and its arguments."); return; }
        args = gui_args_new("exec", "--device"); gui_args_add(args, form->device);
        gui_args_add(args, checked(form, 1) ? "--interactive" : "--pipe");
        if (checked(form, 0)) gui_args_add(args, "--translate-path");
        append_option(args, "--cwd", field(form, 1)); append_option(args, "--env", field(form, 2));
        append_option(args, "--path-map", field(form, 3)); append_option(args, "--timeout", field(form, 4));
        gui_args_add(args, "--");
        if (!append_words(args, field(form, 0), form)) { g_ptr_array_unref(args); return; }
        if (checked(form, 1)) {
            g_ptr_array_add(args, NULL); gui_terminal_external(gui, form->device, (const char *const *)args->pdata);
            g_ptr_array_unref(args); return;
        }
    } else if (!strcmp(action, "advanced")) {
        args = gui_args_new(NULL, NULL);
        if (!append_words(args, field(form, 0), form)) { g_ptr_array_unref(args); return; }
        if (!args->len) { form_error(form, "Enter an explicit Waddle command (without the word waddle)."); g_ptr_array_unref(args); return; }
        const char *first = args->pdata[0];
        if (!strcmp(first, "gui") || !strcmp(first, "repl") || !strcmp(first, "shell") || !strcmp(first, "waddle")) {
            form_error(form, "Use a management command here; use Terminal for interactive sessions."); g_ptr_array_unref(args); return;
        }
    }
    if (!args) return;
    gboolean dry_supported = !strcmp(action, "rename") || !strcmp(action, "clone") || !strcmp(action, "remove") || !strcmp(action, "export") || !strcmp(action, "import");
    if (dry_supported) { gui_args_add(args, "--json"); if (checked(form, 3)) gui_args_add(args, "--dry-run"); }
    if (gui_run_args(gui, action, form->device, args)) {
        gtk_label_set_text(form->error, "Submitted. Activity shows live output and the actual exit status. Your entries remain available for recovery.");
        gtk_window_set_modal(form->window, FALSE);
    }
}
static void operation_build(form_t *form, GtkWidget *box) {
    const char *action = form->action;
    gboolean storage = FALSE;
    if (!strcmp(action, "rename") || !strcmp(action, "clone")) {
        form_entry(form, box, 0, "New device name", ""); storage = TRUE;
    } else if (!strcmp(action, "export") || !strcmp(action, "import")) {
        form_entry(form, box, 0, !strcmp(action, "export") ? "Parent directory for backup" : "Backup directory", ""); form_picker(form, 0, TRUE);
        form_entry(form, box, 1, !strcmp(action, "export") ? "New backup folder name (letters, digits, - or _)" : "New device name", !strcmp(action, "export") ? "backup" : "");
        gui_label(box, "Imports disable host sharing. Exports are portable backups of a stopped device. The backend validates directory ownership and contents.", "dim-label"); storage = TRUE;
    } else if (!strcmp(action, "remove")) {
        JsonObject *data = gui_device_data(form->gui, form->device);
        char *warning = g_strdup_printf("Remove %s?\nOwned disk: %s\nState: %s\n\nHost folders shared into Windows are not device-owned and are never removed. Preview the affected resources first. Activity reports the exact retained-data location after removal; it may differ from the current state path above.",
            form->device, gui_json_text(data, "disk_image", "unavailable"), gui_json_text(data, "state_dir", "unavailable"));
        gui_label(box, warning, NULL); g_free(warning);
        form_entry(form, box, 0, "Type the device name to confirm", "");
        form_check(form, box, 0, "Retain owned disk and data (--keep-data)", TRUE); storage = TRUE;
    } else if (!strcmp(action, "av-setup")) {
        gui_label(box, "Setup requires a stopped device and the existing bundled AV components. It may provision a signed driver and restart Windows. KVMFR provisioning can require host privileges.", NULL);
        form_check(form, box, 0, "Provision KVMFR", FALSE); form_check(form, box, 1, "Use UEFI firmware", FALSE);
        form_entry(form, box, 0, "Existing firmware variables image (optional)", ""); form_picker(form, 0, FALSE);
        form_entry(form, box, 1, "GPU parent PCI address (optional)", "");
        form_entry(form, box, 2, "Existing mediated device UUID (optional)", "");
        gui_label(box, "Use an administrator-created mediated device. Software GPU contexts do not reserve dedicated hardware or VRAM.", "dim-label");
    } else if (!strcmp(action, "av-run")) {
        form_entry(form, box, 0, "Guest process ID", "");
        form_check(form, box, 0, "Run latency fixture diagnostic (--latency)", FALSE);
        gui_label(box, "The existing native AV viewer presents windows from this process. Closing the view or stopping this operation leaves Windows running. Full desktop capture is not available in the current managed AV API.", "dim-label");
    } else if (!strcmp(action, "exec")) {
        form_entry(form, box, 0, "Executable and arguments (quoted argv)", "");
        form_entry(form, box, 1, "Guest working directory (optional)", "");
        form_entry(form, box, 2, "Environment KEY=VALUE (optional)", "");
        form_entry(form, box, 3, "Path mapping SRC=DEST (optional)", "");
        form_entry(form, box, 4, "Timeout in seconds (0 disables)", "0");
        form_check(form, box, 0, "Translate Linux paths", TRUE);
        form_check(form, box, 1, "Run interactively in an external terminal", FALSE);
        gui_label(box, "Arguments use shell-style quoting for splitting only. No local shell runs. Use saved launchers or Advanced CLI for repeatable environment/path options.", "dim-label");
    } else if (!strcmp(action, "advanced")) {
        form_entry(form, box, 0, "Explicit CLI command and arguments (without waddle)", "device --help");
        gui_label(box, "All existing CLI capabilities are available here. Include the exact device name or --device NAME. Quoting splits argv only; pipes, redirections and command substitutions are not executed. Interactive sessions belong in Terminal.", NULL);
    }
    if (storage) {
        gui_label(box, "The device must be stopped. Preview validates the exact operation using the backend dry run; review Activity before unchecking Preview and applying it.", "dim-label");
        form_check(form, box, 3, "Preview only (--dry-run)", TRUE);
    }
}
void gui_form_open(gui_t *gui, const char *action) {
    form_t *form = g_new0(form_t, 1);
    form->gui = gui; form->action = g_strdup(action); form->device = g_strdup(gui->selected);
    form->window = GTK_WINDOW(adw_window_new());
    gtk_window_set_application(form->window, GTK_APPLICATION(gui->application));
    gtk_window_set_transient_for(form->window, gui->window); gtk_window_set_destroy_with_parent(form->window, TRUE);
    gtk_window_set_modal(form->window, TRUE); gtk_window_set_default_size(form->window, 660, 650);
    char *title = g_strdup_printf("%s%s%s", !strcmp(action, "create") ? "Create device" : action, form->device ? " · " : "", form->device ? form->device : "");
    gtk_window_set_title(form->window, title); g_free(title);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_box_append(GTK_BOX(root), adw_header_bar_new());
    GtkWidget *box;
    GtkWidget *page = gui_page(&box, !strcmp(action, "create") ? "Create a Windows device" : "Review operation", form->device ? form->device : "No device is selected");
    gtk_widget_set_vexpand(page, TRUE); gtk_box_append(GTK_BOX(root), page);
    if (!strcmp(action, "create")) creation_build(form, box); else operation_build(form, box);
    form->error = GTK_LABEL(gui_label(box, "", "warning"));
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *cancel = gtk_button_new_with_label("Close"); g_signal_connect(cancel, "clicked", G_CALLBACK(form_close), form);
    gtk_box_append(GTK_BOX(buttons), cancel);
    if (!strcmp(action, "create")) {
        form->back = gtk_button_new_with_label("Back"); gtk_widget_set_sensitive(form->back, FALSE);
        g_signal_connect(form->back, "clicked", G_CALLBACK(creation_back), form); gtk_box_append(GTK_BOX(buttons), form->back);
    }
    form->next = gtk_button_new_with_label(!strcmp(action, "create") ? "Continue" : "Submit reviewed operation");
    gtk_widget_add_css_class(form->next, !strcmp(action, "remove") ? "destructive-action" : "suggested-action");
    g_signal_connect(form->next, "clicked", !strcmp(action, "create") ? G_CALLBACK(creation_submit) : G_CALLBACK(operation_submit), form);
    gtk_box_append(GTK_BOX(buttons), form->next); gtk_box_append(GTK_BOX(box), buttons);
    adw_window_set_content(ADW_WINDOW(form->window), root);
    g_object_set_data_full(G_OBJECT(form->window), "form", form, form_free);
    gtk_window_present(form->window);
}
/** @brief Confirmation context owns argv and captured identity until dialog response. */
typedef struct confirmation_t { gui_t *gui; char *device, *label; char **argv; } confirmation_t;
static void confirmation_free(gpointer context, GClosure *closure) {
    (void)closure; confirmation_t *confirmation = context;
    g_free(confirmation->device); g_free(confirmation->label); g_strfreev(confirmation->argv); g_free(confirmation);
}
static void confirmation_response(gpointer dialog, const char *response, gpointer context) {
    (void)dialog; confirmation_t *confirmation = context;
    if (!strcmp(response, "apply")) gui_run(confirmation->gui, confirmation->label, confirmation->device, (const char *const *)confirmation->argv);
}
void gui_confirm(gui_t *gui, const char *title, const char *description, const char *label, const char *device, const char *const *arguments) {
    confirmation_t *confirmation = g_new0(confirmation_t, 1);
    confirmation->gui = gui; confirmation->device = g_strdup(device); confirmation->label = g_strdup(label); confirmation->argv = g_strdupv((char **)arguments);
#if ADW_CHECK_VERSION(1, 5, 0)
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, description));
    adw_alert_dialog_add_responses(dialog, "cancel", "Cancel", "apply", "Apply", NULL);
    adw_alert_dialog_set_response_appearance(dialog, "apply", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dialog, "cancel"); adw_alert_dialog_set_close_response(dialog, "cancel");
    g_signal_connect_data(dialog, "response", G_CALLBACK(confirmation_response), confirmation, confirmation_free, 0);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(gui->window));
#else
    GtkWidget *dialog = adw_message_dialog_new(gui->window, title, description);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel", "apply", "Apply", NULL);
    adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "apply", ADW_RESPONSE_DESTRUCTIVE);
    adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
    adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
    g_signal_connect_data(dialog, "response", G_CALLBACK(confirmation_response), confirmation, confirmation_free, 0);
    gtk_window_present(GTK_WINDOW(dialog));
#endif
}
static void unavailable(gui_t *gui, const char *title, const char *reason) {
#if ADW_CHECK_VERSION(1, 5, 0)
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, reason));
    adw_alert_dialog_add_response(dialog, "close", "Close");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(gui->window));
#else
    GtkWidget *dialog = adw_message_dialog_new(gui->window, title, reason);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "close", "Close"); gtk_window_present(GTK_WINDOW(dialog));
#endif
}
static gboolean require_device(gui_t *gui) {
    if (!gui->selected || !gui_device_data(gui, gui->selected)) { gui_message(gui, "Select a device first. No operation has been started."); return FALSE; }
    return TRUE;
}
static void config_apply(gui_t *gui) {
    GPtrArray *args = gui_args_new("device", "config"); gui_args_add(args, "set"); gui_args_add(args, gui->selected);
    GString *review = g_string_new("Apply these settings together? The device must be stopped.\n\n");
    for (guint i = 0; i < GuiSettingCount; ++i) {
        const char *value = gtk_editable_get_text(GTK_EDITABLE(gui->settings[i]));
        if (g_strcmp0(value, gui->originals[i]) == 0) continue;
        char *assignment = g_strdup_printf("%s=%s", gui_setting_keys[i], value);
        gui_args_add(args, assignment); g_string_append_printf(review, "%s\n", assignment); g_free(assignment);
    }
    if (args->len == 4) { gui_message(gui, "No settings have changed."); }
    else {
        gui_args_add(args, "--json"); g_ptr_array_add(args, NULL);
        gui_confirm(gui, "Apply device configuration?", review->str, "Apply configuration", gui->selected, (const char *const *)args->pdata);
    }
    g_string_free(review, TRUE); g_ptr_array_unref(args);
}
static void read_logs(gui_t *gui) {
    const char *targets[] = {"all", "daemon", "qemu", "virtiofsd"};
    const char *lines = gtk_editable_get_text(GTK_EDITABLE(gui->log_lines));
    char *end = NULL; guint64 count = g_ascii_strtoull(lines, &end, 10);
    if (!*lines || !end || *end || count < 1 || count > 10000) { gui_message(gui, "Log line count must be 1–10000."); return; }
    GPtrArray *args = gui_args_new("logs", gui->selected);
    gui_args_add(args, "--target"); gui_args_add(args, targets[MIN(gtk_drop_down_get_selected(gui->log_target), 3)]);
    gui_args_add(args, "--lines"); gui_args_add(args, lines);
    gui_args_add(args, gtk_check_button_get_active(gui->log_follow) ? "--follow" : "--json");
    gui_run_args(gui, "Read logs", gui->selected, args);
}
/** @brief Batch form owns checkbox names; one existing CLI call at a time. */
typedef struct batch_t { gui_t *gui; GtkWindow *window; GtkDropDown *operation; GPtrArray *checks; } batch_t;
static void batch_free(gpointer context) { batch_t *batch = context; g_ptr_array_unref(batch->checks); g_free(batch); }
static void batch_submit(GtkButton *button, gpointer context) {
    (void)button; batch_t *batch = context;
    const char *operations[] = {"start", "stop", "restart", "status"};
    /* Backend supports --all, but explicit selection is expanded into a queued list.
     * The queue runs serially and reports each result independently. */
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < batch->checks->len; ++i) {
        GtkCheckButton *check = batch->checks->pdata[i];
        if (gtk_check_button_get_active(check)) g_ptr_array_add(names, g_strdup(gtk_check_button_get_label(check)));
    }
    if (!names->len) { gui_message(batch->gui, "Select at least one named device."); g_ptr_array_unref(names); return; }
    if (batch->gui->job) { gui_message(batch->gui, "Wait for the active operation before submitting a batch."); g_ptr_array_unref(names); return; }
    const char *operation = operations[MIN(gtk_drop_down_get_selected(batch->operation), 3)];
    g_key_file_set_string_list(batch->gui->store->keys, "Session", "batch_names", (const char *const *)names->pdata, names->len);
    g_key_file_set_string(batch->gui->store->keys, "Session", "batch_operation", operation);
    g_ptr_array_unref(names);
    gui_dispatch(batch->gui, "batch-next"); gtk_window_destroy(batch->window);
}
static void batch_open(gui_t *gui) {
    batch_t *batch = g_new0(batch_t, 1); batch->gui = gui; batch->checks = g_ptr_array_new();
    batch->window = GTK_WINDOW(adw_window_new()); gtk_window_set_transient_for(batch->window, gui->window);
    gtk_window_set_destroy_with_parent(batch->window, TRUE); gtk_window_set_modal(batch->window, TRUE);
    gtk_window_set_title(batch->window, "Explicit multi-device operation"); gtk_window_set_default_size(batch->window, 500, 500);
    GtkWidget *box; GtkWidget *page = gui_page(&box, "Select devices", "Each operation runs serially against the checked names. Failures are recorded per device and the batch continues.");
    const char *operations[] = {"Start", "Graceful shutdown", "Restart subsystem", "Inspect status", NULL};
    batch->operation = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(operations)); gtk_box_append(GTK_BOX(box), GTK_WIDGET(batch->operation));
    if (gui->registry) {
        JsonArray *array = json_object_get_array_member(json_node_get_object(json_parser_get_root(gui->registry)), "results");
        for (guint i = 0; i < json_array_get_length(array); ++i) {
            GtkWidget *check = gtk_check_button_new_with_label(gui_json_text(json_array_get_object_element(array, i), "name", ""));
            gtk_box_append(GTK_BOX(box), check); g_ptr_array_add(batch->checks, check);
        }
    }
    GtkWidget *submit = gtk_button_new_with_label("Run selected operation"); gtk_widget_add_css_class(submit, "suggested-action");
    g_signal_connect(submit, "clicked", G_CALLBACK(batch_submit), batch); gtk_box_append(GTK_BOX(box), submit);
    adw_window_set_content(ADW_WINDOW(batch->window), page);
    g_object_set_data_full(G_OBJECT(batch->window), "batch", batch, batch_free); gtk_window_present(batch->window);
}
static void batch_next(gui_t *gui) {
    if (gui->job || gui->closing) return;
    gsize count = 0;
    char **names = g_key_file_get_string_list(gui->store->keys, "Session", "batch_names", &count, NULL);
    if (names && count) {
        char *operation = gui_store_text(gui->store, "Session", "batch_operation", "status");
        g_key_file_set_string_list(gui->store->keys, "Session", "batch_names", (const char *const *)(names + 1), count - 1);
        const char *args[] = {operation, names[0], "--json", NULL};
        if (!gui_run(gui, operation, names[0], args)) {
            g_key_file_remove_group(gui->store->keys, "Session", NULL);
            gui_message(gui, "Batch could not start. Remaining device operations were not submitted.");
        }
        g_free(operation);
    }
    g_strfreev(names);
}
void gui_dispatch(gui_t *gui, const char *action) {
    if (gui->closing) return;
    if (!strcmp(action, "palette")) { gui_palette_open(gui); return; }
    if (!strcmp(action, "refresh")) { gui_refresh(gui); return; }
    const char *pages[] = {"devices", "applications", "activity", "diagnostics", "preferences"};
    for (guint i = 0; i < G_N_ELEMENTS(pages); ++i) if (!strcmp(action, pages[i])) { gtk_stack_set_visible_child_name(gui->pages, pages[i]); return; }
    if (!strcmp(action, "create") || !strcmp(action, "import") || !strcmp(action, "advanced")) { gui_form_open(gui, action); return; }
    if (!strcmp(action, "batch")) { batch_open(gui); return; }
    if (!strcmp(action, "batch-next")) { batch_next(gui); return; }
    if (!strcmp(action, "cancel")) {
        g_key_file_remove_group(gui->store->keys, "Session", NULL); gui_job_stop(gui->job); return;
    }
    if (!strcmp(action, "logs-stop")) { if (gui->stream_logs) gui_job_stop(gui->job); return; }
    if (!strcmp(action, "output-copy") || !strcmp(action, "output-save") || !strcmp(action, "logs-copy") || !strcmp(action, "logs-save")) {
        gui_output_export(gui, g_str_has_prefix(action, "logs-") ? gui->logs : gui->output, g_str_has_suffix(action, "save")); return;
    }
    if (!strcmp(action, "retry")) {
        if (gui->last_argv) gui_confirm(gui, "Retry previous operation?", "Review the previous result and equivalent CLI command in Activity. This repeats the exact arguments against the original device.", gui->operation, gui->operation_device, (const char *const *)gui->last_argv);
        return;
    }
    if (!strcmp(action, "command-copy")) {
        if (!gui->last_argv) return;
        GString *command = g_string_new("waddle");
        for (guint i = 0; gui->last_argv[i]; ++i) { char *quoted = g_shell_quote(gui->last_argv[i]); g_string_append_printf(command, " %s", quoted); g_free(quoted); }
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(gui->window)), command->str); g_string_free(command, TRUE); return;
    }
    if (!strcmp(action, "version")) { const char *args[] = {"--version", NULL}; gui_run(gui, "Version", NULL, args); return; }
    if (!strcmp(action, "doctor-all")) { const char *args[] = {"device", "doctor", "--all", "--json", NULL}; gui_run(gui, "Doctor all devices", NULL, args); return; }
    if (!strcmp(action, "launcher-new")) { gui_launcher_edit(gui, NULL); return; }
    if (!strcmp(action, "default-clear")) {
        const char *args[] = {"device", "default", "--clear", "--json", NULL}; gui_run(gui, "Clear default device", NULL, args); return;
    }
    if (!require_device(gui)) return;
    if (!strcmp(action, "terminal")) { gui_terminal_open(gui); return; }
    if (!strcmp(action, "terminal-external")) { gui_terminal_external(gui, gui->selected, NULL); return; }
    if (!strcmp(action, "desktop") || !strcmp(action, "pause") || !strcmp(action, "resume")) {
        unavailable(gui, !strcmp(action, "desktop") ? "Desktop unavailable" : "Pause / resume unavailable",
            !strcmp(action, "desktop") ? "This Waddle version exposes only per-process managed AV capture. A full desktop target and its input lease must be added to the existing AV backend before this control can connect. Use Graphics & Display → Attach to process for seamless application viewing. No device state has changed." :
            "The current daemon protocol has no pause/resume execution operation. Disconnecting a viewer is not a pause. This control requires a supported backend lifecycle interface; no device state has changed."); return;
    }
    if (!strcmp(action, "logs")) { gtk_stack_set_visible_child_name(gui->pages, "devices"); gtk_stack_set_visible_child_name(gui->details, "logs"); return; }
    if (!strcmp(action, "logs-read")) { read_logs(gui); return; }
    if (!strcmp(action, "folders")) { gui_load_details(gui); return; }
    if (!strcmp(action, "config-reload")) { g_clear_pointer(&gui->config_device, g_free); gui_load_details(gui); return; }
    if (!strcmp(action, "config-apply")) { config_apply(gui); return; }
    if (!strcmp(action, "pin")) {
        gboolean pinned = g_key_file_get_boolean(gui->store->keys, "PinnedDevices", gui->selected, NULL);
        g_key_file_set_boolean(gui->store->keys, "PinnedDevices", gui->selected, !pinned); gui_store_flush(gui); gui_refresh(gui); return;
    }
    const char *forms[] = {"rename", "clone", "export", "remove", "exec", "av-setup", "av-run"};
    for (guint i = 0; i < G_N_ELEMENTS(forms); ++i) if (!strcmp(action, forms[i])) { gui_form_open(gui, action); return; }
    if (!strcmp(action, "reboot")) {
        const char *args[] = {"exec", "--device", gui->selected, "--pipe", "--", "shutdown.exe", "/r", "/t", "0", NULL};
        char *description = g_strdup_printf("Reboot Windows on %s now? Save work first. This sends shutdown.exe /r /t 0 through the existing guest execution path. A disconnected guest may report a session error after accepting the reboot; inspect readiness before retrying.", gui->selected);
        gui_confirm(gui, "Reboot Windows?", description, "Reboot Windows", gui->selected, args); g_free(description); return;
    }
    if (!strcmp(action, "kill")) {
        const char *args[] = {"kill", gui->selected, "--json", NULL};
        char *description = g_strdup_printf("Force stop %s? Unsaved Windows work can be lost. This terminates subsystem processes; it does not remove the device.", gui->selected);
        gui_confirm(gui, "Force stop device?", description, "Force stop", gui->selected, args); g_free(description); return;
    }
    if (!strcmp(action, "default") || !strcmp(action, "default-clear")) {
        const char *args[] = {"device", "default", !strcmp(action, "default") ? gui->selected : "--clear", "--json", NULL}; gui_run(gui, "Change default device", gui->selected, args); return;
    }
    if (!strcmp(action, "inspect")) { const char *args[] = {"device", "show", gui->selected, "--json", NULL}; gui_run(gui, "Inspect profile", gui->selected, args); return; }
    if (!strcmp(action, "doctor") || !strcmp(action, "repair-preview") || !strcmp(action, "repair")) {
        GPtrArray *args = gui_args_new("device", "doctor"); gui_args_add(args, gui->selected); gui_args_add(args, "--json");
        if (strcmp(action, "doctor")) gui_args_add(args, "--repair");
        if (!strcmp(action, "repair-preview")) gui_args_add(args, "--dry-run");
        if (!strcmp(action, "repair")) {
            g_ptr_array_add(args, NULL); gui_confirm(gui, "Apply supported repairs?", "Run Preview repairs first and review the exact findings in Activity. The backend determines which recovery actions are safe for this device.", "Repair device", gui->selected, (const char *const *)args->pdata); g_ptr_array_unref(args);
        } else gui_run_args(gui, action, gui->selected, args);
        return;
    }
    if (!strcmp(action, "fs-test")) { const char *args[] = {"fs", "test", gui->selected, "--json", NULL}; gui_run(gui, "Filesystem test", gui->selected, args); return; }
    if (!strcmp(action, "av-probe")) { const char *args[] = {"av", "probe", "--device", gui->selected, NULL}; gui_run(gui, "Probe AV", gui->selected, args); return; }
    const char *lifecycle[] = {"start", "stop", "restart", "status"};
    for (guint i = 0; i < G_N_ELEMENTS(lifecycle); ++i) if (!strcmp(action, lifecycle[i])) {
        const char *args[] = {action, gui->selected, "--json", NULL}; gui_run(gui, action, gui->selected, args); return;
    }
}
