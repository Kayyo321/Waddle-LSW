/**
 * @file test_daemon_mount_lifecycle.c
 * @brief Isolated process-wrapper tests for ordered multi-mount supervision.
 *
 * This fixture replaces process, AV, and device operations with deterministic
 * recorders. It never launches QEMU/virtiofsd or opens a host export.
 */
#include "daemon_state.h"
#include "daemon_device.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <unistd.h>

static int spawn_mount_calls;
static int stop_mount_calls;
static int stopped_pids[WaddleMaxMounts * 4];
static int fail_mount_index = -1;
static int fail_stop_pid = -1;
static int mount_poll_dead_pid = -1;
static int fail_fs_binary;
static int fail_qemu_binary;
static int fail_root_validation;
static int fail_qemu_spawn;
static int fail_qemu_kill;
static int fail_av_prepare;
static int qemu_poll_dead;
static int named_device_mode;
static int named_config_loads;
static char named_runtime_path[WaddleMaxPathLen];
static char named_state_path[WaddleMaxPathLen];
static int qemu_spawn_calls;
static int qemu_kill_calls;
static int qemu_cleanup_calls;
static int av_free_calls;
static int av_prepare_calls;
static int mock_vsock_ready;
static uint64_t mock_monotonic_ms;
static int event_count;
static char events[128][24];
static qemu_virtiofs_mount_t seen_mounts[WaddleMaxMounts];
static size_t seen_mount_count;

static void record_event(const char *event) {
    assert(event_count < (int)(sizeof(events) / sizeof(events[0])));
    snprintf(events[event_count++], sizeof(events[0]), "%s", event);
}

static void reset_mocks(void) {
    spawn_mount_calls = 0;
    stop_mount_calls = 0;
    memset(stopped_pids, 0, sizeof(stopped_pids));
    fail_mount_index = -1;
    fail_stop_pid = -1;
    mount_poll_dead_pid = -1;
    fail_fs_binary = 0;
    fail_qemu_binary = 0;
    fail_root_validation = 0;
    fail_qemu_spawn = 0;
    fail_qemu_kill = 0;
    fail_av_prepare = 0;
    qemu_poll_dead = 0;
    named_device_mode = 0;
    named_config_loads = 0;
    named_runtime_path[0] = '\0';
    named_state_path[0] = '\0';
    qemu_spawn_calls = 0;
    qemu_kill_calls = 0;
    qemu_cleanup_calls = 0;
    av_free_calls = 0;
    av_prepare_calls = 0;
    mock_vsock_ready = 0;
    mock_monotonic_ms = 0;
    event_count = 0;
    seen_mount_count = 0;
}

static void init_state(daemon_state_t *state, uint32_t mount_count) {
    memset(state, 0, sizeof(*state));
    state->state = SubsystemStateStopped;
    state->lock_fd = -1;
    state->av_environment.fd = -1;
    state->qemu.pid = 0;
    state->qemu.log_fd = -1;
    state->qemu.qmp.socket_fd = -1;
    state->virtiofs.pid = 0;
    state->virtiofs.log_fd = -1;
    for (size_t i = 0; i < WaddleMaxMounts - 1; i++) {
        state->virtiofs_additional[i].pid = 0;
        state->virtiofs_additional[i].log_fd = -1;
    }
    daemon_config_init_defaults(&state->config);
    state->config.mount_count = mount_count;
    snprintf(state->runtime_dir, sizeof(state->runtime_dir), "/tmp/waddle-mount-test");
    snprintf(state->qmp_sock_path, sizeof(state->qmp_sock_path), "/tmp/waddle-mount-test/qmp.sock");
    snprintf(state->qemu_log_path, sizeof(state->qemu_log_path), "/tmp/waddle-mount-test/qemu.log");
    snprintf(state->virtiofsd_sock_path, sizeof(state->virtiofsd_sock_path), "/tmp/waddle-mount-test/virtiofsd.sock");
    snprintf(state->virtiofsd_log_path, sizeof(state->virtiofsd_log_path), "/tmp/waddle-mount-test/virtiofsd.log");
    for (size_t i = 0; i < WaddleMaxMounts - 1; i++) {
        snprintf(state->virtiofsd_additional_sock_path[i], WaddleMaxPathLen,
                 "/tmp/waddle-mount-test/virtiofsd-%zu.sock", i + 1);
        snprintf(state->virtiofsd_additional_log_path[i], WaddleMaxPathLen,
                 "/tmp/waddle-mount-test/virtiofsd-%zu.log", i + 1);
    }
    for (size_t i = 0; i < mount_count; i++) {
        snprintf(state->config.mounts[i].host_path, WaddleMaxPathLen, "/tmp/export-%zu", i);
        snprintf(state->config.mounts[i].guest_drive, sizeof(state->config.mounts[i].guest_drive), "%c:\\",
                 (int)('D' + i));
        state->config.mounts[i].read_only = (uint32_t)(i & 1U);
    }
}

