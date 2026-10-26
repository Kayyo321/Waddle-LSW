/**
 * @file session_main.c
 * @brief CLI boundary tests for the control-only session entry.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
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

/* Remove one tree entry; used with nftw depth-first over a test-owned directory. */
static int remove_tree_entry(const char *path, const struct stat *status, int type, struct FTW *info)
{
    (void)status;
    (void)type;
    (void)info;
    return remove(path) == 0 ? 0 : -1;
}

/* Remove a directory tree that the supervised attempts created under the test root. */
static void remove_tree(const char *path)
{
    assert(nftw(path, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
}

/* Remove an empty directory the attempts may have created; a missing one is fine. */
static void remove_empty_if_present(const char *path)
{
    if (rmdir(path) != 0) assert(errno == ENOENT);
}

/* Accept and immediately close daemon connections: each supervised attempt fails at admission. */
static void *close_accepted_clients(void *context)
{
    int listener = *(int *)context;
    for (int index = 0; index < 2; ++index) {
        int client = accept(listener, NULL, NULL);
        assert(client >= 0);
        close(client);
    }
    return NULL;
}

/* Accept one connection, close it, and send SIGTERM during the supervisor's backoff. The thread blocks
 * SIGTERM itself so the signal is never delivered to it, and the runtime's signalfd consumes it. */
static void *close_then_stop(void *context)
{
    int listener = *(int *)context;
    int client = accept(listener, NULL, NULL);
    sigset_t blocked;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
    assert(client >= 0);
    close(client);
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGTERM) == 0);
    assert(pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0);
    nanosleep(&pause, NULL);
    assert(kill(getpid(), SIGTERM) == 0);
    return NULL;
}

