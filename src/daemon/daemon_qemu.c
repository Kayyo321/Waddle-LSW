/**
 * @file daemon_qemu.c
 * @brief Implementation of QEMU hypervisor process manager and argument builder.
 */

#include "daemon_qemu.h"
#include "../av/av_layout.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * @brief Appends a duplicated string to the argument vector.
 *
 * @param[in,out] argv Array of strings.
 * @param[in,out] argc Pointer to current argument count.
 * @param[in]     max  Maximum allowed argument capacity.
 * @param[in]     val  String value to duplicate and append.
 * @return 0 on success, or -1 on allocation failure or capacity exceeded.
 */
static int append_arg(char **argv, size_t *argc, size_t max, const char *val) {
    if (*argc + 1 >= max) {
        errno = E2BIG;
        return -1;
    }
    char *copy = strdup(val);
    if (copy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    argv[(*argc)++] = copy;
    argv[*argc] = NULL;
    return 0;
}

int qemu_build_args(const daemon_config_t *config,
                    const char *qmp_sock_path,
                    const char *virtiofsd_sock_path,
                    const char *pid_file_path,
                    char **argv,
                    size_t max_args) {
    if (config == NULL || qmp_sock_path == NULL || argv == NULL || max_args < 32) {
        errno = EINVAL;
        return -1;
    }

    size_t argc = 0;
    argv[0] = NULL;

    /* Binary name */
    if (append_arg(argv, &argc, max_args, "qemu-system-x86_64") != 0) goto fail;

    /* BIOS/firmware directory search */
    char exe_dir[WaddleMaxPathLen] = {0};
    ssize_t exelen = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (exelen > 0) {
        exe_dir[exelen] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
    }

    char candidate1[WaddleMaxPathLen + 64];
    char candidate2[WaddleMaxPathLen + 64];
    snprintf(candidate1, sizeof(candidate1), "%.800s/vendor/pc-bios", exe_dir);
    snprintf(candidate2, sizeof(candidate2), "%.800s/../submodules/qemu/pc-bios", exe_dir);

    const char *bios_candidates[] = {
        candidate1,
        candidate2,
        "build/vendor/pc-bios",
        "submodules/qemu/pc-bios",
        "/usr/share/seabios",
        "/usr/share/qemu",
        NULL
    };
    for (size_t b = 0; bios_candidates[b] != NULL; b++) {
        char test_file[WaddleMaxPathLen + 64];
        snprintf(test_file, sizeof(test_file), "%.900s/bios-256k.bin", bios_candidates[b]);
        if (access(test_file, R_OK) == 0) {
            if (append_arg(argv, &argc, max_args, "-L") != 0 ||
                append_arg(argv, &argc, max_args, bios_candidates[b]) != 0) goto fail;
            break;
        }
    }

    /* VM name */
    if (append_arg(argv, &argc, max_args, "-name") != 0 ||
        append_arg(argv, &argc, max_args, "waddle-lsw,debug-threads=on") != 0) goto fail;

    /* KVM acceleration check */
    if (access("/dev/kvm", R_OK | W_OK) == 0) {
        if (append_arg(argv, &argc, max_args, "-enable-kvm") != 0 ||
            append_arg(argv, &argc, max_args, "-cpu") != 0 ||
            append_arg(argv, &argc, max_args, "host") != 0) goto fail;
    } else {
        if (append_arg(argv, &argc, max_args, "-cpu") != 0 ||
            append_arg(argv, &argc, max_args, "max") != 0) goto fail;
    }

    /* SMP vCPUs */
    char vcpus_buf[32];
    snprintf(vcpus_buf, sizeof(vcpus_buf), "%u", config->vcpus);
    if (append_arg(argv, &argc, max_args, "-smp") != 0 ||
        append_arg(argv, &argc, max_args, vcpus_buf) != 0) goto fail;

    /* Memory configuration */
    char mem_buf[32];
    snprintf(mem_buf, sizeof(mem_buf), "%uM", config->memory_mb);
    if (append_arg(argv, &argc, max_args, "-m") != 0 ||
        append_arg(argv, &argc, max_args, mem_buf) != 0) goto fail;

    /* Memory backend for vhost-user shared memory */
    char mem_backend[256];
    snprintf(mem_backend, sizeof(mem_backend),
             "memory-backend-file,id=mem,size=%uM,mem-path=/dev/shm,share=on",
             config->memory_mb);
    if (append_arg(argv, &argc, max_args, "-object") != 0 ||
        append_arg(argv, &argc, max_args, mem_backend) != 0 ||
        append_arg(argv, &argc, max_args, "-numa") != 0 ||
        append_arg(argv, &argc, max_args, "node,memdev=mem") != 0) goto fail;

    /* VirtIO-FS vhost-user socket backend */
    if (virtiofsd_sock_path != NULL && virtiofsd_sock_path[0] != '\0') {
        char chardev_buf[WaddleMaxPathLen + 64];
        snprintf(chardev_buf, sizeof(chardev_buf),
                 "socket,id=char0,path=%s", virtiofsd_sock_path);
        if (append_arg(argv, &argc, max_args, "-chardev") != 0 ||
            append_arg(argv, &argc, max_args, chardev_buf) != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args, "vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=waddle_fs") != 0) goto fail;
    }

    /* VSOCK device */
    char vsock_buf[64];
    snprintf(vsock_buf, sizeof(vsock_buf), "vhost-vsock-pci,id=vsock0,guest-cid=%u", config->vsock_cid);
    if (append_arg(argv, &argc, max_args, "-device") != 0 ||
        append_arg(argv, &argc, max_args, vsock_buf) != 0) goto fail;

    /* QMP Monitor socket */
    char qmp_buf[WaddleMaxPathLen + 64];
    snprintf(qmp_buf, sizeof(qmp_buf), "socket,id=qmp0,path=%s,server=on,wait=off", qmp_sock_path);
    if (append_arg(argv, &argc, max_args, "-chardev") != 0 ||
        append_arg(argv, &argc, max_args, qmp_buf) != 0 ||
        append_arg(argv, &argc, max_args, "-mon") != 0 ||
        append_arg(argv, &argc, max_args, "chardev=qmp0,mode=control") != 0) goto fail;

    /* Base disk image */
    char drive_buf[WaddleMaxPathLen + 64];
    snprintf(drive_buf, sizeof(drive_buf), "file=%s,if=%s,cache=writeback", config->disk_image,
             config->av_enabled && config->av_uefi ? "ide" : "virtio");
    if (append_arg(argv, &argc, max_args, "-drive") != 0 ||
        append_arg(argv, &argc, max_args, drive_buf) != 0) goto fail;

    if (config->av_enabled && config->av_uefi) {
        char firmware_vars[WaddleMaxPathLen + 64];
        int length = snprintf(firmware_vars, sizeof(firmware_vars),
            "if=pflash,format=raw,file=%s.av_uefi.fd", config->disk_image);
        if (length <= 0 || (size_t)length >= sizeof(firmware_vars) ||
            append_arg(argv, &argc, max_args, "-machine") != 0 ||
            append_arg(argv, &argc, max_args, "q35") != 0 ||
            append_arg(argv, &argc, max_args, "-drive") != 0 ||
            append_arg(argv, &argc, max_args, "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd") != 0 ||
            append_arg(argv, &argc, max_args, "-drive") != 0 ||
            append_arg(argv, &argc, max_args, firmware_vars) != 0)
            goto fail;
    }

    /* AV devices are managed with the existing VM; no external AV VM assumed. */
    if (config->av_enabled) {
        if (daemon_config_validate(config) != 0) { errno = EINVAL; goto fail; }
        char av_backend[WaddleMaxPathLen + 128];
        int length = snprintf(av_backend, sizeof(av_backend),
            "memory-backend-file,id=waddle_av,size=%llu,mem-path=%s,share=on",
            (unsigned long long)AvMappingBytes, config->av_shm_path);
        if (length < 0 || (size_t)length >= sizeof(av_backend)) { errno = E2BIG; goto fail; }
        if (append_arg(argv, &argc, max_args, "-object") != 0 ||
            append_arg(argv, &argc, max_args, av_backend) != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args, "ivshmem-plain,memdev=waddle_av") != 0 ||
            append_arg(argv, &argc, max_args, "-audiodev") != 0 ||
            append_arg(argv, &argc, max_args, "none,id=waddle_silent") != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args, "ich9-intel-hda") != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args, "hda-duplex,audiodev=waddle_silent") != 0) goto fail;
        if (config->av_gpu_bdf[0]) {
            char gpu_device[64];
            snprintf(gpu_device, sizeof(gpu_device), "vfio-pci,host=%s", config->av_gpu_bdf);
            if (append_arg(argv, &argc, max_args, "-device") != 0 ||
                append_arg(argv, &argc, max_args, gpu_device) != 0 ||
                append_arg(argv, &argc, max_args, "-vga") != 0 ||
                append_arg(argv, &argc, max_args, "none") != 0) goto fail;
        }
    }

    /* Headless display */
    if (append_arg(argv, &argc, max_args, "-display") != 0 ||
        append_arg(argv, &argc, max_args, "none") != 0) goto fail;

    /* PID file */
    if (pid_file_path != NULL && pid_file_path[0] != '\0') {
        if (append_arg(argv, &argc, max_args, "-pidfile") != 0 ||
            append_arg(argv, &argc, max_args, pid_file_path) != 0) goto fail;
    }

    return (int)argc;

