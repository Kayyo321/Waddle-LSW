/**
 * @file test_daemon_config.c
 * @brief C unit tests verifying daemon configuration loading and C ABI interop.
 */

#include "daemon_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_defaults(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    assert(cfg.memory_mb == 4096);
    assert(cfg.vcpus == 4);
    assert(cfg.vsock_cid == 3);
    assert(cfg.vsock_port == 5242);
    assert(cfg.start_timeout_sec == 60);
    assert(cfg.stop_timeout_sec == 15);
    assert(strcmp(cfg.default_shell, "powershell.exe") == 0);
    assert(cfg.mount_count == 1);
    assert(strcmp(cfg.mounts[0].guest_drive, "Z:\\") == 0);
    assert(cfg.mounts[0].read_only == 0);
    assert(daemon_config_validate(&cfg) == 0);
}

static void test_parse_string(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    const char *ini =
        "[subsystem]\n"
        "memory_mb = 2048\n"
        "vcpus = 2\n"
        "vsock_cid = 5\n"
        "vsock_port = 6000\n"
        "default_shell = cmd.exe\n"
        "disk_image = /tmp/test.qcow2\n"
        "\n"
        "[filesystem]\n"
        "export_1 = /var/test:Y:\\:ro\n"
        "\n"
        "[timeouts]\n"
        "start_timeout = 45\n"
        "stop_timeout = 10\n";

    assert(daemon_config_parse_string(&cfg, ini, strlen(ini)) == 0);
    assert(cfg.memory_mb == 2048);
    assert(cfg.vcpus == 2);
    assert(cfg.vsock_cid == 5);
    assert(cfg.vsock_port == 6000);
    assert(strcmp(cfg.default_shell, "cmd.exe") == 0);
    assert(strcmp(cfg.disk_image, "/tmp/test.qcow2") == 0);
    assert(cfg.mount_count == 1);
    assert(strcmp(cfg.mounts[0].host_path, "/var/test") == 0);
    assert(strcmp(cfg.mounts[0].guest_drive, "Y:\\") == 0);
    assert(cfg.mounts[0].read_only == 1);
    assert(cfg.start_timeout_sec == 45);
    assert(cfg.stop_timeout_sec == 10);
    assert(daemon_config_validate(&cfg) == 0);
}

static void test_export_parser(void) {
    waddle_daemon_fs_mount_t mount;
    memset(&mount, 0, sizeof(mount));

    assert(daemon_config_parse_export("/home/user:Z:\\:rw", &mount) == 0);
    assert(strcmp(mount.host_path, "/home/user") == 0);
    assert(strcmp(mount.guest_drive, "Z:\\") == 0);
    assert(mount.read_only == 0);

    assert(daemon_config_parse_export("/data:X:\\:ro", &mount) == 0);
    assert(strcmp(mount.host_path, "/data") == 0);
    assert(strcmp(mount.guest_drive, "X:\\") == 0);
    assert(mount.read_only == 1);

    /* Missing mode defaults to rw */
    assert(daemon_config_parse_export("/data:X:\\", &mount) == 0);
    assert(mount.read_only == 0);

    /* Invalid export formats */
    assert(daemon_config_parse_export(NULL, &mount) == -1);
    assert(daemon_config_parse_export("relative:Z:\\", &mount) == -1);
    assert(daemon_config_parse_export("/home:bad", &mount) == -1);
    assert(daemon_config_parse_export("/home:Z:\\:unknown", &mount) == -1);
}

static void test_file_loading(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    /* Missing file is non-fatal: returns 0 with defaults preserved */
    assert(daemon_config_load_file(&cfg, "/nonexistent/waddle/config.ini") == 0);
    assert(cfg.memory_mb == 4096);

    /* Create temporary config file */
    char tmp_template[] = "/tmp/waddle_test_cfg_XXXXXX";
    int fd = mkstemp(tmp_template);
    assert(fd >= 0);

    const char *content =
        "[subsystem]\n"
        "memory_mb = 16384\n"
        "vcpus = 8\n";
    ssize_t written = write(fd, content, strlen(content));
    assert(written == (ssize_t)strlen(content));
    close(fd);

    assert(daemon_config_load_file(&cfg, tmp_template) == 0);
    assert(cfg.memory_mb == 16384);
    assert(cfg.vcpus == 8);

    unlink(tmp_template);
}

static void test_validation_ranges(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    /* Test null check */
    assert(daemon_config_validate(NULL) == -1);

    /* Memory out of range */
    cfg.memory_mb = 128;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.memory_mb = 131072;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.memory_mb = 4096;

    /* VCPUs out of range */
    cfg.vcpus = 0;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vcpus = 256;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vcpus = 4;

    /* CID out of range */
    cfg.vsock_cid = 0;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vsock_cid = 2;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vsock_cid = 3;

    /* Timeout out of range */
    cfg.start_timeout_sec = 0;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.start_timeout_sec = 60;

    assert(daemon_config_validate(&cfg) == 0);
}

