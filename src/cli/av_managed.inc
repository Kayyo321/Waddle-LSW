/* Managed AV orchestration shares the existing resolver and execution bridge.
 * This include is private to host.c; no symbols or ownership cross its TU. */
static int av_child_wait(pid_t child) {
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
static int av_run_helper(char *const arguments[]) {
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) { execvp(arguments[0], arguments); _exit(127); }
    return av_child_wait(child);
}
static int av_bundle_path(const char *relative, char *output, size_t capacity) {
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) return -1;
    executable[length] = '\0';
    char *slash = strrchr(executable, '/');
    if (!slash) return -1;
    *slash = '\0';
    return snprintf(output, capacity, "%s/%s", executable, relative) < (int)capacity ? 0 : -1;
}
/* Destination directory is private, same-user and never a symbolic link. */
static int av_private_directory(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    struct stat status;
    if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0077)) { errno = EACCES; return -1; }
    return 0;
}
/* Atomic replacement of one owned deployment output; descriptors close on all paths. */
static int av_stage_file(const char *relative, const char *directory, const char *name) {
    char source[PATH_MAX], destination[PATH_MAX], temporary[PATH_MAX];
    if (av_bundle_path(relative, source, sizeof(source)) != 0 ||
        snprintf(destination, sizeof(destination), "%s/%s", directory, name) >= (int)sizeof(destination) ||
        snprintf(temporary, sizeof(temporary), "%s/.av_stage_XXXXXX", directory) >= (int)sizeof(temporary))
        return -1;
    int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), output = -1, result = -1;
    if (input < 0) goto cleanup;
    struct stat status;
    if (fstat(input, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 || status.st_size > 16777216)
        goto cleanup;
    if (lstat(destination, &status) == 0) {
        if (!S_ISREG(status.st_mode) || status.st_uid != geteuid()) { errno = EACCES; goto cleanup; }
    } else if (errno != ENOENT) goto cleanup;
    output = mkostemp(temporary, O_CLOEXEC);
    if (output < 0) goto cleanup;
    char bytes[65536];
    for (;;) {
        ssize_t count = read(input, bytes, sizeof(bytes));
        if (count < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (!count) break;
        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(output, bytes + offset, (size_t)(count - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto cleanup;
            offset += written;
        }
    }
    if (fsync(output) != 0 || rename(temporary, destination) != 0) goto cleanup;
    result = 0;
cleanup:
    if (input >= 0) close(input);
    if (output >= 0) { close(output); if (result) unlink(temporary); }
    return result;
}
static int av_deploy(const device_info_t *device, char *agent, size_t capacity) {
    char state[WaddleMaxPathLen], directory[PATH_MAX], drivers[PATH_MAX];
    if (daemon_device_get_state_dir(device->name, state, sizeof(state)) != 0 ||
        snprintf(directory, sizeof(directory), "%s/av", state) >= (int)sizeof(directory) ||
        snprintf(drivers, sizeof(drivers), "%s/ivshmem", directory) >= (int)sizeof(drivers) ||
        av_private_directory(directory) != 0 || av_private_directory(drivers) != 0)
        return 1;
    const char *driver_files[] = {"ivshmem.inf", "ivshmem.sys", "ivshmem.cat", "LICENSE.txt"};
    for (size_t i = 0; i < sizeof(driver_files) / sizeof(driver_files[0]); ++i) {
        char source[128];
        snprintf(source, sizeof(source), "vendor/av/ivshmem/%s", driver_files[i]);
        if (av_stage_file(source, drivers, driver_files[i]) != 0) goto fail;
    }
    if (av_stage_file("waddle-guest-av.exe", directory, "waddle-guest-av.exe") != 0 ||
        av_stage_file("av_wgc.dll", directory, "av_wgc.dll") != 0 ||
        snprintf(agent, capacity, "%s/waddle-guest-av.exe", directory) >= (int)capacity)
        goto fail;
    char *arguments[] = {"waddle", "exec", "--device", (char *)device->name,
        "--translate-path", "--", agent, "--setup", drivers, NULL};
    return cmd_exec(9, arguments, 2);
fail:
    fprintf(stderr, "waddle av: bundled AV agent, native WinRT DLL or signed driver package is missing: %s\n", strerror(errno));
    return 1;
}
static int cmd_av(int argc, char **argv) {
    av_command_t command;
    int parsed = waddle_av_parse((size_t)(argc - 2), (const char *const *)(argv + 2), &command);
    if (parsed != 0) {
        FILE *stream = parsed == 1 ? stdout : stderr;
        fputs("waddle av setup [--device NAME] [--kvmfr] [--uefi] [--gpu PCI_BDF]\n"
              "waddle av probe [--device NAME]\n"
              "waddle av run PROCESS_ID [--device NAME]\n", stream);
        return parsed == 1 ? 0 : 2;
    }
    device_info_t device;
    char socket_path[WaddleMaxPathLen];
    if (resolve_target_device(command.device[0] ? command.device : NULL, &device,
                              socket_path, sizeof(socket_path), 1) != 0)
        return 2;
    daemon_config_t config;
    daemon_config_init_defaults(&config);
    if (daemon_config_load_file(&config, device.config_path) != 0) return 1;
    char state[WaddleMaxPathLen], agent[PATH_MAX];
    if (daemon_device_get_state_dir(device.name, state, sizeof(state)) != 0 ||
        snprintf(agent, sizeof(agent), "%s/av/waddle-guest-av.exe", state) >= (int)sizeof(agent))
        return 1;
    if (command.kind == AvSetup) {
        if (access("/dev/kvm", R_OK | W_OK) != 0 || access("/dev/vhost-vsock", R_OK | W_OK) != 0) {
            char helper[PATH_MAX];
            if (av_bundle_path("waddle-av-setup", helper, sizeof(helper)) != 0) return 1;
            char *arguments[] = {"sudo", "--", helper, "--prepare-host", NULL};
            int result = av_run_helper(arguments);
            if (result != 0 || access("/dev/kvm", R_OK | W_OK) != 0 ||
                access("/dev/vhost-vsock", R_OK | W_OK) != 0) return 1;
        }
        char memory[WaddleMaxPathLen], setting[WaddleMaxPathLen + 16], gpu[32];
        if (command.kvmfr) {
            char helper[PATH_MAX];
            if (av_bundle_path("waddle-av-setup", helper, sizeof(helper)) != 0) return 1;
            char *arguments[] = {"sudo", "--", helper, "--load-module", NULL};
            int result = av_run_helper(arguments);
            if (result != 0) return result;
            strcpy(memory, "/dev/kvmfr0");
        } else if (snprintf(memory, sizeof(memory), "%s/av_shm", state) >= (int)sizeof(memory))
            return 1;
        snprintf(setting, sizeof(setting), "shm_path=%s", memory);
        snprintf(gpu, sizeof(gpu), "gpu_bdf=%s", command.gpu_bdf);
        const char *changes[] = {"enabled=1", setting, gpu, command.uefi ? "uefi=1" : "uefi=0"};
        if (!(config.av_enabled && !strcmp(config.av_shm_path, memory) &&
              !strcmp(config.av_gpu_bdf, command.gpu_bdf) && config.av_uefi == command.uefi) &&
            daemon_device_config_update(device.name, changes, 4, 0, 0) != 0) {
            fprintf(stderr, "waddle av: configuring AV requires a stopped device: %s\n", strerror(errno));
            return 1;
        }
        return av_deploy(&device, agent, sizeof(agent));
    }
    if (!config.av_enabled || access(agent, R_OK) != 0) {
        fputs("waddle av: run av setup for this device first\n", stderr);
        return 1;
    }
    if (command.kind == AvProbe) {
        char *arguments[] = {"waddle", "exec", "--device", device.name, "--translate-path",
            "--", agent, "--probe", NULL};
        return cmd_exec(8, arguments, 2);
    }
    char process_id[16], host_path[PATH_MAX], cid[16];
    snprintf(process_id, sizeof(process_id), "%u", command.process_id);
    snprintf(cid, sizeof(cid), "%u", config.vsock_cid);
    if (av_bundle_path("waddle-av-host", host_path, sizeof(host_path)) != 0 || access(host_path, X_OK) != 0)
        return 1;
    int output[2];
    if (pipe2(output, O_CLOEXEC) != 0) return 1;
    pid_t guest = fork();
    if (guest < 0) { close(output[0]); close(output[1]); return 1; }
    if (!guest) {
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0) _exit(1);
        close(output[1]);
        char *arguments[] = {"waddle", "exec", "--device", device.name, "--translate-path",
            "--", agent, process_id, NULL};
        _exit(cmd_exec(8, arguments, 2));
    }
    close(output[1]);
    char pending[256] = {0};
    size_t length = 0;
    int ready = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 90000;
    while (!ready) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remaining = deadline - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        if (remaining <= 0) break;
        struct pollfd descriptor = {output[0], POLLIN, 0};
        int available = poll(&descriptor, 1, (int)remaining);
        if (available < 0 && errno == EINTR) continue;
        if (available <= 0) break;
        char byte;
        ssize_t count = read(output[0], &byte, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        if (byte == '\n') {
            pending[length] = '\0';
            ready = !strcmp(pending, "AV guest: ready") || !strcmp(pending, "AV guest: ready\r");
            length = 0;
        } else if (length + 1 < sizeof(pending)) pending[length++] = byte;
        else break;
    }
    int result = 1;
    if (ready) {
        char *arguments[] = {host_path, cid, config.av_shm_path, NULL};
        result = av_run_helper(arguments);
    } else fputs("waddle av: guest did not announce listener readiness before deadline\n", stderr);
    close(output[0]);
    /* The child owns only this execution session. Its cancellation closes the
     * bridge job and agent; the user-selected game process is not owned here. */
    if (kill(guest, SIGTERM) != 0 && errno != ESRCH) result = 1;
    int status = 0;
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        pid_t waited = waitpid(guest, &status, WNOHANG);
        if (waited == guest) return result;
        if (waited < 0 && errno != EINTR) return 1;
        struct timespec pause = {0, 20000000};
        nanosleep(&pause, NULL);
    }
    kill(guest, SIGKILL);
    av_child_wait(guest);
    return result;
}
