/** @file gui_main.c @brief Native single-instance Waddle application. */
#include <adwaita.h>
#include <vte/vte.h>
#include "gui_backend.h"
#include "gui_store.h"
#include <stdio.h>

static void activate(GApplication *application, gpointer context) {
    (void)context;
    GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(application));
    if (!window) {
        window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
        gtk_window_set_title(window, "Waddle");
        gtk_window_set_default_size(window, 1100, 760);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(box), adw_header_bar_new());
        AdwStatusPage *status = ADW_STATUS_PAGE(adw_status_page_new());
        adw_status_page_set_title(status, "Your Windows devices");
        adw_status_page_set_description(status, "Opening Waddle never starts a device.");
        adw_status_page_set_icon_name(status, "computer-symbolic");
        gtk_widget_set_vexpand(GTK_WIDGET(status), TRUE);
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(status));
        adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
    }
    gtk_window_present(window);
}
/** @brief Run single-instance GUI; argv borrowed for run, application freed on return.
 * @return GApplication status, 1 without a display. GTK main thread only. */
int main(int argc, char **argv) {
    if (!g_getenv("DISPLAY") && !g_getenv("WAYLAND_DISPLAY")) {
        fputs("waddle: the GUI needs a graphical session. Use 'waddle repl' for the terminal.\n", stderr);
        return 1;
    }
    AdwApplication *application = adw_application_new("io.github.Kayyo321.Waddle", G_APPLICATION_DEFAULT_FLAGS);
    g_application_add_main_option(G_APPLICATION(application), "device", 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_STRING, "Select a device without booting it", "NAME");
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
