/**
 * @file test_auto_terminal.c
 * @brief Tests for zero-flag default interactive terminal launcher and automatic path translation.
 */

#include "daemon_client.h"
#include "daemon_config.h"
#include "path_rules.h"
#include "waddle/cli_protocol.h"
#include "waddle/daemon_protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_mount_path_translation(void) {
    /* Verify daemon default export config maps host home to Z:\ */
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);
    assert(cfg.mount_count >= 1);
    assert(strcmp(cfg.mounts[0].guest_drive, "Z:\\") == 0);

    /* Test path rule mapping */
    path_rule_t rules[8];
    rules[0] = (path_rule_t){cfg.mounts[0].host_path, cfg.mounts[0].guest_drive};

    char host_path[1024];
    snprintf(host_path, sizeof(host_path), "%.900s/projects/my_app", cfg.mounts[0].host_path);

    char *translated = waddle_translate_rules(host_path, rules, 1);
    assert(translated != NULL);
    assert(strncmp(translated, "Z:\\", 3) == 0);
    assert(strstr(translated, "projects\\my_app") != NULL);
    free(translated);

    /* Test root fallback rule */
    path_rule_t root_rules[2];
    root_rules[0] = (path_rule_t){cfg.mounts[0].host_path, cfg.mounts[0].guest_drive};
    root_rules[1] = (path_rule_t){"/", "Z:\\"};

    char *system_path = waddle_translate_rules("/usr/bin/tool", root_rules, 2);
    assert(system_path != NULL);
    assert(strcmp(system_path, "Z:\\usr\\bin\\tool") == 0);
    free(system_path);
}

static void test_mock_guest_session(void) {
    /* Spawns mock guest on UNIX socket and executes waddle exec */
    char sock_path[96];
    snprintf(sock_path, sizeof(sock_path), "/tmp/waddle_test_autoterm_%d.sock", (int)getpid());
    unlink(sock_path);

    pid_t mock_pid = fork();
    assert(mock_pid >= 0);
    if (mock_pid == 0) {
        execl("./build/waddle-mock-guest", "./build/waddle-mock-guest", "--socket-path", sock_path, (char *)NULL);
        _exit(127);
    }

    /* Wait for mock guest socket to appear without consuming its single connection */
    int connected = 0;
    for (int i = 0; i < 60; i++) {
        usleep(20000);
        struct stat st;
        if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            connected = 1;
            break;
        }
    }
    assert(connected == 1);

    /* Run waddle exec with --pipe and --cwd /tmp */
    pid_t cli_pid = fork();
    assert(cli_pid >= 0);
    if (cli_pid == 0) {
        execl("./build/waddle",
              "./build/waddle",
              "exec",
              "--pipe",
              "--cwd",
              "/tmp",
              "--socket-path",
              sock_path,
              "--",
              "/bin/echo",
              "AUTO_SESSION_SUCCESS",
              (char *)NULL);
        _exit(127);
    }

    int cli_status = 0;
    waitpid(cli_pid, &cli_status, 0);
    assert(WIFEXITED(cli_status) && WEXITSTATUS(cli_status) == 0);

    /* Terminate mock guest */
    kill(mock_pid, SIGTERM);
    int mock_status = 0;
    waitpid(mock_pid, &mock_status, 0);
    unlink(sock_path);
}

static void test_cli_default_options(void) {
    /* Test --version */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDOUT_FILENO);
            close(dev_null);
        }
        execl("./build/waddle", "./build/waddle", "--version", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Test --help */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDOUT_FILENO);
            close(dev_null);
        }
        execl("./build/waddle", "./build/waddle", "--help", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    test_mount_path_translation();
    test_mock_guest_session();
    test_cli_default_options();
    printf("All test_auto_terminal tests passed successfully!\n");
    return 0;
}
