/** @file canonical_paths.c @brief Filesystem-aware canonical path contract regression. */
#include "path_rules.h"
#include "waddle/cli_protocol.h"
#include "waddle/daemon_protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t allocation_failure_index;
static size_t allocation_call_index;
static int injected_open_errno;
static int injected_fstatat_errno;
static int injected_openat_errno;
static int injected_lstat_errno;
static int injected_realpath_errno;
static size_t injected_realpath_after;
static size_t realpath_call_index;
static size_t fake_realpath_length;
static size_t fake_realpath_after;
static size_t fake_realpath_call_index;

static int fail_allocation(void) {
    if (allocation_failure_index == 0) return 0;
    allocation_call_index++;
    if (allocation_call_index != allocation_failure_index) return 0;
    errno = ENOMEM;
    return 1;
}

extern void *__real_malloc(size_t size);
extern char *__real_strdup(const char *string);
extern char *__real_strndup(const char *string, size_t length);
extern char *__real_realpath(const char *path, char *resolved_path);
extern int __real_open(const char *path, int flags, ...);
extern int __real_fstatat(int directory, const char *path, struct stat *status, int flags);
extern int __real_openat(int directory, const char *path, int flags, ...);
extern int __real_lstat(const char *path, struct stat *status);

void *__wrap_malloc(size_t size) {
    return fail_allocation() ? NULL : __real_malloc(size);
}

char *__wrap_strdup(const char *string) {
    return fail_allocation() ? NULL : __real_strdup(string);
}

char *__wrap_strndup(const char *string, size_t length) {
    return fail_allocation() ? NULL : __real_strndup(string, length);
}

char *__wrap_realpath(const char *path, char *resolved_path) {
    if (fake_realpath_length != 0) {
        fake_realpath_call_index++;
        if (fake_realpath_call_index > fake_realpath_after) {
            char *fake_path = __real_malloc(fake_realpath_length + 1);
            if (fake_path == NULL) return NULL;
            memset(fake_path, 'x', fake_realpath_length);
            fake_path[0] = '/';
            fake_path[fake_realpath_length] = '\0';
            return fake_path;
        }
    }
    if (injected_realpath_errno != 0) {
        realpath_call_index++;
        if (realpath_call_index > injected_realpath_after) { errno = injected_realpath_errno; return NULL; }
    }
    return fail_allocation() ? NULL : __real_realpath(path, resolved_path);
}

int __wrap_open(const char *path, int flags, ...) {
    if (injected_open_errno != 0 && strcmp(path, "/") == 0) { errno = injected_open_errno; return -1; }
    if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list arguments;
        va_start(arguments, flags);
        mode_t mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}

int __wrap_fstatat(int directory, const char *path, struct stat *status, int flags) {
    if (injected_fstatat_errno != 0) { errno = injected_fstatat_errno; return -1; }
    return __real_fstatat(directory, path, status, flags);
}

int __wrap_openat(int directory, const char *path, int flags, ...) {
    if (injected_openat_errno != 0) { errno = injected_openat_errno; return -1; }
    if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list arguments;
        va_start(arguments, flags);
        mode_t mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
        return __real_openat(directory, path, flags, mode);
    }
    return __real_openat(directory, path, flags);
}

int __wrap_lstat(const char *path, struct stat *status) {
    if (injected_lstat_errno != 0) { errno = injected_lstat_errno; return -1; }
    return __real_lstat(path, status);
}