/* The canonical-root module has separate real-filesystem tests. This seam
 * verifies whole-set preflight happens before supervisor side effects. */
int waddle_path_roots_validate(const char *const *roots, size_t count, char **output) {
    if (fail_root_validation) { errno = ENOENT; return -1; }
    for (size_t i = 0; i < count; i++) { output[i] = strdup(roots[i]); assert(output[i] != NULL); }
    return (int)count;
}
void waddle_path_roots_free(char **roots, size_t count) {
    for (size_t i = 0; i < count; i++) { free(roots[i]); roots[i] = NULL; }
}

/* Device/config/AV mocks satisfy daemon_state.c's direct dependencies. */
void daemon_config_init_defaults(daemon_config_t *config) { memset(config, 0, sizeof(*config)); config->start_timeout_sec = 1; config->stop_timeout_sec = 1; }
int daemon_config_load_file(daemon_config_t *config, const char *path) {
    (void)config;
    if (named_device_mode && path != NULL && ++named_config_loads > 1) return 0;
    errno = ENOENT; return -1;
}
int daemon_device_open_directory(const char *path, int create) {
    if (create && mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}
int daemon_device_get_socket_path(const char *name, char *path, size_t capacity) {
    if (named_device_mode && strcmp(name, "named") == 0) {
        int written = snprintf(path, capacity, "%s/daemon.sock", named_runtime_path);
        return written >= 0 && (size_t)written < capacity ? 0 : -1;
    }
    errno = ENOENT; return -1;
}
int daemon_device_list(device_list_t *list) { (void)list; return 0; }
int daemon_device_find(const char *name, device_info_t *info) {
    if (named_device_mode && strcmp(name, "named") == 0) {
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "named");
        snprintf(info->config_path, sizeof(info->config_path), "/mock/named.ini");
        snprintf(info->state_dir, sizeof(info->state_dir), "%s", named_state_path);
        info->config_valid = 1;
        return 0;
    }
    errno = ENOENT; return -1;
}
int daemon_device_registry_lock(int exclusive) { (void)exclusive; return open("/dev/null", O_RDONLY | O_CLOEXEC); }
const char *waddle_subsystem_state_to_string(waddle_subsystem_state_t state) { (void)state; return "test"; }
int av_environment_prepare(av_environment_t *environment, const daemon_config_t *config, char *error, size_t cap) {
    (void)config; av_prepare_calls++;
    if (fail_av_prepare) { snprintf(error, cap, "mock AV prepare failure"); errno = ENOMEM; return -1; }
    environment->fd = 100; return 0;
}
void av_environment_free(av_environment_t *environment) { av_free_calls++; if (environment != NULL) environment->fd = -1; }

