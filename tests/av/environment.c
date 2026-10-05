#include "av_environment.h"
#include "av_layout.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char root[] = "/tmp/waddle-av-environment-XXXXXX";
    assert(mkdtemp(root));
    daemon_config_t config;
    daemon_config_init_defaults(&config);
    config.av_enabled = 1;
    snprintf(config.av_shm_path, sizeof(config.av_shm_path), "%s/shared", root);
    av_environment_t environment = {.fd = -1};
    char error[256];
    assert(av_environment_prepare(&environment, &config, error, sizeof(error)) == 0);
    assert(environment.length == AvMappingBytes);
    assert(av_layout_validate(environment.mapping, environment.length) == 0);
    struct stat status;
    assert(fstat(environment.fd, &status) == 0);
    assert((status.st_mode & 0777) == 0600);
    av_environment_t second = {.fd = -1};
    assert(av_environment_prepare(&second, &config, error, sizeof(error)) == -1);
    assert(errno == EEXIST);
    assert(strstr(error, "preserved"));
    assert(av_layout_validate(environment.mapping, environment.length) == 0);
    av_environment_free(&second);
    assert(access(config.av_shm_path, F_OK) == 0);
    av_environment_free(&environment);
    assert(access(config.av_shm_path, F_OK) != 0);
    av_environment_free(&environment);
    assert(symlink("/dev/null", config.av_shm_path) == 0);
    assert(av_environment_prepare(&environment, &config, error, sizeof(error)) == -1);
    assert(lstat(config.av_shm_path, &status) == 0 && S_ISLNK(status.st_mode));
    unlink(config.av_shm_path);
    if (access("/usr/share/OVMF/OVMF_CODE_4M.fd", R_OK) == 0) {
        config.av_uefi = 1;
        snprintf(config.disk_image, sizeof(config.disk_image), "%s/windows.qcow2", root);
        char firmware[1200];
        snprintf(firmware, sizeof(firmware), "%s.av_uefi.fd", config.disk_image);
        assert(av_environment_prepare(&environment, &config, error, sizeof(error)) == 0);
        av_environment_free(&environment);
        assert(stat(firmware, &status) == 0 && (status.st_mode & 0777) == 0600);
        ino_t original = status.st_ino;
        assert(av_environment_prepare(&environment, &config, error, sizeof(error)) == 0);
        av_environment_free(&environment);
        assert(stat(firmware, &status) == 0 && status.st_ino == original);
        assert(unlink(firmware) == 0 && symlink("/dev/null", firmware) == 0);
        assert(av_environment_prepare(&environment, &config, error, sizeof(error)) == -1);
        assert(lstat(firmware, &status) == 0 && S_ISLNK(status.st_mode));
        unlink(firmware);
    }
    rmdir(root);
    puts("AV environment: automatic sparse mapping, private permissions, collision/symlink "
         "preservation passed");
    return 0;
}
