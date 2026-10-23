/** @file gui_main.c @brief Single-instance, command-line-aware native Waddle application. */
#include "gui_private.h"
#include <stdio.h>
#include <string.h>

static void activate(GApplication *application, gpointer context) {
    (void)application;
    gui_window_present(context);
}
static int command_line(GApplication *application, GApplicationCommandLine *command, gpointer context) {
    gui_t *gui = context;
    if (gui->closing) { g_application_command_line_printerr(command, "Waddle is closing; retry after its sessions finish detaching.\n"); return 1; }
    GVariantDict *options = g_application_command_line_get_options_dict(command);
    const char *device = NULL;
    if (g_variant_dict_lookup(options, "device", "&s", &device)) {
        if (!gui_name_valid(device)) {
            g_application_command_line_printerr(command, "waddle gui: invalid device name\n");
            return 2;
        }
        gui->device_requested = TRUE;
        g_free(gui->requested);
        gui->requested = g_strdup(device);
    }
    g_application_activate(application);
    if (device && gui->registry) { gui_select(gui, device); g_clear_pointer(&gui->requested, g_free); }
    return 0;
}
static void action_activate(GSimpleAction *action, GVariant *parameter, gpointer context) {
    (void)action;
    if (parameter) gui_dispatch(context, g_variant_get_string(parameter, NULL));
}
/** @brief Run GTK application; argv borrowed, returns 1 headless or application status.
 * All GUI and async holds drain before owned state is freed. Main thread only. */
int main(int argc, char **argv) {
    if ((!g_getenv("DISPLAY") || !*g_getenv("DISPLAY")) &&
        (!g_getenv("WAYLAND_DISPLAY") || !*g_getenv("WAYLAND_DISPLAY"))) {
        fputs("waddle: the GUI needs a graphical session. Use 'waddle repl' for the terminal.\n", stderr);
        return 1;
    }
    if (!gtk_init_check()) {
        fputs("waddle: cannot connect to a graphical session. Use 'waddle repl' for the terminal.\n", stderr);
        return 1;
    }
    gui_t gui = {0};
    gui.cli = gui_sibling_path("waddle");
    if (!gui.cli) return 1;
    GError *error = NULL;
    gui.store = gui_store_load(&error);
    if (error) { g_printerr("Waddle preferences: %s\n", error->message); g_clear_error(&error); }
    gui.terminals = g_ptr_array_new();
    gui.application = adw_application_new("io.github.Kayyo321.Waddle", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_main_option(G_APPLICATION(gui.application), "device", 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_STRING, "Select a device without booting it", "NAME");
    GSimpleAction *action = g_simple_action_new("action", G_VARIANT_TYPE_STRING);
    g_signal_connect(action, "activate", G_CALLBACK(action_activate), &gui);
    g_action_map_add_action(G_ACTION_MAP(gui.application), G_ACTION(action));
    g_object_unref(action);
    const char *palette_keys[] = {"<Primary>k", NULL};
    const char *refresh_keys[] = {"F5", NULL};
    const char *terminal_keys[] = {"<Primary><Shift>t", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(gui.application), "app.action('palette')", palette_keys);
    gtk_application_set_accels_for_action(GTK_APPLICATION(gui.application), "app.action('refresh')", refresh_keys);
    gtk_application_set_accels_for_action(GTK_APPLICATION(gui.application), "app.action('terminal')", terminal_keys);
    g_signal_connect(gui.application, "activate", G_CALLBACK(activate), &gui);
    g_signal_connect(gui.application, "command-line", G_CALLBACK(command_line), &gui);
    int status = g_application_run(G_APPLICATION(gui.application), argc, argv);
    g_clear_object(&gui.registry);
    gui_terminal_cleanup(&gui);
    g_ptr_array_unref(gui.terminals);
    gui_store_free(gui.store);
    for (guint i = 0; i < GuiSettingCount; ++i) g_free(gui.originals[i]);
    g_strfreev(gui.last_argv);
    g_free(gui.selected); g_free(gui.requested); g_free(gui.cli);
    g_free(gui.operation); g_free(gui.operation_device); g_free(gui.pending_av_device); g_free(gui.config_device); g_free(gui.initial_page);
    g_object_unref(gui.application);
    return status;
}