int daemon_fs_find_binary(char *path, size_t capacity) {
    if (fail_fs_binary) { errno = ENOENT; return -1; }
    snprintf(path, capacity, "/mock/virtiofsd");
    return 0;
}
int daemon_fs_spawn_for_mount(virtiofs_process_t *proc, const char *socket_path,
                              const char *shared_dir, uint32_t read_only,
                              const char *log_path, const char *binary_path,
                              uint32_t timeout_ms) {
    (void)read_only; (void)binary_path; (void)timeout_ms;
    int index = spawn_mount_calls++;
    record_event("spawn-mount");
    snprintf(proc->socket_path, sizeof(proc->socket_path), "%s", socket_path);
    snprintf(proc->shared_dir, sizeof(proc->shared_dir), "%s", shared_dir);
    snprintf(proc->log_path, sizeof(proc->log_path), "%s", log_path);
    proc->log_fd = 50 + index;
    proc->pid = 200 + index;
    proc->is_running = 1;
    if (index == fail_mount_index) { errno = EIO; return -1; }
    return 0;
}
int daemon_fs_stop(virtiofs_process_t *proc, uint32_t timeout_ms) {
    (void)timeout_ms;
    stop_mount_calls++;
    stopped_pids[stop_mount_calls - 1] = (int)proc->pid;
    record_event("stop-mount");
    if (proc->pid == fail_stop_pid) { errno = EIO; return -1; }
    proc->pid = 0; proc->is_running = 0; proc->log_fd = -1;
    proc->socket_path[0] = '\0'; proc->shared_dir[0] = '\0'; proc->log_path[0] = '\0';
    return 0;
}
int daemon_fs_poll_status(virtiofs_process_t *proc) {
    if (proc->pid == mount_poll_dead_pid) { proc->pid = 0; proc->is_running = 0; return 0; }
    return proc->pid > 0 ? 1 : 0;
}
void daemon_fs_cleanup(virtiofs_process_t *proc) { (void)proc; }

int qemu_find_binary(char *path, size_t capacity) {
    if (fail_qemu_binary) { errno = ENOENT; return -1; }
    snprintf(path, capacity, "/mock/qemu"); return 0;
}
int qemu_spawn_for_device_mounts(qemu_process_t *proc, const daemon_config_t *config,
                                 const char *device_name, const char *qmp_path,
                                 const qemu_virtiofs_mount_t *mounts, size_t mount_count,
                                 const char *log_path, const char *binary_path) {
    (void)config; (void)device_name; (void)log_path; (void)binary_path;
    qemu_spawn_calls++;
    record_event("spawn-qemu");
    seen_mount_count = mount_count;
    assert((mount_count == 0 && mounts == NULL) || (mount_count > 0 && mounts != NULL));
    if (mount_count > 0) memcpy(seen_mounts, mounts, mount_count * sizeof(*mounts));
    if (fail_qemu_spawn) { errno = EIO; return -1; }
    proc->pid = 100; proc->is_running = 1; proc->log_fd = 60;
    snprintf(proc->qmp_sock_path, sizeof(proc->qmp_sock_path), "%s", qmp_path);
    return 0;
}
int qemu_poll_status(qemu_process_t *proc) {
    if (qemu_poll_dead) { proc->pid = 0; proc->is_running = 0; return 0; }
    return proc->is_running ? 1 : 0;
}
int qemu_shutdown_graceful(qemu_process_t *proc) { (void)proc; record_event("graceful-qemu"); return 0; }
int qemu_kill(qemu_process_t *proc) {
    qemu_kill_calls++; record_event("kill-qemu");
    if (fail_qemu_kill) { errno = EIO; return -1; }
    proc->pid = 0; proc->is_running = 0; return 0;
}
void qemu_cleanup(qemu_process_t *proc) {
    qemu_cleanup_calls++;
    if (proc != NULL) proc->log_fd = -1;
}

int socket(int domain, int type, int protocol) {
    (void)type; (void)protocol;
    if (domain == AF_VSOCK && mock_vsock_ready) return 77;
    errno = ENODEV;
    return -1;
}
int connect(int fd, __CONST_SOCKADDR_ARG address, socklen_t length) {
    (void)fd; (void)address; (void)length;
    return mock_vsock_ready ? 0 : -1;
}
int usleep(useconds_t usec) { (void)usec; return 0; }
int clock_gettime(clockid_t clock_id, struct timespec *value) {
    (void)clock_id;
    mock_monotonic_ms += 100;
    value->tv_sec = (time_t)(mock_monotonic_ms / 1000);
    value->tv_nsec = (long)(mock_monotonic_ms % 1000) * 1000000L;
    return 0;
}

