/**
 * @file daemon_qemu.c
 * @brief Implementation of QEMU hypervisor process manager and argument builder.
 */

#include "daemon_qemu.h"
#include "daemon_device.h"
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

/** @brief Frozen local CUPS socket forwarded into user-mode guest networking. */
static const char CupsSocketPath[] = "/run/cups/cups.sock";

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

/** @brief Check a nullable path is NUL-terminated within the public path bound. */
static int qemu_path_valid(const char *path, int required) {
    if (path == NULL) return !required;
    size_t length = strnlen(path, WaddleMaxPathLen);
    return length < WaddleMaxPathLen &&
        (!required || (length != 0 && path[0] == '/'));
}

/** @brief Check whether a bounded path is safe as an unescaped QEMU keyval value. */
static int qemu_keyval_path_valid(const char *path, int required) {
    if (!qemu_path_valid(path, required)) return 0;
    if (path == NULL) return !required;
    for (size_t index = 0; path[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)path[index];
        if (character == ',' || character < 0x20 || character == 0x7f) return 0;
    }
    return 1;
}

int qemu_build_args(const daemon_config_t *config,
                    const char *qmp_sock_path,
                    const char *virtiofsd_sock_path,
                    const char *pid_file_path,
                    char **argv,
                    size_t max_args) {
    return qemu_build_args_for_device(config, "default", qmp_sock_path,
                                      virtiofsd_sock_path, pid_file_path,
                                      argv, max_args);
}

static int qemu_build_args_internal(const daemon_config_t *config,
                                    const char *device_name,
                                    const char *qmp_sock_path,
                                    const char *virtiofsd_sock_path,
                                    const qemu_virtiofs_mount_t *mounts,
                                    size_t mount_count,
                                    const char *pid_file_path,
                                    char **argv,
                                    size_t max_args);

/** @brief Validate the same bounded ASCII device identifier as daemon registry. */
static int qemu_device_name_valid(const char *device_name) {
    if (device_name == NULL) return 0;
    size_t length = strnlen(device_name, WaddleMaxDeviceNameLen);
    if (length == 0 || length >= WaddleMaxDeviceNameLen) return 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)device_name[index];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9')) &&
            character != '-' && character != '_') return 0;
    }
    return 1;
}

int qemu_build_args_for_device(const daemon_config_t *config,
                               const char *device_name,
                               const char *qmp_sock_path,
                               const char *virtiofsd_sock_path,
                               const char *pid_file_path,
                               char **argv,
                               size_t max_args) {
    return qemu_build_args_internal(config, device_name, qmp_sock_path,
                                    virtiofsd_sock_path, NULL, 0,
                                    pid_file_path, argv, max_args);
}

/** @brief Validate and normalize the drive identity in one multi-mount entry. */
static int qemu_mount_drive_letter(const qemu_virtiofs_mount_t *mount,
                                   char *drive_letter) {
    if (mount == NULL || drive_letter == NULL) return 0;
    if (mount->guest_drive[1] != ':' ||
        (mount->guest_drive[2] != '\\' && mount->guest_drive[2] != '/') ||
        mount->guest_drive[3] != '\0') return 0;
    unsigned char drive = (unsigned char)mount->guest_drive[0];
    if (drive >= 'A' && drive <= 'Z') drive = (unsigned char)(drive - 'A' + 'a');
    if (drive < 'a' || drive > 'z') return 0;
    *drive_letter = (char)drive;
    return 1;
}

/** @brief Validate all multi-mount inputs before the builder allocates argv. */
static int qemu_mounts_valid(const qemu_virtiofs_mount_t *mounts,
                             size_t mount_count) {
    if (mount_count > WaddleMaxMounts ||
        ((mount_count == 0) != (mounts == NULL))) return 0;
    for (size_t index = 0; index < mount_count; ++index) {
        char drive_letter;
        if (!qemu_mount_drive_letter(&mounts[index], &drive_letter) ||
            !qemu_keyval_path_valid(mounts[index].socket_path, 1) ||
            mounts[index].socket_path[0] != '/') return 0;
        for (size_t prior = 0; prior < index; ++prior) {
            char prior_drive_letter;
            if (!qemu_mount_drive_letter(&mounts[prior], &prior_drive_letter) ||
                drive_letter == prior_drive_letter ||
                strcmp(mounts[index].socket_path,
                       mounts[prior].socket_path) == 0) return 0;
        }
    }
    return 1;
}