fail:
    qemu_free_args(argv, argc);
    return -1;
}

void qemu_free_args(char **argv, size_t argc) {
    if (argv == NULL) return;
    for (size_t i = 0; i < argc; i++) {
        if (argv[i] != NULL) {
            free(argv[i]);
            argv[i] = NULL;
        }
    }
}

int qemu_find_binary(char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap < 16) {
        errno = EINVAL;
        return -1;
    }

    char exe_dir[WaddleMaxPathLen];
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *last_slash = strrchr(exe_dir, '/');
        if (last_slash != NULL) {
            *last_slash = '\0';
        } else {
            exe_dir[0] = '.';
            exe_dir[1] = '\0';
        }

        /* Check relative vendor paths */
        char candidate[WaddleMaxPathLen + 64];
        snprintf(candidate, sizeof(candidate), "%s/vendor/qemu-system-x86_64", exe_dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidate);
            return 0;
        }

        snprintf(candidate, sizeof(candidate), "%s/../build/vendor/qemu-system-x86_64", exe_dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidate);
            return 0;
        }

        snprintf(candidate, sizeof(candidate), "%s/qemu-system-x86_64", exe_dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidate);
            return 0;
        }
    }

    /* Fallback to local build vendor directory */
    if (access("build/vendor/qemu-system-x86_64", X_OK) == 0) {
        snprintf(out_path, path_cap, "build/vendor/qemu-system-x86_64");
        return 0;
    }

    /* Standard known install locations */
    const char *candidates[] = {
        "/usr/bin/qemu-system-x86_64",
        "/usr/local/bin/qemu-system-x86_64",
        NULL
    };

    for (size_t i = 0; candidates[i] != NULL; i++) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidates[i]);
            return 0;
        }
    }

    /* Search in PATH */
    const char *path_env = getenv("PATH");
    if (path_env != NULL) {
        char *path_copy = strdup(path_env);
        if (path_copy != NULL) {
            char *token = strtok(path_copy, ":");
            while (token != NULL) {
                char candidate[WaddleMaxPathLen + 64];
                snprintf(candidate, sizeof(candidate), "%s/qemu-system-x86_64", token);
                if (access(candidate, X_OK) == 0) {
                    snprintf(out_path, path_cap, "%s", candidate);
                    free(path_copy);
                    return 0;
                }
                token = strtok(NULL, ":");
            }
            free(path_copy);
        }
    }

    errno = ENOENT;
    return -1;
}