static void test_eight_mount_order_and_shutdown(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, WaddleMaxMounts);
    state.config.av_enabled = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    assert(spawn_mount_calls == WaddleMaxMounts);
    assert(seen_mount_count == WaddleMaxMounts);
    assert(strcmp(seen_mounts[0].socket_path, state.virtiofsd_sock_path) == 0);
    assert(strcmp(seen_mounts[7].socket_path, state.virtiofsd_additional_sock_path[6]) == 0);
    assert(strcmp(seen_mounts[0].guest_drive, "D:\\") == 0);
    assert(strcmp(seen_mounts[7].guest_drive, "K:\\") == 0);
    assert(state.virtiofs_owned_count == WaddleMaxMounts);
    assert(daemon_state_kill_subsystem(&state) == 0);
    assert(qemu_kill_calls == 1 && stop_mount_calls == WaddleMaxMounts);
    assert(av_free_calls == 1);
    assert(state.virtiofs_owned_count == 0);
}

static void test_failed_spawn_rolls_back_reverse_and_retains_failure(void) {
    for (int failed_index = 0; failed_index < (int)WaddleMaxMounts; failed_index++) {
        daemon_state_t state;
        reset_mocks(); init_state(&state, WaddleMaxMounts);
        fail_mount_index = failed_index;
        assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
        assert(state.state == SubsystemStateFailed);
        assert(spawn_mount_calls == failed_index + 1);
        assert(event_count >= (failed_index + 1) + (failed_index + 1));
        for (int i = 0; i <= failed_index; i++) assert(strcmp(events[i], "spawn-mount") == 0);
        for (int i = 0; i <= failed_index; i++) assert(strcmp(events[failed_index + 1 + i], "stop-mount") == 0);
        for (int i = 0; i <= failed_index; i++) assert(stopped_pids[i] == 200 + failed_index - i);
        assert(state.virtiofs_owned_count == 0);
        assert(qemu_spawn_calls == 0);
    }
}

static void test_missing_backend_and_retry_identity(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 1); fail_fs_binary = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
    assert(spawn_mount_calls == 0 && qemu_spawn_calls == 0);

    reset_mocks(); init_state(&state, 1);
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    state.state = SubsystemStateFailed;
    state.config.mounts[0].host_path[0] = '/';
    state.config.mounts[0].host_path[1] = 'x';
    state.config.mounts[0].host_path[2] = '\0';
    errno = 0;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1 && errno == EBUSY);
    assert(spawn_mount_calls == 1 && qemu_spawn_calls == 1);
    assert(daemon_state_kill_subsystem(&state) == 0);

    reset_mocks(); init_state(&state, 1); fail_qemu_binary = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
    assert(spawn_mount_calls == 0 && qemu_spawn_calls == 0);
    assert(state.virtiofs_owned_count == 0);

    reset_mocks(); init_state(&state, 2); fail_qemu_spawn = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
    assert(qemu_spawn_calls == 1 && stop_mount_calls == 2);
    assert(state.virtiofs_owned_count == 0);
}

static void test_graceful_only_preserves_and_stop_failure_retries(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 2);
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    errno = 0;
    assert(daemon_state_stop_subsystem(&state, WaddleStopGracefulOnly, 0) == -1);
    assert(errno == ETIMEDOUT);
    assert(qemu_cleanup_calls == 0);
    assert(state.qemu.pid == 100 && state.virtiofs_owned_count == 2);
    fail_stop_pid = 200;
    assert(daemon_state_kill_subsystem(&state) == -1);
    assert(qemu_cleanup_calls == 1);
    assert(state.state == SubsystemStateFailed && state.virtiofs_owned_count == 1);
    assert(state.virtiofs.pid == 200 && state.virtiofs.socket_path[0] != '\0');
    daemon_state_cleanup(&state);
    assert(state.virtiofs_owned_count == 1 && state.virtiofs.pid == 200);
    fail_stop_pid = -1;
    daemon_state_cleanup(&state);
    assert(state.virtiofs_owned_count == 0);
    assert(qemu_cleanup_calls == 1);
}

static void test_qemu_kill_failure_retains_log_resources(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 0);
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    fail_qemu_kill = 1;
    errno = 0;
    assert(daemon_state_kill_subsystem(&state) == -1 && errno == EIO);
    assert(state.qemu.pid == 100 && state.qemu.is_running && state.qemu.log_fd == 60);
    assert(qemu_cleanup_calls == 0);
    fail_qemu_kill = 0;
    assert(daemon_state_kill_subsystem(&state) == 0);
    assert(qemu_cleanup_calls == 1 && state.qemu.log_fd == -1);
}