/** @brief Append one drive-derived QEMU chardev/device argument group. */
static int qemu_append_mount_args(char **argv, size_t *argc, size_t max_args,
                                  const qemu_virtiofs_mount_t *mount) {
    char drive_letter;
    if (!qemu_mount_drive_letter(mount, &drive_letter)) {
        errno = EINVAL;
        return -1;
    }
    char chardev_id[32];
    char device_id[32];
    char chardev_buf[WaddleMaxPathLen + 64];
    char device_buf[160];
    int id_length = snprintf(chardev_id, sizeof(chardev_id),
                             "waddle_fs_char_%c", drive_letter);
    int device_id_length = snprintf(device_id, sizeof(device_id),
                                    "waddle_fs_dev_%c", drive_letter);
    int chardev_length = snprintf(chardev_buf, sizeof(chardev_buf),
                                  "socket,id=%s,path=%s", chardev_id,
                                  mount->socket_path);
    int device_length = snprintf(device_buf, sizeof(device_buf),
        "vhost-user-fs-pci,id=%s,queue-size=1024,chardev=%s,tag=waddle_fs_%c",
        device_id, chardev_id, drive_letter);
    if (id_length < 0 || (size_t)id_length >= sizeof(chardev_id) ||
        device_id_length < 0 || (size_t)device_id_length >= sizeof(device_id) ||
        chardev_length < 0 || (size_t)chardev_length >= sizeof(chardev_buf) ||
        device_length < 0 || (size_t)device_length >= sizeof(device_buf)) {
        errno = E2BIG;
        return -1;
    }
    if (append_arg(argv, argc, max_args, "-chardev") != 0 ||
        append_arg(argv, argc, max_args, chardev_buf) != 0 ||
        append_arg(argv, argc, max_args, "-device") != 0 ||
        append_arg(argv, argc, max_args, device_buf) != 0) return -1;
    return 0;
}

