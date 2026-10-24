/**
 * @file path_filesystem.c
 * @brief Host filesystem-aware canonical export and path conversion boundary.
 * @note Independent calls own all temporary allocations; roots must remain
 * stable while a VM uses them. This layer performs no access authorization.
 */
#include "waddle/cli_protocol.h"
#include "waddle/daemon_protocol.h"
#include "path_rules.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *normalize_absolute_path(const char *path, int root_syntax) {
    /* Public callers validate non-NULL before reaching this private helper. */
    if (path[0] != '/') { errno = EINVAL; return NULL; }
    size_t length = strnlen(path, WaddleMaxPathLen);
    if (length == WaddleMaxPathLen) { errno = ENAMETOOLONG; return NULL; }
    char *result = malloc(length + 2);
    if (result == NULL) return NULL;
    size_t used = 0, at = 0;
    result[used++] = '/';
    while (at < length) {
        while (at < length && path[at] == '/') at++;
        size_t start = at;
        while (at < length && path[at] != '/') at++;
        size_t n = at - start;
        if (n == 0) continue;
        if (n == 1 && path[start] == '.') continue;
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
            free(result); errno = EINVAL; return NULL;
        }
        if (root_syntax) {
            for (size_t i = 0; i < n; i++) {
                if ((unsigned char)path[start + i] < 32 || path[start + i] == '\\' || path[start + i] == '=') {
                    free(result); errno = EINVAL; return NULL;
                }
            }
        }
        if (used > 1) result[used++] = '/';
        /* Normalization only removes bytes, and the input is already bounded. */
        memcpy(result + used, path + start, n); used += n;
    }
    result[used] = '\0';
    if (root_syntax && !waddle_path_rule_valid(result, "Z:\\")) { free(result); errno = EINVAL; return NULL; }
    return result;
}

static int validate_root_nofollow(const char *root) {
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    const char *p = root + 1;
    while (*p != '\0') {
        const char *end = strchr(p, '/');
        size_t n = end == NULL ? strlen(p) : (size_t)(end - p);
        char part[WaddleMaxPathLen];
        /* Root normalization removes empty components; each component is shorter than the path bound. */
        memcpy(part, p, n); part[n] = '\0';
        struct stat st;
        if (fstatat(fd, part, &st, AT_SYMLINK_NOFOLLOW) != 0) { int e = errno; close(fd); errno = e; return -1; }
        if (S_ISLNK(st.st_mode)) { close(fd); errno = ELOOP; return -1; }
        if (!S_ISDIR(st.st_mode)) { close(fd); errno = ENOTDIR; return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) { int e = errno; close(fd); errno = e; return -1; }
        close(fd); fd = next;
        if (end == NULL) break;
        p = end + 1;
    }
    close(fd);
    return 0;
}

int waddle_path_roots_validate(const char *const *roots, size_t count, char **canonical_roots) {
    if (count > PathRulesMaxCount || (count && (roots == NULL || canonical_roots == NULL))) { errno = EINVAL; return -1; }
    if (canonical_roots != NULL) for (size_t i = 0; i < count; i++) canonical_roots[i] = NULL;
    for (size_t i = 0; i < count; i++) {
        if (roots[i] == NULL) { errno = EINVAL; goto fail; }
        canonical_roots[i] = normalize_absolute_path(roots[i], 1);
        if (canonical_roots[i] == NULL) goto fail;
        for (size_t j = 0; j < i; j++) if (strcmp(canonical_roots[i], canonical_roots[j]) == 0) { errno = EINVAL; goto fail; }
        if (validate_root_nofollow(canonical_roots[i]) != 0) goto fail;
    }
    return (int)count;
fail:
    {
        int saved_errno = errno;
        waddle_path_roots_free(canonical_roots, count);
        errno = saved_errno;
    }
    return -1;
}

void waddle_path_roots_free(char **canonical_roots, size_t count) {
    if (canonical_roots == NULL) return;
    for (size_t i = 0; i < count; i++) { free(canonical_roots[i]); canonical_roots[i] = NULL; }
}