static void test_readiness_timeout_retries_same_processes(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 2);
    assert(daemon_state_start_subsystem(&state, DaemonStartFlagWaitGuest, 1) == -1);
    assert(state.state == SubsystemStateFailed);
    assert(state.qemu.pid == 100 && state.virtiofs_owned_count == 2);
    assert(spawn_mount_calls == 2 && qemu_spawn_calls == 1);
    mock_vsock_ready = 1;
    assert(daemon_state_start_subsystem(&state, DaemonStartFlagWaitGuest, 1) == 0);
    assert(spawn_mount_calls == 2 && qemu_spawn_calls == 1);
    assert(state.state == SubsystemStateRunning);
    assert(daemon_state_kill_subsystem(&state) == 0);
}

static void test_named_device_acquire_rewrites_every_mount_log_path(void) {
    char root[] = "/tmp/waddle named mount test XXXXXX";
    assert(mkdtemp(root) != NULL);
    char runtime[128];
    char state_dir[128];
    snprintf(runtime, sizeof(runtime), "%s/named", root);
    snprintf(state_dir, sizeof(state_dir), "%s/persistent", root);
    assert(mkdir(runtime, 0700) == 0);
    assert(mkdir(state_dir, 0700) == 0);
    reset_mocks();
    named_device_mode = 1;
    snprintf(named_runtime_path, sizeof(named_runtime_path), "%s", runtime);
    snprintf(named_state_path, sizeof(named_state_path), "%s", state_dir);

    daemon_state_t state;
    assert(daemon_state_init(&state, runtime) == 0);
    assert(strstr(state.virtiofsd_additional_log_path[0], "/named/logs/") != NULL);
    assert(daemon_state_acquire_lock(&state) == 0);
    char expected[160];
    snprintf(expected, sizeof(expected), "%s/logs/virtiofsd-1.log", state_dir);
    assert(strcmp(state.virtiofsd_additional_log_path[0], expected) == 0);
    assert(strstr(state.virtiofsd_additional_log_path[0], "/named/logs/") == NULL);
    daemon_state_release_lock(&state);

    unlink(state.lock_path);
    char runtime_logs[160];
    char state_logs[160];
    snprintf(runtime_logs, sizeof(runtime_logs), "%s/logs", runtime);
    snprintf(state_logs, sizeof(state_logs), "%s/logs", state_dir);
    rmdir(runtime_logs);
    rmdir(state_logs);
    rmdir(state_dir);
    rmdir(runtime);
    rmdir(root);
}

static void test_invalid_root_set_rejected_before_spawning(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 2);
    fail_root_validation = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1 && errno == ENOENT);
    assert(state.state == SubsystemStateFailed);
    assert(spawn_mount_calls == 0 && qemu_spawn_calls == 0 && av_prepare_calls == 0);
}

static void test_duplicate_drives_rejected_before_spawning(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 2);
    snprintf(state.config.mounts[1].guest_drive, sizeof(state.config.mounts[1].guest_drive), "d:/");
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
    assert(errno == EINVAL);
    assert(spawn_mount_calls == 0 && qemu_spawn_calls == 0);
}

static void test_dead_qemu_allows_config_change_after_cleanup(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 1);
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    state.state = SubsystemStateFailed;
    snprintf(state.config.mounts[0].host_path, WaddleMaxPathLen, "/tmp/updated-export");
    qemu_poll_dead = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    assert(qemu_kill_calls == 0);
    assert(qemu_cleanup_calls == 1);
    assert(spawn_mount_calls == 2 && qemu_spawn_calls == 2);
    assert(strcmp(seen_mounts[0].socket_path, state.virtiofsd_sock_path) == 0);
    assert(daemon_state_kill_subsystem(&state) == 0);

    reset_mocks(); init_state(&state, 0);
    state.config.av_enabled = 0;
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    state.state = SubsystemStateFailed;
    qemu_poll_dead = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    assert(qemu_cleanup_calls == 1);
    assert(qemu_spawn_calls == 2 && spawn_mount_calls == 0);
    assert(daemon_state_kill_subsystem(&state) == 0);
}