static int qemu_build_args_internal(const daemon_config_t *config,
                                    const char *device_name,
                                    const char *qmp_sock_path,
                                    const char *virtiofsd_sock_path,
                                    const qemu_virtiofs_mount_t *mounts,
                                    size_t mount_count,
                                    const char *pid_file_path,
                                    char **argv,
                                    size_t max_args) {
    if (argv != NULL && max_args != 0) argv[0] = NULL;
    if (config == NULL || !qemu_device_name_valid(device_name) ||
        qmp_sock_path == NULL || argv == NULL || max_args < 32 ||
        !qemu_keyval_path_valid(qmp_sock_path, 1) ||
        !qemu_keyval_path_valid(virtiofsd_sock_path, 0) ||
        !qemu_mounts_valid(mounts, mount_count) ||
        !qemu_path_valid(pid_file_path, 0) || daemon_config_validate(config) != 0) {
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
        int chardev_length = snprintf(chardev_buf, sizeof(chardev_buf),
                                      "socket,id=char0,path=%s", virtiofsd_sock_path);
        if (chardev_length < 0 || (size_t)chardev_length >= sizeof(chardev_buf)) {
            errno = E2BIG;
            goto fail;
        }
        if (append_arg(argv, &argc, max_args, "-chardev") != 0 ||
            append_arg(argv, &argc, max_args, chardev_buf) != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args, "vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=waddle_fs") != 0) goto fail;
    }
    for (size_t mount_index = 0; mount_index < mount_count; ++mount_index) {
        if (qemu_append_mount_args(argv, &argc, max_args, &mounts[mount_index]) != 0)
            goto fail;
    }

    /* VSOCK device */
    char vsock_buf[64];
    snprintf(vsock_buf, sizeof(vsock_buf), "vhost-vsock-pci,id=vsock0,guest-cid=%u", config->vsock_cid);
    if (append_arg(argv, &argc, max_args, "-device") != 0 ||
        append_arg(argv, &argc, max_args, vsock_buf) != 0) goto fail;

    /* Explicit user networking, optional CUPS forwarding, or no NIC. */
    if (config->network_mode == NetworkModeNone) {
        if (append_arg(argv, &argc, max_args, "-nic") != 0 ||
            append_arg(argv, &argc, max_args, "none") != 0) goto fail;
    } else {
        struct stat cups_stat;
        int cups_available = config->network_cups_forward &&
            lstat(CupsSocketPath, &cups_stat) == 0 && S_ISSOCK(cups_stat.st_mode);
        char netdev_buf[256];
        int netdev_length;
        if (cups_available) {
            char cups_chardev[WaddleMaxPathLen + 64];
            int cups_length = snprintf(cups_chardev, sizeof(cups_chardev),
                "socket,id=waddle_cups,path=%s", CupsSocketPath);
            if (cups_length < 0 || (size_t)cups_length >= sizeof(cups_chardev)) {
                errno = E2BIG; goto fail;
            }
            if (append_arg(argv, &argc, max_args, "-chardev") != 0 ||
                append_arg(argv, &argc, max_args, cups_chardev) != 0) goto fail;
            netdev_length = snprintf(netdev_buf, sizeof(netdev_buf),
                "user,id=waddle_net,guestfwd=tcp:10.0.2.100:631-chardev:waddle_cups");
        } else {
            netdev_length = snprintf(netdev_buf, sizeof(netdev_buf), "user,id=waddle_net");
        }
        if (netdev_length < 0 || (size_t)netdev_length >= sizeof(netdev_buf) ||
            append_arg(argv, &argc, max_args, "-netdev") != 0 ||
            append_arg(argv, &argc, max_args, netdev_buf) != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args, "virtio-net-pci,netdev=waddle_net") != 0) goto fail;
    }

    /* USB hotplug and automatic selectors use this stable QMP bus when enabled. */
    if (config->usb_controller &&
        (append_arg(argv, &argc, max_args, "-device") != 0 ||
         append_arg(argv, &argc, max_args, "qemu-xhci,id=waddle_xhci") != 0)) goto fail;

    /* One shared HDA endpoint: PipeWire capture/playback or a silent backend. */
    char audio_backend[256];
    int audio_length;
    if (config->audio_backend == AudioBackendPipewire) {
        audio_length = snprintf(audio_backend, sizeof(audio_backend),
            config->audio_microphone
                ? "pipewire,id=waddle_audio,out.name=waddle-discard.%s"
                : "pipewire,id=waddle_audio,out.name=waddle-discard.%s,in.voices=0",
            device_name);
    } else {
        audio_length = snprintf(audio_backend, sizeof(audio_backend), "none,id=waddle_audio");
    }
    if (audio_length < 0 || (size_t)audio_length >= sizeof(audio_backend) ||
        append_arg(argv, &argc, max_args, "-audiodev") != 0 ||
        append_arg(argv, &argc, max_args, audio_backend) != 0 ||
        append_arg(argv, &argc, max_args, "-device") != 0 ||
        append_arg(argv, &argc, max_args, "ich9-intel-hda") != 0 ||
        append_arg(argv, &argc, max_args, "-device") != 0 ||
        append_arg(argv, &argc, max_args, "hda-duplex,audiodev=waddle_audio") != 0) goto fail;

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
            append_arg(argv, &argc, max_args, "-boot") != 0 ||
            append_arg(argv, &argc, max_args, "order=c,strict=on") != 0 ||
            append_arg(argv, &argc, max_args, "-drive") != 0 ||
            append_arg(argv, &argc, max_args, "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd") != 0 ||
            append_arg(argv, &argc, max_args, "-drive") != 0 ||
            append_arg(argv, &argc, max_args, firmware_vars) != 0)
            goto fail;
    }

    /* AV devices are managed with the existing VM; no external AV VM assumed. */
    if (config->av_enabled) {
        char av_backend[WaddleMaxPathLen + 128];
        int length = snprintf(av_backend, sizeof(av_backend),
            "memory-backend-file,id=waddle_av,size=%llu,mem-path=%s,share=on",
            (unsigned long long)AvMappingBytes, config->av_shm_path);
        if (length < 0 || (size_t)length >= sizeof(av_backend)) { errno = E2BIG; goto fail; }
        if (append_arg(argv, &argc, max_args, "-object") != 0 ||
            append_arg(argv, &argc, max_args, av_backend) != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args, "ivshmem-plain,memdev=waddle_av") != 0) goto fail;
        /* A parent address is a capability constraint, never physical assignment. */
        if (config->av_gpu_bdf[0] && !config->av_gpu_mdev_uuid[0]) {
            errno = EINVAL; goto fail;
        }
        if (config->av_gpu_mdev_uuid[0]) {
            char gpu_device[128];
            int gpu_length = snprintf(gpu_device, sizeof(gpu_device),
                "vfio-pci,sysfsdev=/sys/bus/mdev/devices/%s,display=off",
                config->av_gpu_mdev_uuid);
            if (gpu_length < 0 || (size_t)gpu_length >= sizeof(gpu_device)) {
                errno = E2BIG; goto fail;
            }
            if (append_arg(argv, &argc, max_args, "-device") != 0 ||
                append_arg(argv, &argc, max_args, gpu_device) != 0) goto fail;
        }
        /* Keep a boot display until the signed guest VDD is configured. EDID
         * advertises a mode but does not certify 144-Hz composition/capture. */
        if (append_arg(argv, &argc, max_args, "-vga") != 0 ||
            append_arg(argv, &argc, max_args, "none") != 0 ||
            append_arg(argv, &argc, max_args, "-device") != 0 ||
            append_arg(argv, &argc, max_args,
                       "VGA,refresh_rate=144,xres=1920,yres=1080,vgamem_mb=64") != 0) goto fail;
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