int main(void) {
    char tree[] = "/tmp/waddle-canonical-XXXXXX";
    assert(mkdtemp(tree) != NULL);
    char root[1024], nested[1024], outside[1024], file_path[1024], link_path[1024], inside_link[1024], broken_link[1024], loop_link[1024], input[1024];
    assert(snprintf(root, sizeof(root), "%s/root", tree) < (int)sizeof(root));
    assert(snprintf(nested, sizeof(nested), "%s/root/nested", tree) < (int)sizeof(nested));
    assert(snprintf(outside, sizeof(outside), "%s/outside", tree) < (int)sizeof(outside));
    assert(snprintf(file_path, sizeof(file_path), "%s/root/regular-file", tree) < (int)sizeof(file_path));
    assert(snprintf(link_path, sizeof(link_path), "%s/link", tree) < (int)sizeof(link_path));
    assert(snprintf(inside_link, sizeof(inside_link), "%s/root/nested-alias", tree) < (int)sizeof(inside_link));
    assert(snprintf(broken_link, sizeof(broken_link), "%s/root/broken", tree) < (int)sizeof(broken_link));
    assert(snprintf(loop_link, sizeof(loop_link), "%s/root/loop", tree) < (int)sizeof(loop_link));
    assert(mkdir(root, 0700) == 0 && mkdir(nested, 0700) == 0 && mkdir(outside, 0700) == 0);
    FILE *regular_file = fopen(file_path, "w");
    assert(regular_file != NULL && fclose(regular_file) == 0);
    assert(symlink(outside, link_path) == 0);
    assert(symlink(nested, inside_link) == 0);
    assert(symlink("/no/such/waddle-target", broken_link) == 0);
    assert(symlink("loop", loop_link) == 0);

    /* Linker syscall seams must preserve variadic creation modes, including
     * gcov's exit-time profile writes outside production path operations. */
    char mode_probe[1024];
    assert(snprintf(mode_probe, sizeof(mode_probe), "%s/mode-probe", tree) < (int)sizeof(mode_probe));
    int mode_fd = open(mode_probe, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(mode_fd >= 0);
    struct stat mode_status;
    assert(fstat(mode_fd, &mode_status) == 0 && (mode_status.st_mode & 0777) == 0600);
    assert(close(mode_fd) == 0 && unlink(mode_probe) == 0);
    int directory_fd = open(tree, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(directory_fd >= 0);
    mode_fd = openat(directory_fd, "mode-probe", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(mode_fd >= 0 && fstat(mode_fd, &mode_status) == 0 && (mode_status.st_mode & 0777) == 0600);
    assert(close(mode_fd) == 0 && unlinkat(directory_fd, "mode-probe", 0) == 0 && close(directory_fd) == 0);

    const char *roots[] = {root, nested};
    char *canonical[2] = {0};
    assert(waddle_path_roots_validate(roots, 2, canonical) == 2);
    assert(strcmp(canonical[0], root) == 0 && strcmp(canonical[1], nested) == 0);
    waddle_path_roots_free(canonical, 2);
    assert(waddle_path_roots_validate(NULL, 0, NULL) == 0);
    waddle_path_roots_free(NULL, 0);
    assert(waddle_path_roots_validate(roots, 1, NULL) == -1 && errno == EINVAL);
    assert(waddle_path_roots_validate(NULL, 1, canonical) == -1 && errno == EINVAL);
    const char *null_root[] = {NULL};
    assert(waddle_path_roots_validate(null_root, 1, canonical) == -1 && errno == EINVAL);
    const char *relative_root[] = {"relative/root"};
    assert(waddle_path_roots_validate(relative_root, 1, canonical) == -1 && errno == EINVAL);
    const char *dot_root[] = {"//./tmp//waddle-normalized/../bad"};
    assert(waddle_path_roots_validate(dot_root, 1, canonical) == -1 && errno == EINVAL);
    const char *backslash_root[] = {"/tmp/waddle\\root"};
    assert(waddle_path_roots_validate(backslash_root, 1, canonical) == -1 && errno == EINVAL);
    const char *equals_root[] = {"/tmp/waddle=root"};
    assert(waddle_path_roots_validate(equals_root, 1, canonical) == -1 && errno == EINVAL);
    const char control_root_bytes[] = {'/', 'x', '\x01', '\0'};
    const char *control_root[] = {control_root_bytes};
    assert(waddle_path_roots_validate(control_root, 1, canonical) == -1 && errno == EINVAL);
    const char malformed_root_bytes[] = {'/', 'x', (char)0xff, '\0'};
    const char *invalid_utf8_root[] = {malformed_root_bytes};
    assert(waddle_path_roots_validate(invalid_utf8_root, 1, canonical) == -1 && errno == EINVAL);
    const char *one_character_root[] = {"/x"};
    assert(waddle_path_roots_validate(one_character_root, 1, canonical) == -1 && errno == ENOENT);
    const char *dot_prefix_root[] = {"/.x"};
    assert(waddle_path_roots_validate(dot_prefix_root, 1, canonical) == -1 && errno == ENOENT);

    path_rule_t rules[] = {{root, "X:\\"}, {nested, "Y:\\"}};
    path_rule_t root_rule[] = {{"/", "Q:\\"}};
    assert(snprintf(input, sizeof(input), "%s/%s", nested, "日本語") < (int)sizeof(input));
    char *mapped = waddle_translate_filesystem_rules(input, rules, 2);
    assert(mapped != NULL && strcmp(mapped, "Y:\\日本語") == 0); free(mapped);
    path_rule_t reverse_rules[] = {{nested, "Y:\\"}, {root, "X:\\"}};
    mapped = waddle_translate_filesystem_rules(input, reverse_rules, 2);
    assert(mapped != NULL && strcmp(mapped, "Y:\\日本語") == 0); free(mapped);
    assert(snprintf(input, sizeof(input), "%s/new/leaf", root) < (int)sizeof(input));
    mapped = waddle_translate_filesystem_rules(input, rules, 2);
    assert(mapped != NULL && strcmp(mapped, "X:\\new\\leaf") == 0); free(mapped);
    assert(snprintf(input, sizeof(input), "%s/nested-alias/missing", root) < (int)sizeof(input));
    mapped = waddle_translate_filesystem_rules(input, rules, 2);
    assert(mapped != NULL && strcmp(mapped, "Y:\\missing") == 0); free(mapped);
    assert(snprintf(input, sizeof(input), "%s/link/file", tree) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == EXDEV);
    assert(snprintf(input, sizeof(input), "%s/broken/child", root) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == ENOENT);
    assert(snprintf(input, sizeof(input), "%s/loop/child", root) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == ELOOP);
    assert(snprintf(input, sizeof(input), "%s/regular-file/.", root) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == ENOTDIR);
    assert(snprintf(input, sizeof(input), "%s/regular-file/", root) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == ENOTDIR);
    assert(snprintf(input, sizeof(input), "%s/regular-file/child", root) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == ENOTDIR);
    assert(waddle_translate_filesystem_rules("/unshared/file", rules, 2) == NULL && errno == EXDEV);
    mapped = waddle_translate_filesystem_rules("relative/path", NULL, 0);
    assert(mapped != NULL && strcmp(mapped, "relative/path") == 0); free(mapped);
    assert(waddle_translate_filesystem_rules("/absolute/path", NULL, 0) == NULL && errno == EXDEV);
    assert(waddle_translate_filesystem_rules(NULL, NULL, 0) == NULL && errno == EINVAL);
    assert(waddle_translate_filesystem_rules("relative", NULL, PathRulesMaxCount + 1) == NULL && errno == EINVAL);
    assert(waddle_translate_filesystem_rules("relative", NULL, 1) == NULL && errno == EINVAL);
    char oversized_input[WaddleMaxPayloadSize + 2];
    oversized_input[0] = '/'; memset(oversized_input + 1, 'x', sizeof(oversized_input) - 2); oversized_input[sizeof(oversized_input) - 1] = '\0';
    assert(waddle_translate_filesystem_rules(oversized_input, root_rule, 1) == NULL && errno == E2BIG);
    assert(snprintf(input, sizeof(input), "%s/../outside/file", root) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == EINVAL);
    assert(snprintf(input, sizeof(input), "%s-sibling/file", root) < (int)sizeof(input));
    assert(waddle_translate_filesystem_rules(input, rules, 2) == NULL && errno == EXDEV);
    mapped = waddle_translate_filesystem_rules("/waddle-canonical-absent-leaf", root_rule, 1);
    assert(mapped != NULL && strcmp(mapped, "Q:\\waddle-canonical-absent-leaf") == 0); free(mapped);

    const char *duplicates[] = {root, root};
    char *untouched[2] = {NULL, NULL};
    assert(waddle_path_roots_validate(duplicates, 2, untouched) == -1 && errno == EINVAL);
    assert(untouched[0] == NULL && untouched[1] == NULL);
    const char *symlink_root[] = {link_path};
    char *one[1] = {NULL};
    assert(waddle_path_roots_validate(symlink_root, 1, one) == -1 && errno == ELOOP && one[0] == NULL);
    assert(waddle_path_roots_validate((const char *const[]){"/missing/waddle-root"}, 1, one) == -1 && errno == ENOENT);
    const char *file_root[] = {file_path};
    assert(waddle_path_roots_validate(file_root, 1, one) == -1 && errno == ENOTDIR && one[0] == NULL);
    char overlong_root[WaddleMaxPathLen + 1];
    overlong_root[0] = '/'; memset(overlong_root + 1, 'x', sizeof(overlong_root) - 2); overlong_root[sizeof(overlong_root) - 1] = '\0';
    const char *long_root[] = {overlong_root};
    assert(waddle_path_roots_validate(long_root, 1, one) == -1 && errno == ENAMETOOLONG);
    char *too_many_outputs[PathRulesMaxCount + 1] = {0};
    assert(waddle_path_roots_validate(NULL, PathRulesMaxCount + 1, too_many_outputs) == -1 && errno == EINVAL);
    path_rule_t malformed[] = {{NULL, "X:\\"}};
    assert(waddle_translate_filesystem_rules(input, malformed, 1) == NULL && errno == EINVAL);
    path_rule_t malformed_target[] = {{root, NULL}};
    assert(waddle_translate_filesystem_rules(input, malformed_target, 1) == NULL && errno == EINVAL);
    path_rule_t malformed_target_value[] = {{root, "not-a-drive"}};
    assert(waddle_translate_filesystem_rules(input, malformed_target_value, 1) == NULL && errno == EINVAL);

    injected_open_errno = EACCES;
    assert(waddle_path_roots_validate(roots, 1, one) == -1 && errno == EACCES && one[0] == NULL);
    injected_open_errno = 0;
    injected_fstatat_errno = EACCES;
    assert(waddle_path_roots_validate(roots, 1, one) == -1 && errno == EACCES && one[0] == NULL);
    injected_fstatat_errno = 0;
    injected_openat_errno = EACCES;
    assert(waddle_path_roots_validate(roots, 1, one) == -1 && errno == EACCES && one[0] == NULL);
    injected_openat_errno = 0;
    injected_lstat_errno = EACCES;
    assert(waddle_translate_filesystem_rules("/waddle-canonical-missing-parent/leaf", root_rule, 1) == NULL && errno == EACCES);
    injected_lstat_errno = 0;
    injected_realpath_errno = EACCES;
    injected_realpath_after = 1;
    realpath_call_index = 0;
    assert(waddle_translate_filesystem_rules("/waddle-canonical-missing-parent/leaf", root_rule, 1) == NULL && errno == EACCES);
    injected_realpath_errno = 0;
    fake_realpath_length = WaddleMaxPathLen;
    fake_realpath_after = 0;
    fake_realpath_call_index = 0;
    assert(waddle_translate_filesystem_rules(nested, rules, 2) == NULL && errno == ENAMETOOLONG);
    fake_realpath_length = WaddleMaxPathLen - 4;
    fake_realpath_after = 2;
    fake_realpath_call_index = 0;
    assert(waddle_translate_filesystem_rules("/waddle-canonical-missing-parent/leaf", root_rule, 1) == NULL && errno == ENAMETOOLONG);
    fake_realpath_length = WaddleMaxPathLen;
    fake_realpath_after = 1;
    fake_realpath_call_index = 0;
    assert(waddle_translate_filesystem_rules("/waddle-canonical-missing-parent/leaf", root_rule, 1) == NULL && errno == ENAMETOOLONG);
    fake_realpath_length = 0;
    injected_fstatat_errno = EACCES;
    assert(waddle_path_roots_validate(roots, 1, one) == -1 && errno == EACCES);
    injected_fstatat_errno = 0;
    injected_open_errno = EMFILE;
    assert(waddle_path_roots_validate(roots, 1, one) == -1 && errno == EMFILE);
    injected_open_errno = 0;

    int observed_allocation_failure = 0;
    int observed_success_after_failures = 0;
    for (size_t failure_index = 1; failure_index < 32; failure_index++) {
        allocation_failure_index = failure_index;
        allocation_call_index = 0;
        mapped = waddle_translate_filesystem_rules(nested, rules, 2);
        allocation_failure_index = 0;
        if (mapped != NULL) {
            observed_success_after_failures = 1;
            free(mapped);
            break;
        }
        assert(errno == ENOMEM);
        observed_allocation_failure = 1;
    }
    assert(observed_allocation_failure && observed_success_after_failures);
    observed_allocation_failure = 0;
    observed_success_after_failures = 0;
    for (size_t failure_index = 1; failure_index < 32; failure_index++) {
        allocation_failure_index = failure_index;
        allocation_call_index = 0;
        mapped = waddle_translate_filesystem_rules("/waddle-canonical-missing-parent/leaf", root_rule, 1);
        allocation_failure_index = 0;
        if (mapped != NULL) {
            observed_success_after_failures = 1;
            free(mapped);
            break;
        }
        assert(errno == ENOMEM);
        observed_allocation_failure = 1;
    }
    assert(observed_allocation_failure && observed_success_after_failures);

    assert(unlink(loop_link) == 0 && unlink(broken_link) == 0 && unlink(inside_link) == 0 && unlink(link_path) == 0);
    assert(unlink(file_path) == 0);
    assert(rmdir(nested) == 0 && rmdir(root) == 0 && rmdir(outside) == 0 && rmdir(tree) == 0);
    return 0;
}