int qemu_spawn(qemu_process_t *proc,
               const daemon_config_t *config,
               const char *qmp_sock_path,
               const char *virtiofsd_sock_path,
               const char *log_path,
               const char *qemu_binary) {
    if (proc == NULL || config == NULL || qmp_sock_path == NULL || log_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(proc, 0, sizeof(*proc));
    proc->log_fd = -1;
    proc->qmp.socket_fd = -1;

    strncpy(proc->qmp_sock_path, qmp_sock_path, sizeof(proc->qmp_sock_path) - 1);
    if (virtiofsd_sock_path != NULL) {
        strncpy(proc->virtiofsd_sock_path, virtiofsd_sock_path, sizeof(proc->virtiofsd_sock_path) - 1);
    }
    strncpy(proc->log_path, log_path, sizeof(proc->log_path) - 1);

    char *argv[QemuMaxArgs];
    memset(argv, 0, sizeof(argv));

    int argc = qemu_build_args(config, qmp_sock_path, virtiofsd_sock_path, NULL, argv, QemuMaxArgs);
    if (argc < 0) {
        return -1;
    }

    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        qemu_free_args(argv, (size_t)argc);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        close(log_fd);
        qemu_free_args(argv, (size_t)argc);
        errno = saved_errno;
        return -1;
    }

    if (pid == 0) {
        /* Child process: redirect standard streams to log file */
        if (dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(log_fd);

        char resolved_binary[WaddleMaxPathLen];
        const char *bin = NULL;
        if (qemu_binary != NULL && qemu_binary[0] != '\0') {
            bin = qemu_binary;
        } else if (qemu_find_binary(resolved_binary, sizeof(resolved_binary)) == 0) {
            bin = resolved_binary;
        } else {
            bin = argv[0];
        }

        execvp(bin, argv);
        _exit(127);
    }

    /* Parent process */
    proc->pid = pid;
    proc->log_fd = log_fd;
    proc->is_running = 1;

    qemu_free_args(argv, (size_t)argc);
    return 0;
}

int qemu_poll_status(qemu_process_t *proc) {
    if (proc == NULL || proc->pid <= 0) {
        if (proc != NULL) proc->is_running = 0;
        return 0;
    }

    int status = 0;
    pid_t r = waitpid(proc->pid, &status, WNOHANG);
    if (r == proc->pid) {
        proc->is_running = 0;
        proc->pid = 0;
        return 0;
    }
    if (r == 0) {
        return 1; /* Process still running */
    }
    if (r < 0 && errno == ECHILD) {
        proc->is_running = 0;
        proc->pid = 0;
        return 0;
    }

    return -1;
}

int qemu_shutdown_graceful(qemu_process_t *proc) {
    if (proc == NULL || proc->pid <= 0) {
        return 0;
    }

    /* Connect to QMP if not already connected */
    if (proc->qmp.socket_fd < 0) {
        if (qmp_connect(&proc->qmp, proc->qmp_sock_path, 1000) != 0) {
            return -1;
        }
        if (qmp_negotiate_capabilities(&proc->qmp) != 0) {
            qmp_close(&proc->qmp);
            return -1;
        }
    }

    return qmp_system_powerdown(&proc->qmp);
}

int qemu_kill(qemu_process_t *proc) {
    if (proc == NULL) return -1;

    if (proc->pid > 0) {
        kill(proc->pid, SIGKILL);
        waitpid(proc->pid, NULL, 0);
        proc->pid = 0;
        proc->is_running = 0;
    }

    qmp_close(&proc->qmp);
    return 0;
}

void qemu_cleanup(qemu_process_t *proc) {
    if (proc == NULL) return;

    qmp_close(&proc->qmp);

    if (proc->log_fd >= 0) {
        close(proc->log_fd);
        proc->log_fd = -1;
    }

    if (proc->pid > 0) {
        waitpid(proc->pid, NULL, WNOHANG);
    }

    proc->is_running = 0;
}