int qemu_build_args_for_device_mounts(const daemon_config_t *config,
                                      const char *device_name,
                                      const char *qmp_sock_path,
                                      const qemu_virtiofs_mount_t *mounts,
                                      size_t mount_count,
                                      const char *pid_file_path,
                                      char **argv,
                                      size_t max_args) {
    return qemu_build_args_internal(config, device_name, qmp_sock_path,
                                    NULL, mounts, mount_count, pid_file_path,
                                    argv, max_args);
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

static int qemu_spawn_internal(qemu_process_t *proc,
                               const daemon_config_t *config,
                               const char *device_name,
                               const char *qmp_sock_path,
                               const char *virtiofsd_sock_path,
                               const qemu_virtiofs_mount_t *mounts,
                               size_t mount_count,
                               const char *log_path,
                               const char *qemu_binary);

int qemu_spawn(qemu_process_t *proc,
               const daemon_config_t *config,
               const char *qmp_sock_path,
               const char *virtiofsd_sock_path,
               const char *log_path,
               const char *qemu_binary) {
    return qemu_spawn_for_device(proc, config, "default", qmp_sock_path,
                                 virtiofsd_sock_path, log_path, qemu_binary);
}

int qemu_spawn_for_device(qemu_process_t *proc,
                          const daemon_config_t *config,
                          const char *device_name,
                          const char *qmp_sock_path,
                          const char *virtiofsd_sock_path,
                          const char *log_path,
                          const char *qemu_binary) {
    return qemu_spawn_internal(proc, config, device_name, qmp_sock_path,
                               virtiofsd_sock_path, NULL, 0, log_path,
                               qemu_binary);
}

int qemu_spawn_for_device_mounts(qemu_process_t *proc,
                                 const daemon_config_t *config,
                                 const char *device_name,
                                 const char *qmp_sock_path,
                                 const qemu_virtiofs_mount_t *mounts,
                                 size_t mount_count,
                                 const char *log_path,
                                 const char *qemu_binary) {
    return qemu_spawn_internal(proc, config, device_name, qmp_sock_path,
                               NULL, mounts, mount_count, log_path,
                               qemu_binary);
}

static int qemu_spawn_internal(qemu_process_t *proc,
                               const daemon_config_t *config,
                               const char *device_name,
                               const char *qmp_sock_path,
                               const char *virtiofsd_sock_path,
                               const qemu_virtiofs_mount_t *mounts,
                               size_t mount_count,
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

    int argc = qemu_build_args_internal(config, device_name, qmp_sock_path,
                                        virtiofsd_sock_path, mounts, mount_count,
                                        NULL, argv, QemuMaxArgs);
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
    if (proc == NULL) { errno = EINVAL; return -1; }
    if (proc->pid > 0) {
        int signal_result;
        do { signal_result = kill(proc->pid, SIGKILL); } while (signal_result < 0 && errno == EINTR);
        if (signal_result < 0 && errno != ESRCH) return -1;
        pid_t waited;
        do { waited = waitpid(proc->pid, NULL, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0 && errno != ECHILD) return -1;
        if (waited == 0) { errno = EBUSY; return -1; }
        proc->pid = 0;
        proc->is_running = 0;
    }
    qmp_close(&proc->qmp);
    return 0;
}

void qemu_cleanup(qemu_process_t *proc) {
    if (proc == NULL) return;

    /* Cleanup may not detach a child whose liveness is still unknown. */
    if (proc->pid > 0) {
        int status = qemu_poll_status(proc);
        if (status != 0) { if (status > 0) errno = EBUSY; return; }
    }
    qmp_close(&proc->qmp);

    if (proc->log_fd >= 0) {
        close(proc->log_fd);
        proc->log_fd = -1;
    }

    proc->is_running = 0;
}
