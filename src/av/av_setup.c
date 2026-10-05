/** @file av_setup.c @brief Bundled KVMFR build/load helper, no shell execution. */
#include "av_layout.h"
#include "av_gpu.h"
#include "waddle/av_protocol.h"
#include "av_kvmfr.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(char *const arguments[]) {
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execvp(arguments[0], arguments);
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static int module_build(char *module_path, size_t capacity) {
    char executable[PATH_MAX], source[PATH_MAX], make_path[PATH_MAX + 2];
    struct utsname system;
    if (uname(&system) != 0)
        return -1;
    ssize_t count = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (count < 0)
        return -1;
    executable[count] = '\0';
    char *slash = strrchr(executable, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    if (snprintf(source, sizeof(source), "%s/vendor/av/module", executable) >= (int)sizeof(source))
        return -1;
    if (access(source, R_OK | W_OK | X_OK) != 0) {
        fprintf(stderr, "AV setup: bundled pinned module sources are missing or unwritable\n");
        return -1;
    }
    char kernel[PATH_MAX];
    if (snprintf(kernel, sizeof(kernel), "/lib/modules/%s/build", system.release) >=
            (int)sizeof(kernel) ||
        access(kernel, R_OK | X_OK) != 0) {
        fprintf(stderr, "AV setup: install build headers for running kernel %s\n", system.release);
        return -1;
    }
    snprintf(make_path, sizeof(make_path), "M=%s", source);
    char *arguments[] = {"make", "-C", kernel, make_path, "modules", NULL};
    if (run(arguments) != 0)
        return -1;
    if (snprintf(module_path, capacity, "%s/kvmfr.ko", source) >= (int)capacity)
        return -1;
    return 0;
}
/* Extend access for the explicit invoking user without replacing live ownership. */
static int grant_invoking_user(const char *path) {
    const char *owner_text = getenv("SUDO_UID");
    uint32_t owner = 0;
    if (!owner_text || av_number_parse(owner_text, strlen(owner_text), &owner) != 0) {
        fputs("AV setup: invoke access setup through sudo as the intended user\n", stderr);
        return -1;
    }
    char grant[48];
    snprintf(grant, sizeof(grant), "u:%u:rw", owner);
    char *arguments[] = {"/usr/bin/setfacl", "-m", grant, (char *)path, NULL};
    if (run(arguments) != 0) {
        fputs("AV setup: install the base-system acl package and retry access setup\n", stderr);
        return -1;
    }
    return 0;
}
static int prepare_host(void) {
    if (geteuid() != 0) {
        fputs("AV setup: host device access setup requires administrator privileges\n", stderr);
        return 1;
    }
    const char *paths[] = {"/dev/kvm", "/dev/vhost-vsock"};
    for (unsigned i = 0; i < 2; ++i) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        struct stat status;
        if (fd < 0 || fstat(fd, &status) != 0 || !S_ISCHR(status.st_mode)) {
            if (fd >= 0) close(fd);
            fprintf(stderr, "AV setup: required kernel device %s is unavailable\n", paths[i]);
            return 1;
        }
        close(fd);
        if (grant_invoking_user(paths[i]) != 0) return 1;
    }
    puts("AV setup: KVM and VSOCK access granted to invoking user");
    return 0;
}
/** @brief Build or load the bundled pinned kernel module for this running kernel.
 * @param[in] argc CRT argument count. @param[in] argv Borrowed NUL-terminated args.
 * @return 0 help/success, 2 invalid command, 1 build/load/capability failure.
 * @note Owns/reaps child build process and closes probe FD; --load-module requires
 * root. Does not unload a live module, alter GPUs, enroll keys or reset mapping.
 */
int main(int argc, char **argv) {
    if ((argc == 3 && !strcmp(argv[1], "--probe-gpu")) ||
        ((argc == 3 || argc == 4) && !strcmp(argv[1], "--probe-mdev"))) {
        char error[512];
        if (av_gpu_mdev_probe("/sys", !strcmp(argv[1], "--probe-gpu") ? argv[2] : (argc == 4 ? argv[3] : ""),
                              !strcmp(argv[1], "--probe-mdev") ? argv[2] : "", error, sizeof(error)) != 0) {
            fprintf(stderr, "AV capability failure: %s\n", error);
            return 1;
        }
        puts("AV mdev capability ready; existing host driver preserved");
        return 0;
    }
    if (argc != 2 || !strcmp(argv[1], "--help")) {
        puts("waddle-av-setup --build-module | --load-module | --prepare-host | --probe-gpu PCI_BDF | --probe-mdev UUID [PCI_BDF]\n"
             "Builds pinned KVMFR against the running kernel; load requires administrator "
             "privileges.");
        return argc == 2 ? 0 : 2;
    }
    if (!strcmp(argv[1], "--prepare-host"))
        return prepare_host();
    int load = !strcmp(argv[1], "--load-module");
    if (!load && strcmp(argv[1], "--build-module"))
        return 2;
    if (load && geteuid() != 0) {
        fprintf(stderr, "AV setup: --load-module requires administrator privileges\n");
        return 1;
    }
    if (load) {
        int fd = open("/dev/kvmfr0", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            long size = av_kvmfr_size(fd);
            off_t seek_size = lseek(fd, 0, SEEK_END);
            close(fd);
            if (size == (long)AvMappingBytes && seek_size == (off_t)AvMappingBytes)
                return grant_invoking_user("/dev/kvmfr0") == 0 ? 0 : 1;
            fprintf(stderr, "AV setup: existing KVMFR device has incompatible size; preserved\n");
            return 1;
        }
    }
    char module_path[PATH_MAX];
    if (module_build(module_path, sizeof(module_path)) != 0)
        return 1;
    if (!load)
        return 0;
    char *arguments[] = {"/sbin/insmod", module_path, "static_size_mb=2048", NULL};
    if (run(arguments) != 0) {
        fprintf(stderr,
                "AV setup: kernel module load rejected; inspect kernel/signature diagnostics\n");
        return 1;
    }
    int fd = open("/dev/kvmfr0", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return 1;
    long size = av_kvmfr_size(fd);
    int access_status = 0;
    const char *owner_text = getenv("SUDO_UID"), *group_text = getenv("SUDO_GID");
    if (owner_text || group_text) {
        uint32_t owner = 0, group = 0;
        if (!owner_text || !group_text ||
            av_number_parse(owner_text, strlen(owner_text), &owner) != 0 ||
            (strcmp(group_text, "0") && av_number_parse(group_text, strlen(group_text), &group) != 0)) {
            errno = EINVAL;
            access_status = -1;
        } else {
            access_status = fchown(fd, (uid_t)owner, (gid_t)group);
        }
    }
    if (!access_status)
        access_status = fchmod(fd, 0600);
    close(fd);
    if (size != (long)AvMappingBytes || access_status != 0) {
        fprintf(stderr, "AV setup: device size/access verification failed: %s\n", strerror(errno));
        return 1;
    }
    puts("AV setup: exact-size KVMFR device ready with private access for invoking user");
    return 0;
}