static char *resolve_absolute_input(const char *path) {
    char *normalized = normalize_absolute_path(path, 0);
    char *candidate = NULL;
    char *resolved = NULL;
    char *output = NULL;
    char *suffix[WaddleMaxPathLen / 2] = {0};
    size_t suffix_count = 0;
    int saved_errno = 0;
    if (normalized == NULL) return NULL;

    /* Consult the unnormalized spelling first so POSIX ENOTDIR semantics for
     * paths such as "file/" and "file/." are not erased by normalization. */
    resolved = realpath(path, NULL);
    if (resolved != NULL) {
        if (strlen(resolved) >= WaddleMaxPathLen) { saved_errno = ENAMETOOLONG; goto cleanup; }
        output = resolved;
        resolved = NULL;
        goto cleanup;
    }
    saved_errno = errno;
    if (saved_errno != ENOENT) goto cleanup;

    candidate = strdup(normalized);
    if (candidate == NULL) { saved_errno = errno; goto cleanup; }
    for (;;) {
        resolved = realpath(candidate, NULL);
        if (resolved != NULL) break;
        saved_errno = errno;
        if (saved_errno != ENOENT) goto cleanup;
        struct stat st;
        if (lstat(candidate, &st) == 0) { saved_errno = ENOENT; goto cleanup; }
        if (errno != ENOENT) { saved_errno = errno; goto cleanup; }
        char *slash = strrchr(candidate, '/'); /* candidate is always an absolute normalized path. */
        const char *component = slash + 1;
        char *suffix_component = strdup(component);
        if (suffix_component == NULL) { saved_errno = errno; goto cleanup; }
        /* A bounded path has fewer components than this fixed suffix array. */
        suffix[suffix_count++] = suffix_component;
        if (slash == candidate) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
    }

    {
        size_t needed = strlen(resolved);
        for (size_t i = suffix_count; i > 0; i--) {
            size_t component_length = strlen(suffix[i - 1]);
            if (needed > WaddleMaxPathLen - component_length - 1) {
                saved_errno = ENAMETOOLONG;
                goto cleanup;
            }
            needed += component_length + 1;
        }
        if (needed >= WaddleMaxPathLen) { saved_errno = ENAMETOOLONG; goto cleanup; }
        output = malloc(needed + 1);
        if (output == NULL) { saved_errno = errno; goto cleanup; }
        strcpy(output, resolved);
        for (size_t i = suffix_count; i > 0; i--) {
            if (strcmp(output, "/") != 0) strcat(output, "/");
            strcat(output, suffix[i - 1]);
        }
    }
    saved_errno = 0;

cleanup:
    free(normalized);
    free(candidate);
    free(resolved);
    for (size_t i = 0; i < suffix_count; i++) free(suffix[i]);
    if (output == NULL) errno = saved_errno;
    return output;
}

char *waddle_translate_filesystem_rules(const char *path, const path_rule_t *rules, size_t count) {
    if (path == NULL || count > PathRulesMaxCount || (count && rules == NULL)) { errno = EINVAL; return NULL; }
    size_t path_len = strnlen(path, WaddleMaxPayloadSize + 1);
    if (path_len > WaddleMaxPayloadSize) { errno = E2BIG; return NULL; }
    if (path[0] != '/') return strdup(path);
    if (count == 0) { errno = EXDEV; return NULL; }
    const char *raw_roots[PathRulesMaxCount];
    char *canonical[PathRulesMaxCount] = {0};
    for (size_t i = 0; i < count; i++) {
        if (rules[i].source == NULL || rules[i].target == NULL || !waddle_path_rule_valid(rules[i].source, rules[i].target)) { errno = EINVAL; return NULL; }
        raw_roots[i] = rules[i].source;
    }
    if (waddle_path_roots_validate(raw_roots, count, canonical) < 0) return NULL;
    char *input = resolve_absolute_input(path);
    if (input == NULL) { waddle_path_roots_free(canonical, count); return NULL; }
    size_t best_len = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(canonical[i]);
        if (n > best_len && strncmp(input, canonical[i], n) == 0 &&
            (n == 1 || input[n] == '\0' || input[n] == '/')) { best_len = n; }
    }
    if (best_len == 0) { free(input); waddle_path_roots_free(canonical, count); errno = EXDEV; return NULL; }
    path_rule_t normalized[PathRulesMaxCount];
    for (size_t i = 0; i < count; i++) normalized[i] = (path_rule_t){canonical[i], rules[i].target};
    char *output = waddle_translate_rules(input, normalized, count);
    int saved_errno = errno;
    free(input); waddle_path_roots_free(canonical, count);
    if (output == NULL) errno = saved_errno;
    return output;
}
