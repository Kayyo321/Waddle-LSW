#include <gio/gdesktopappinfo.h>
#include <glib.h>

#include <stdio.h>
#include <string.h>

/**
 * @brief Parse one generated Desktop Entry with GLib and compare its argv.
 * @param[in] argc Process argument count; must be 3.
 * @param[in] argv Borrowed, non-null UTF-8 arguments: executable, desktop path,
 *                  and expected executable path. No pointer is retained.
 * @return 0 on exact parse, 2..6 on usage, parsing, or mismatch errors.
 * @note Single-threaded test fixture. GLib owns its returned app-info and argv
 *       values; this function releases both before returning. It never launches.
 */
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: consumer DESKTOP_FILE EXPECTED_EXECUTABLE\n");
        return 2;
    }

    GDesktopAppInfo *app_info = g_desktop_app_info_new_from_filename(argv[1]);
    if (app_info == NULL) {
        fprintf(stderr, "GDesktopAppInfo rejected the generated file\n");
        return 3;
    }

    const char *commandline = g_app_info_get_commandline(G_APP_INFO(app_info));
    gint parsed_count = 0;
    gchar **parsed_argv = NULL;
    GError *error = NULL;
    if (commandline == NULL || !g_shell_parse_argv(commandline, &parsed_count, &parsed_argv, &error)) {
        fprintf(stderr, "GLib could not decode parsed Exec: %s\n", error == NULL ? "no command line" : error->message);
        if (error != NULL) g_error_free(error);
        g_object_unref(app_info);
        return 4;
    }

    const char *Expected[] = { argv[2], "open", "--device", "default", "--app", "a-App", "%F" };
    int result = 0;
    if (parsed_count != (gint)G_N_ELEMENTS(Expected)) {
        fprintf(stderr, "unexpected argv count: %d\n", parsed_count);
        result = 5;
    } else {
        for (gint index = 0; index < parsed_count; ++index) {
            if (strcmp(parsed_argv[index], Expected[index]) != 0) {
                fprintf(stderr, "argv[%d] mismatch\n", index);
                result = 6;
                break;
            }
        }
    }

    g_strfreev(parsed_argv);
    g_object_unref(app_info);
    return result;
}
