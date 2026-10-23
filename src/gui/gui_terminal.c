/** @file gui_terminal.c @brief GTK4 VTE sessions wrapping the existing repl implementation. */
#include "gui_private.h"
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
/* PCRE2 requires this width selector before its system declarations. */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

/** @brief Session owned by gui terminal array until application shutdown.
 * Terminal and tab are explicitly referenced until session_free; VTE owns PTY/reap. */
typedef struct terminal_session_t {
    gui_t *gui;
    VteTerminal *terminal;
    GtkLabel *tab, *status;
    GtkWidget *page;
    char *device;
    GPid child;
    gboolean spawning, held, closed;
    guint kill_timer;
} terminal_session_t;
static void terminal_settings(gui_t *gui, VteTerminal *terminal) {
    char *font = gui_store_text(gui->store, "Preferences", "terminal_font", "Monospace 11");
    PangoFontDescription *description = pango_font_description_from_string(font);
    vte_terminal_set_font(terminal, description); pango_font_description_free(description); g_free(font);
    vte_terminal_set_scrollback_lines(terminal, gui_store_number(gui->store, "terminal_scrollback", 10000, 100, 100000));
    vte_terminal_set_mouse_autohide(terminal, TRUE);
}
static void session_release(terminal_session_t *session) {
    if (session->held) { session->held = FALSE; g_application_release(G_APPLICATION(session->gui->application)); }
}
static void session_exited(VteTerminal *terminal, int status, gpointer context) {
    (void)terminal; terminal_session_t *session = context;
    session->child = 0;
    if (session->kill_timer) { g_source_remove(session->kill_timer); session->kill_timer = 0; }
    if (!session->gui->closing) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        char *message = g_strdup_printf("%s · Terminal exited (%d). Output is retained; Reconnect opens a new session.", session->device, code);
        gtk_label_set_text(session->status, message); g_free(message);
    }
    session_release(session);
}
static gboolean session_kill(gpointer context) {
    terminal_session_t *session = context;
    session->kill_timer = 0;
    if (session->child > 0) kill(session->child, SIGKILL);
    return G_SOURCE_REMOVE;
}
static void session_stop(terminal_session_t *session) {
    session->closed = TRUE;
    if (session->child > 0) {
        kill(session->child, SIGHUP);
        if (!session->kill_timer) session->kill_timer = g_timeout_add_seconds(3, session_kill, session);
    }
}
static void session_spawned(VteTerminal *terminal, GPid child, GError *error, gpointer context) {
    (void)terminal; terminal_session_t *session = context;
    session->spawning = FALSE;
    if (error) {
        if (!session->gui->closing) gtk_label_set_text(session->status, error->message);
        session_release(session); return;
    }
    session->child = child;
    if (session->gui->closing || session->closed) session_stop(session);
    else gtk_label_set_text(session->status, "Keyboard and pointer focus belong to this terminal when selected. Ctrl+Shift+C/V copy/paste. Closing the session leaves Windows running.");
}
static terminal_session_t *current_session(gui_t *gui) {
    GtkNotebook *notebook = g_object_get_data(G_OBJECT(gui->window), "terminal_notebook");
    if (!notebook) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(notebook, gtk_notebook_get_current_page(notebook));
    return page ? g_object_get_data(G_OBJECT(page), "session") : NULL;
}
static void session_free(gpointer context) {
    terminal_session_t *session = context;
    if (session->kill_timer) g_source_remove(session->kill_timer);
    g_signal_handlers_disconnect_by_data(session->terminal, session);
    g_object_unref(session->terminal); g_object_unref(session->tab); g_object_unref(session->status);
    g_free(session->device); g_free(session);
}
static void terminal_action(GtkButton *button, gpointer context) {
    gui_t *gui = context;
    const char *action = g_object_get_data(G_OBJECT(button), "terminal_action");
    if (!g_strcmp0(action, "new")) { gui_terminal_open(gui); return; }
    terminal_session_t *session = current_session(gui);
    if (!session) { gui_message(gui, "Open a terminal session first."); return; }
    if (!g_strcmp0(action, "copy")) vte_terminal_copy_clipboard_format(session->terminal, VTE_FORMAT_TEXT);
    else if (!g_strcmp0(action, "paste")) vte_terminal_paste_clipboard(session->terminal);
    else if (!g_strcmp0(action, "clear")) vte_terminal_reset(session->terminal, FALSE, TRUE);
    else if (!g_strcmp0(action, "zoom-in")) vte_terminal_set_font_scale(session->terminal, MIN(3.0, vte_terminal_get_font_scale(session->terminal) + 0.1));
    else if (!g_strcmp0(action, "zoom-out")) vte_terminal_set_font_scale(session->terminal, MAX(0.5, vte_terminal_get_font_scale(session->terminal) - 0.1));
    else if (!g_strcmp0(action, "reconnect")) {
        char *device = g_strdup(session->device);
        gui_select(gui, device); gui_terminal_open(gui); g_free(device);
    } else if (!g_strcmp0(action, "close")) {
        session_stop(session);
        GtkNotebook *notebook = g_object_get_data(G_OBJECT(gui->window), "terminal_notebook");
        int index = gtk_notebook_page_num(notebook, session->page);
        if (index >= 0) gtk_notebook_remove_page(notebook, index);
    }
}
static gboolean terminal_key(GtkEventControllerKey *controller, guint key, guint keycode, GdkModifierType state, gpointer context) {
    (void)controller; (void)keycode; terminal_session_t *session = context;
    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        if (key == GDK_KEY_C || key == GDK_KEY_c) { vte_terminal_copy_clipboard_format(session->terminal, VTE_FORMAT_TEXT); return TRUE; }
        if (key == GDK_KEY_V || key == GDK_KEY_v) { vte_terminal_paste_clipboard(session->terminal); return TRUE; }
    }
    return FALSE;
}
static void terminal_search(GtkSearchEntry *entry, gpointer context) {
    gui_t *gui = context; terminal_session_t *session = current_session(gui); if (!session) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    char *literal = g_regex_escape_string(text, -1); GError *error = NULL;
    VteRegex *regex = *text ? vte_regex_new_for_search(literal, -1, PCRE2_MULTILINE, &error) : NULL;
    vte_terminal_search_set_regex(session->terminal, regex, 0);
    vte_terminal_search_set_wrap_around(session->terminal, TRUE);
    if (regex) { vte_terminal_search_find_next(session->terminal); vte_regex_unref(regex); }
    if (error) { gui_message(gui, error->message); g_clear_error(&error); }
    g_free(literal);
}
GtkWidget *gui_terminal_page(gui_t *gui) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    GtkWidget *toolbar = gtk_flow_box_new(); gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(toolbar), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(toolbar), 8);
    const char *actions[] = {"new", "reconnect", "copy", "paste", "zoom-in", "zoom-out", "clear", "close"};
    const char *titles[] = {"New session", "Reconnect", "Copy", "Paste", "Zoom in", "Zoom out", "Clear", "Close session"};
    for (guint i = 0; i < G_N_ELEMENTS(actions); ++i) {
        GtkWidget *button = gtk_button_new_with_label(titles[i]);
        g_object_set_data(G_OBJECT(button), "terminal_action", (gpointer)actions[i]);
        g_signal_connect(button, "clicked", G_CALLBACK(terminal_action), gui);
        gtk_flow_box_insert(GTK_FLOW_BOX(toolbar), button, -1);
    }
    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_box_append(GTK_BOX(box), gui_button("Open external terminal", "utilities-terminal-symbolic", "terminal-external", FALSE));
    GtkWidget *search = gtk_search_entry_new(); gtk_widget_set_tooltip_text(search, "Search current terminal output");
    g_signal_connect(search, "search-changed", G_CALLBACK(terminal_search), gui); gtk_box_append(GTK_BOX(box), search);
    GtkWidget *notebook = gtk_notebook_new(); gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_widget_set_vexpand(notebook, TRUE); gtk_box_append(GTK_BOX(box), notebook);
    g_object_set_data(G_OBJECT(gui->window), "terminal_notebook", notebook);
    return box;
}
void gui_terminal_command(gui_t *gui, const char *device, const char *const *arguments) {
    if (!device) { gui_message(gui, "Select a device before opening a terminal."); return; }
    terminal_session_t *session = g_new0(terminal_session_t, 1);
    session->gui = gui; session->device = g_strdup(device);
    session->terminal = VTE_TERMINAL(vte_terminal_new()); g_object_ref_sink(session->terminal);
    terminal_settings(gui, session->terminal);
    session->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    session->status = GTK_LABEL(gtk_label_new("Connecting through waddle repl…")); g_object_ref_sink(session->status);
    gtk_label_set_wrap(session->status, TRUE); gtk_label_set_xalign(session->status, 0);
    gtk_box_append(GTK_BOX(session->page), GTK_WIDGET(session->status));
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(session->terminal));
    gtk_box_append(GTK_BOX(session->page), scroll);
    session->tab = GTK_LABEL(gtk_label_new(session->device)); g_object_ref_sink(session->tab);
    GtkNotebook *notebook = g_object_get_data(G_OBJECT(gui->window), "terminal_notebook");
    int page = gtk_notebook_append_page(notebook, session->page, GTK_WIDGET(session->tab));
    g_object_set_data(G_OBJECT(session->page), "session", session);
    gtk_notebook_set_current_page(notebook, page);
    g_signal_connect(session->terminal, "child-exited", G_CALLBACK(session_exited), session);
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(terminal_key), session); gtk_widget_add_controller(GTK_WIDGET(session->terminal), keys);
    g_ptr_array_add(gui->terminals, session);
    char *argv[] = {gui->cli, "repl", "--device", session->device, NULL};
    session->held = TRUE; session->spawning = TRUE; g_application_hold(G_APPLICATION(gui->application));
    GPtrArray *actual = gui_args_new(gui->cli, NULL);
    if (arguments) for (guint i = 0; arguments[i]; ++i) gui_args_add(actual, arguments[i]);
    else for (guint i = 1; argv[i]; ++i) gui_args_add(actual, argv[i]);
    g_ptr_array_add(actual, NULL);
    vte_terminal_spawn_async(session->terminal, VTE_PTY_DEFAULT, NULL, (char **)actual->pdata, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, NULL, 15000, NULL, session_spawned, session);
    g_ptr_array_unref(actual);
    gtk_stack_set_visible_child_name(gui->pages, "devices"); gtk_stack_set_visible_child_name(gui->details, "terminal");
    gtk_widget_grab_focus(GTK_WIDGET(session->terminal));
}
void gui_terminal_open(gui_t *gui) { gui_terminal_command(gui, gui->selected, NULL); }
/* External terminal processes intentionally outlive the manager. GSubprocess's
 * asynchronous wait owns its reference, with no application/window dependency. */