static void test_rollback_failures_override_original_start_error(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 2);
    fail_mount_index = 1;
    fail_stop_pid = 201;
    errno = 0;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
    assert(errno == EIO);
    assert(state.virtiofs_owned_count == 2 && state.virtiofs.pid == 200 &&
           state.virtiofs_additional[0].pid == 201);
    assert(strstr(state.last_error, "spawn virtiofsd mount 1") != NULL);
    assert(strstr(state.last_error, "mount cleanup failed") != NULL);
    fail_mount_index = -1;
    fail_stop_pid = -1;
    assert(daemon_state_kill_subsystem(&state) == 0);

    reset_mocks(); init_state(&state, 1);
    state.config.av_enabled = 1;
    fail_av_prepare = 1;
    fail_stop_pid = 200;
    errno = 0;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
    assert(errno == EIO && state.virtiofs_owned_count == 1);
    assert(strstr(state.last_error, "mock AV prepare failure") != NULL);
    assert(strstr(state.last_error, "mount cleanup failed") != NULL);
    fail_stop_pid = -1;
    assert(daemon_state_kill_subsystem(&state) == 0);

    reset_mocks(); init_state(&state, 1);
    fail_qemu_spawn = 1;
    fail_stop_pid = 200;
    errno = 0;
    assert(daemon_state_start_subsystem(&state, 0, 1) == -1);
    assert(errno == EIO && state.virtiofs_owned_count == 1);
    assert(strstr(state.last_error, "spawn QEMU hypervisor") != NULL);
    assert(strstr(state.last_error, "mount cleanup failed") != NULL);
    fail_stop_pid = -1;
    assert(daemon_state_kill_subsystem(&state) == 0);
}

static void test_child_exit_is_qemu_first_and_dead_vm_restarts_cleanly(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 2); state.config.av_enabled = 1;
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    mount_poll_dead_pid = 201;
    daemon_state_reap_children(&state);
    assert(state.state == SubsystemStateFailed);
    assert(strcmp(events[0], "spawn-mount") == 0 && strcmp(events[2], "spawn-qemu") == 0);
    assert(strcmp(events[3], "kill-qemu") == 0);
    assert(stopped_pids[0] == 0 && stopped_pids[1] == 200);
    assert(qemu_kill_calls == 1 && state.virtiofs_owned_count == 0);
    assert(av_free_calls == 1);

    reset_mocks(); init_state(&state, 1);
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    state.state = SubsystemStateFailed;
    state.qemu.is_running = 1;
    /* Model qemu_poll_status() having reaped the prior VM. */
    state.qemu.pid = 0;
    state.qemu.is_running = 0;
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    assert(qemu_spawn_calls == 2 && spawn_mount_calls == 2);
    assert(state.virtiofs_owned_count == 1);
    assert(daemon_state_kill_subsystem(&state) == 0);
}

static void test_zero_mount_and_status_legacy_pid(void) {
    daemon_state_t state;
    reset_mocks(); init_state(&state, 0);
    assert(daemon_state_start_subsystem(&state, 0, 1) == 0);
    assert(seen_mount_count == 0 && spawn_mount_calls == 0);
    assert(daemon_state_kill_subsystem(&state) == 0);

    reset_mocks(); init_state(&state, 2);
    state.virtiofs.pid = 4321;
    waddle_daemon_status_resp_t response;
    daemon_state_get_status(&state, &response);
    assert(response.virtiofsd_pid == 4321);
}

int main(void) {
    test_eight_mount_order_and_shutdown();
    test_failed_spawn_rolls_back_reverse_and_retains_failure();
    test_missing_backend_and_retry_identity();
    test_named_device_acquire_rewrites_every_mount_log_path();
    test_invalid_root_set_rejected_before_spawning();
    test_duplicate_drives_rejected_before_spawning();
    test_dead_qemu_allows_config_change_after_cleanup();
    test_rollback_failures_override_original_start_error();
    test_graceful_only_preserves_and_stop_failure_retries();
    test_qemu_kill_failure_retains_log_resources();
    test_readiness_timeout_retries_same_processes();
    test_child_exit_is_qemu_first_and_dead_vm_restarts_cleanly();
    test_zero_mount_and_status_legacy_pid();
    puts("daemon mount lifecycle tests passed");
    return 0;
}
