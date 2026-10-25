/**
 * @file session_main.c
 * @brief CLI boundary tests for the control-only session entry.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/daemon/daemon_device.h"

int waddle_session_main(int argc, char **argv);
void session_main_private_tests(void);
void session_main_private_fail_signalfd(void);
void session_main_private_fail_clock(void);
void session_main_private_set_clock_anomaly(uint32_t anomaly);
void session_main_private_set_random_mode(uint32_t mode);
void session_control_host_main_entry_tests(void);

static void make_directory(const char *path)
{
    assert(mkdir(path, 0700) == 0);
}

static void write_profile(const char *path, const char *mapping_path)
{
    char profile[512];
    int length = snprintf(profile, sizeof(profile),
        "[subsystem]\n"
        "disk_image = /tmp/session-main-test.qcow2\n"
        "vsock_cid = 33\n"
        "[av]\n"
        "enabled = 1\n"
        "shm_path = %s\n", mapping_path);
    assert(length > 0 && (size_t)length < sizeof(profile));
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    size_t offset = 0;
    while (offset < (size_t)length) {
        ssize_t written = write(fd, profile + offset, (size_t)length - offset);
        assert(written > 0);
        offset += (size_t)written;
    }
    assert(close(fd) == 0);
}

static int make_daemon_listener(const char *device_name, char socket_path[256])
{
    assert(daemon_device_get_socket_path(device_name, socket_path, 256) == 0);
    char device_runtime[256];
    int length = snprintf(device_runtime, sizeof(device_runtime),
        "%s/waddle/%s", getenv("XDG_RUNTIME_DIR"), device_name);
    assert(length > 0 && (size_t)length < sizeof(device_runtime));
    assert(mkdir(device_runtime, 0700) == 0);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    assert(strlen(socket_path) < sizeof(address.sun_path));
    strcpy(address.sun_path, socket_path);
    assert(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 4) == 0);
    return listener;
}

static void run_pre_admission_failure(const char *device_name, int listener,
                                      uint32_t random_mode)
{
    char *arguments[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)device_name};
    session_main_private_set_random_mode(random_mode);
    assert(waddle_session_main(4, arguments) == 1);
    session_main_private_set_random_mode(0);
    int client = accept(listener, NULL, NULL);
    assert(client >= 0);
    close(client);
    close(listener);
}

int main(void)
{
    session_main_private_tests();
    char *help_args[] = {(char *)"waddle-session", (char *)"--help"};
    char *valid_without_device[] = {(char *)"waddle-session", (char *)"--control-only"};
    char *missing_control[] = {(char *)"waddle-session", (char *)"--device",
                               (char *)"known"};
    char *bad_kind[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"known", (char *)"--backing-kind",
        (char *)"guess"};
    char *bad_duration[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"known", (char *)"--run-for-ms", (char *)"0"};
    char *duplicate_kind[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"known", (char *)"--backing-kind",
        (char *)"file", (char *)"--backing-kind", (char *)"kvmfr"};
    char *duplicate_duration[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"known", (char *)"--run-for-ms", (char *)"10",
        (char *)"--run-for-ms", (char *)"20"};
    char root[] = "/tmp/session-main-cli-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char config_home[128];
    char state_home[128];
    char runtime_home[128];
    char config_parent[160];
    char config_dir[192];
    char runtime_dir[160];
    char profile_path[224];
    char missing_daemon_profile[224];
    char missing_mapping_path[224];
    char early_profile[224];
    char random_profile[224];
    char early_mapping_path[224];
    char random_mapping_path[224];
    assert(snprintf(config_home, sizeof(config_home), "%s/config", root) > 0);
    assert(snprintf(state_home, sizeof(state_home), "%s/state", root) > 0);
    assert(snprintf(runtime_home, sizeof(runtime_home), "%s/runtime", root) > 0);
    assert(snprintf(config_parent, sizeof(config_parent), "%s/waddle", config_home) > 0);
    assert(snprintf(config_dir, sizeof(config_dir), "%s/devices", config_parent) > 0);
    assert(snprintf(runtime_dir, sizeof(runtime_dir), "%s/waddle", runtime_home) > 0);
    make_directory(config_home);
    make_directory(state_home);
    make_directory(runtime_home);
    make_directory(config_parent);
    make_directory(config_dir);
    make_directory(runtime_dir);
    assert(setenv("XDG_CONFIG_HOME", config_home, 1) == 0);
    assert(setenv("XDG_STATE_HOME", state_home, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", runtime_home, 1) == 0);
    char *missing_device[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"missing"};
    assert(waddle_session_main(4, missing_device) == 2);
    assert(snprintf(profile_path, sizeof(profile_path), "%s/bad-av.ini", config_dir) > 0);
    static const char bad_av_profile[] =
        "[subsystem]\n"
        "disk_image = /tmp/session-main-test.qcow2\n"
        "vsock_cid = 33\n";
    int profile_fd = open(profile_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          0600);
    assert(profile_fd >= 0);
    assert(write(profile_fd, bad_av_profile, sizeof(bad_av_profile) - 1) ==
           sizeof(bad_av_profile) - 1);
    assert(close(profile_fd) == 0);
    char *bad_config[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"bad-av"};
    assert(snprintf(missing_daemon_profile, sizeof(missing_daemon_profile),
        "%s/no-daemon.ini", config_dir) > 0);
    assert(snprintf(missing_mapping_path, sizeof(missing_mapping_path),
        "%s/not-created", root) > 0);
    write_profile(missing_daemon_profile, missing_mapping_path);
    assert(snprintf(early_profile, sizeof(early_profile), "%s/early.ini",
        config_dir) > 0);
    assert(snprintf(random_profile, sizeof(random_profile), "%s/random.ini",
        config_dir) > 0);
    assert(snprintf(early_mapping_path, sizeof(early_mapping_path), "%s/missing-map",
        root) > 0);
    assert(snprintf(random_mapping_path, sizeof(random_mapping_path), "%s/random-map",
        root) > 0);
    write_profile(early_profile, early_mapping_path);
    write_profile(random_profile, random_mapping_path);
    int random_mapping = open(random_mapping_path,
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(random_mapping >= 0 && close(random_mapping) == 0);
    char *missing_daemon[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"no-daemon"};
    char *early_exit[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"early"};
    assert(waddle_session_main(2, help_args) == 0);
    assert(waddle_session_main(2, valid_without_device) == 2);
    assert(waddle_session_main(3, missing_control) == 2);
    assert(waddle_session_main(6, bad_kind) == 2);
    assert(waddle_session_main(6, bad_duration) == 2);
    assert(waddle_session_main(8, duplicate_kind) == 2);
    assert(waddle_session_main(8, duplicate_duration) == 2);
    assert(waddle_session_main(4, bad_config) == 2);
    assert(waddle_session_main(4, missing_daemon) == 1);
    session_main_private_set_clock_anomaly(5);
    assert(waddle_session_main(4, early_exit) == 1);
    session_main_private_fail_clock();
    assert(waddle_session_main(4, early_exit) == 1);
    session_main_private_fail_signalfd();
    assert(waddle_session_main(4, early_exit) == 1);
    char early_socket[256];
    int early_listener = make_daemon_listener("early", early_socket);
    run_pre_admission_failure("early", early_listener, 0);
    char random_socket[256];
    int random_listener = make_daemon_listener("random", random_socket);
    run_pre_admission_failure("random", random_listener, 1);
    assert(unsetenv("XDG_CONFIG_HOME") == 0);
    assert(unsetenv("XDG_STATE_HOME") == 0);
    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(unlink(profile_path) == 0);
    assert(unlink(missing_daemon_profile) == 0);
    assert(unlink(early_profile) == 0);
    assert(unlink(random_profile) == 0);
    assert(unlink(random_mapping_path) == 0);
    assert(rmdir(config_dir) == 0);
    assert(rmdir(config_parent) == 0);
    char early_runtime[256];
    char random_runtime[256];
    assert(snprintf(early_runtime, sizeof(early_runtime), "%s/waddle/early",
        runtime_home) > 0);
    assert(snprintf(random_runtime, sizeof(random_runtime), "%s/waddle/random",
        runtime_home) > 0);
    assert(unlink(early_socket) == 0);
    assert(unlink(random_socket) == 0);
    assert(rmdir(early_runtime) == 0);
    assert(rmdir(random_runtime) == 0);
    assert(rmdir(config_home) == 0);
    assert(rmdir(state_home) == 0);
    assert(rmdir(runtime_dir) == 0);
    assert(rmdir(runtime_home) == 0);
    assert(rmdir(root) == 0);
    session_control_host_main_entry_tests();
    puts("session_main CLI tests passed");
    return 0;
}