/* Accept and close one daemon connection; a missing mapping is terminal after one admission-free attempt. */
static void *close_one_accepted_client(void *context)
{
    int listener = *(int *)context;
    int client = accept(listener, NULL, NULL);
    assert(client >= 0);
    close(client);
    return NULL;
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
    /* Supervised: a daemon that closes each admission is a retryable refusal; one retry, then exit 1. */
    char supervised_profile[224];
    assert(snprintf(supervised_profile, sizeof(supervised_profile), "%s/supervised.ini",
        config_dir) > 0);
    write_profile(supervised_profile, random_mapping_path);
    char supervised_socket[256];
    int supervised_listener = make_daemon_listener("supervised", supervised_socket);
    pthread_t closer;
    assert(pthread_create(&closer, NULL, close_accepted_clients, &supervised_listener) == 0);
    char *supervised_args[] = {(char *)"waddle-session", (char *)"--supervised",
        (char *)"--device", (char *)"supervised", (char *)"--max-retries", (char *)"1"};
    assert(waddle_session_main(6, supervised_args) == 1);
    assert(pthread_join(closer, NULL) == 0);
    close(supervised_listener);
    assert(unlink(supervised_socket) == 0);
    char supervised_runtime[256];
    char supervised_state[256];
    char state_waddle[256];
    char state_devices[256];
    assert(snprintf(supervised_runtime, sizeof(supervised_runtime), "%s/waddle/supervised",
        runtime_home) > 0);
    assert(snprintf(supervised_state, sizeof(supervised_state), "%s/waddle/devices/supervised",
        state_home) > 0);
    assert(snprintf(state_devices, sizeof(state_devices), "%s/waddle/devices", state_home) > 0);
    assert(snprintf(state_waddle, sizeof(state_waddle), "%s/waddle", state_home) > 0);
    remove_tree(supervised_runtime);
    remove_tree(supervised_state);
    remove_empty_if_present(state_devices);
    remove_empty_if_present(state_waddle);
    assert(unlink(supervised_profile) == 0);
    /* A missing mapping after a connected daemon is terminal: one attempt, no retry. */
    char nomap_profile[224];
    assert(snprintf(nomap_profile, sizeof(nomap_profile), "%s/supervised-nomap.ini",
        config_dir) > 0);
    write_profile(nomap_profile, missing_mapping_path);
    char nomap_socket[256];
    int nomap_listener = make_daemon_listener("supervised-nomap", nomap_socket);
    pthread_t nomap_closer;
    assert(pthread_create(&nomap_closer, NULL, close_one_accepted_client, &nomap_listener) == 0);
    char *nomap_args[] = {(char *)"waddle-session", (char *)"--supervised",
        (char *)"--device", (char *)"supervised-nomap", (char *)"--max-retries", (char *)"5"};
    assert(waddle_session_main(6, nomap_args) == 1);
    assert(pthread_join(nomap_closer, NULL) == 0);
    close(nomap_listener);
    assert(unlink(nomap_socket) == 0);
    char nomap_runtime[256];
    assert(snprintf(nomap_runtime, sizeof(nomap_runtime), "%s/waddle/supervised-nomap",
        runtime_home) > 0);
    remove_tree(nomap_runtime);
    assert(unlink(nomap_profile) == 0);
    /* No daemon listening: each refused connection is retryable; one retry, then exit 1. */
    char *supervised_absent[] = {(char *)"waddle-session", (char *)"--supervised",
        (char *)"--device", (char *)"no-daemon", (char *)"--max-retries", (char *)"1"};
    assert(waddle_session_main(6, supervised_absent) == 1);
    /* Supervised mode needs XDG_RUNTIME_DIR; without it the command refuses before any attempt. */
    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(waddle_session_main(6, supervised_absent) == 2);
    assert(setenv("XDG_RUNTIME_DIR", runtime_home, 1) == 0);
    /* A stop signal during the backoff ends the supervised run cleanly with status 0. */
    char stop_profile[224];
    assert(snprintf(stop_profile, sizeof(stop_profile), "%s/stopper.ini", config_dir) > 0);
    write_profile(stop_profile, random_mapping_path);
    char stop_socket[256];
    int stop_listener = make_daemon_listener("stopper", stop_socket);
    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, close_then_stop, &stop_listener) == 0);
    char *stop_args[] = {(char *)"waddle-session", (char *)"--supervised", (char *)"--device",
        (char *)"stopper", (char *)"--max-retries", (char *)"0"};
    assert(waddle_session_main(6, stop_args) == 0);
    assert(pthread_join(stopper, NULL) == 0);
    close(stop_listener);
    assert(unlink(stop_socket) == 0);
    char stop_runtime[256];
    char stop_state[256];
    assert(snprintf(stop_runtime, sizeof(stop_runtime), "%s/waddle/stopper", runtime_home) > 0);
    assert(snprintf(stop_state, sizeof(stop_state), "%s/waddle/devices/stopper", state_home) > 0);
    remove_tree(stop_runtime);
    remove_tree(stop_state);
    remove_empty_if_present(state_devices);
    remove_empty_if_present(state_waddle);
    assert(unlink(stop_profile) == 0);
    /* Another process holds the device's session.lock: admission reports Busy before any daemon
     * exchange, and both modes map it to the handover status, not a failure. */
    char locked_profile[224];
    assert(snprintf(locked_profile, sizeof(locked_profile), "%s/locked.ini", config_dir) > 0);
    write_profile(locked_profile, random_mapping_path);
    char locked_socket[256];
    int locked_listener = make_daemon_listener("locked", locked_socket);
    char locked_lock[256];
    assert(snprintf(locked_lock, sizeof(locked_lock), "%s/waddle/locked/session.lock", runtime_home) > 0);
    int held = open(locked_lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    assert(held >= 0 && flock(held, LOCK_EX | LOCK_NB) == 0);
    char *locked_supervised[] = {(char *)"waddle-session", (char *)"--device", (char *)"locked",
        (char *)"--max-retries", (char *)"1"};
    assert(waddle_session_main(5, locked_supervised) == 75);
    char *locked_one_shot[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"locked"};
    assert(waddle_session_main(4, locked_one_shot) == 75);
    assert(close(held) == 0);
    close(locked_listener);
    assert(unlink(locked_socket) == 0);
    char locked_runtime[256];
    assert(snprintf(locked_runtime, sizeof(locked_runtime), "%s/waddle/locked", runtime_home) > 0);
    remove_tree(locked_runtime);
    assert(unlink(locked_profile) == 0);
    /* A failed token read ends the supervised attempt as an OS failure, not a retry. */
    char random_sup_profile[224];
    assert(snprintf(random_sup_profile, sizeof(random_sup_profile), "%s/randsup.ini", config_dir) > 0);
    write_profile(random_sup_profile, random_mapping_path);
    char randsup_socket[256];
    int randsup_listener = make_daemon_listener("randsup", randsup_socket);
    session_main_private_set_random_mode(1);
    char *randsup_args[] = {(char *)"waddle-session", (char *)"--device", (char *)"randsup",
        (char *)"--max-retries", (char *)"3"};
    assert(waddle_session_main(5, randsup_args) == 1);
    session_main_private_set_random_mode(0);
    close(randsup_listener);
    assert(unlink(randsup_socket) == 0);
    char randsup_runtime[256];
    assert(snprintf(randsup_runtime, sizeof(randsup_runtime), "%s/waddle/randsup", runtime_home) > 0);
    remove_tree(randsup_runtime);
    assert(unlink(random_sup_profile) == 0);
    /* A malformed AV value makes the device configuration unloadable: exit 2. */
    char bad_config_profile[224];
    assert(snprintf(bad_config_profile, sizeof(bad_config_profile), "%s/badcfg.ini", config_dir) > 0);
    {
        int bad_fd = open(bad_config_profile, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        static const char malformed[] = "[subsystem]\ndisk_image = /tmp/x.qcow2\nvsock_cid = 33\n[av]\nenabled = 9\n";
        assert(bad_fd >= 0);
        assert(write(bad_fd, malformed, sizeof(malformed) - 1) == (ssize_t)(sizeof(malformed) - 1));
        assert(close(bad_fd) == 0);
    }
    char *badcfg_args[] = {(char *)"waddle-session", (char *)"--supervised", (char *)"--device", (char *)"badcfg"};
    assert(waddle_session_main(4, badcfg_args) == 2);
    assert(unlink(bad_config_profile) == 0);
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
