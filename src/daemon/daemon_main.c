/**
 * @file daemon_main.c
 * @brief Entry point for waddled (Waddle subsystem background supervisor daemon).
 */

#include "daemon_server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage: waddled [options]\n"
            "Options:\n"
            "  --runtime-dir PATH    Set custom runtime directory (default: $XDG_RUNTIME_DIR/waddle)\n"
            "  --foreground, -F      Run in foreground (do not daemonize)\n"
            "  --daemonize, -d       Daemonize into background\n"
            "  --help, -h            Show this help text\n");
}

int main(int argc, char **argv) {
    const char *runtime_dir = NULL;
    int daemonize = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--runtime-dir") == 0 && i + 1 < argc) {
            runtime_dir = argv[++i];
        } else if (strcmp(argv[i], "--daemonize") == 0 || strcmp(argv[i], "-d") == 0) {
            daemonize = 1;
        } else if (strcmp(argv[i], "--foreground") == 0 || strcmp(argv[i], "-F") == 0) {
            daemonize = 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(stderr);
            return 1;
        }
    }

    if (daemonize) {
        if (daemon(1, 0) != 0) {
            perror("daemon");
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE so broken client connections do not terminate supervisor */
    signal(SIGPIPE, SIG_IGN);

    return daemon_server_run(runtime_dir, &g_stop);
}