static void external_done(GObject *source, GAsyncResult *result, gpointer context) {
    (void)context;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
    g_object_unref(source);
}
void gui_terminal_external(gui_t *gui, const char *device, const char *const *arguments) {
    const char *candidates[] = {"kgx", "gnome-terminal", "konsole", "xfce4-terminal", "xterm", NULL};
    char *preferred = gui_store_text(gui->store, "Preferences", "external_terminal", "");
    char *program = *preferred ? g_find_program_in_path(preferred) : NULL;
    guint kind = 0;
    if (program) {
        char *base = g_path_get_basename(program);
        for (guint i = 0; candidates[i]; ++i) if (!strcmp(base, candidates[i])) kind = i;
        g_free(base);
    } else if (*preferred) { gui_message(gui, "Configured external terminal executable was not found."); g_free(preferred); return; }
    else for (guint i = 0; candidates[i]; ++i) if ((program = g_find_program_in_path(candidates[i]))) { kind = i; break; }
    g_free(preferred);
    if (!program) { gui_message(gui, "No supported host terminal found. Use the embedded terminal or install kgx, gnome-terminal, konsole, xfce4-terminal or xterm."); return; }
    GPtrArray *argv = gui_args_new(program, kind < 2 ? "--" : (kind == 3 ? "-x" : "-e"));
    gui_args_add(argv, gui->cli);
    if (arguments) for (guint i = 0; arguments[i]; ++i) gui_args_add(argv, arguments[i]);
    else { gui_args_add(argv, "repl"); gui_args_add(argv, "--device"); gui_args_add(argv, device); }
    g_ptr_array_add(argv, NULL);
    GError *error = NULL; GSubprocess *process = g_subprocess_newv((const char *const *)argv->pdata, G_SUBPROCESS_FLAGS_NONE, &error);
    if (!process) { gui_message(gui, error->message); g_clear_error(&error); }
    else {
        g_subprocess_wait_async(process, NULL, external_done, NULL);
    }
    g_ptr_array_unref(argv); g_free(program);
}
void gui_terminals_close(gui_t *gui) {
    /* Session storage must outlive VTE child callbacks and escalation timers.
     * Destroyed after application holds drain in gui_terminal_cleanup. */
    for (guint i = 0; i < gui->terminals->len; ++i) session_stop(gui->terminals->pdata[i]);
}
/** @brief Called after application loop drains child holds, no live callbacks remain. */
void gui_terminal_cleanup(gui_t *gui) {
    for (guint i = 0; i < gui->terminals->len; ++i) session_free(gui->terminals->pdata[i]);
    g_ptr_array_set_size(gui->terminals, 0);
}
/** @brief Update fonts/scrollback for all retained terminal sessions, main-context only. */
void gui_terminal_preferences(gui_t *gui) {
    for (guint i = 0; i < gui->terminals->len; ++i) terminal_settings(gui, ((terminal_session_t *)gui->terminals->pdata[i])->terminal);
}