static void test_seamless_settings(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);
    assert(cfg.network_mode == NetworkModeUser);
    assert(cfg.network_cups_forward == 1);
    assert(cfg.usb_controller == 1);
    assert(cfg.usb_auto_attach_count == 0);
    assert(cfg.audio_backend == AudioBackendPipewire);
    assert(cfg.audio_microphone == 1 && cfg.audio_per_app == 1);
    assert(cfg.input_ime == InputImeGuest);
    assert(cfg.input_inhibit_shortcuts == InputInhibitShortcutsFullscreen);
    assert(cfg.input_repeat == InputRepeatHost);
    assert(cfg.clipboard_enabled == 1 && cfg.clipboard_max_mb == 64 && cfg.clipboard_files == 1);
    assert(cfg.integration_start_at_login == 0);
    assert(cfg.integration_menu_apps == IntegrationMenuAppsInstalled);
    assert(cfg.integration_desktop_shortcuts == IntegrationDesktopShortcutsMirror);
    assert(cfg.integration_exe_handler == IntegrationExeHandlerWaddle);
    assert(cfg.integration_uac == IntegrationUacWindow && cfg.integration_notify == 1);

    const char *ini =
        "[network]\nmode=none\ncups_forward=true\n"
        "[usb]\ncontroller=false\nauto_attach=046D:C534,1234:ABCD\n"
        "[audio]\nbackend=none\nmicrophone=0\nper_app=false\n"
        "[input]\nime=host\ninhibit_shortcuts=never\nrepeat=off\n"
        "[clipboard]\nenabled=false\nmax_mb=1\nfiles=0\n"
        "[integration]\nstart_at_login=1\nmenu_apps=all\ndesktop_shortcuts=off\n"
        "exe_handler=off\nuac=auto\nnotify=false\n";
    assert(daemon_config_parse_string(&cfg, ini, strlen(ini)) == 0);
    assert(cfg.network_mode == NetworkModeNone && cfg.network_cups_forward == 1);
    assert(cfg.usb_controller == 0 && cfg.usb_auto_attach_count == 2);
    assert(cfg.usb_auto_attach[0].vendor_id == 0x046d && cfg.usb_auto_attach[0].product_id == 0xc534);
    assert(cfg.usb_auto_attach[1].vendor_id == 0x1234 && cfg.usb_auto_attach[1].product_id == 0xabcd);
    assert(cfg.audio_backend == AudioBackendNone && cfg.audio_microphone == 0 && cfg.audio_per_app == 0);
    assert(cfg.input_ime == InputImeHost && cfg.input_inhibit_shortcuts == InputInhibitShortcutsNever);
    assert(cfg.input_repeat == InputRepeatOff);
    assert(cfg.clipboard_enabled == 0 && cfg.clipboard_max_mb == 1 && cfg.clipboard_files == 0);
    assert(cfg.integration_start_at_login == 1 && cfg.integration_menu_apps == IntegrationMenuAppsAll);
    assert(cfg.integration_desktop_shortcuts == IntegrationDesktopShortcutsOff);
    assert(cfg.integration_exe_handler == IntegrationExeHandlerOff);
    assert(cfg.integration_uac == IntegrationUacAuto && cfg.integration_notify == 0);

    daemon_config_t before = cfg;
    const char *invalid[] = {
        "[network]\nmode=bridge\n", "[network]\ncups_forward=yes\n", "[network]\nunknown=1\n",
        "[usb]\ncontroller=2\n", "[usb]\nauto_attach=1234:5678,1234:5678\n", "[usb]\nunknown=1\n",
        "[audio]\nbackend=alsa\n", "[audio]\nper_app=TRUE\n", "[audio]\nunknown=1\n",
        "[input]\nime=system\n", "[input]\ninhibit_shortcuts=always\n", "[input]\nrepeat=guest\n",
        "[clipboard]\nmax_mb=0\n", "[clipboard]\nmax_mb=1025\n", "[clipboard]\nfiles=2\n",
        "[integration]\nmenu_apps=some\n", "[integration]\nuac=prompt\n", "[integration]\nnotify=2\n",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(daemon_config_parse_string(&cfg, invalid[i], strlen(invalid[i])) == -1);
        assert(memcmp(&cfg, &before, sizeof(cfg)) == 0);
    }
}

int main(void) {
    test_defaults();
    test_parse_string();
    test_export_parser();
    test_file_loading();
    test_validation_ranges();
    test_seamless_settings();
    printf("test_daemon_config: all daemon configuration and INI parsing tests passed\n");
    return 0;
}
