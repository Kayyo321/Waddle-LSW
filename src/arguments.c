/**
 * @file arguments.c
 * @brief Win32 CommandLineToArgvW inverse quoting, unquoting parser, and path translation.
 */

#include "waddle/cli_protocol.h"
#include "path_rules.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

char *waddle_quote(char *const argv[]) {
    if (argv == NULL || argv[0] == NULL) {
        char *empty = (char *)malloc(1);
        if (empty != NULL) {
            empty[0] = '\0';
        }
        return empty;
    }

    size_t size = 1;
    for (size_t i = 0; argv[i] != NULL; i++) {
        size_t n = strlen(argv[i]);
        if (size > WaddleMaxPayloadSize - 3 || n > (WaddleMaxPayloadSize - size - 3) / 2) {
            errno = E2BIG;
            return NULL;
        }
        size += (2 * n) + 3;
    }

    char *out = (char *)malloc(size);
    if (out == NULL) {
        return NULL;
    }

    char *p = out;
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            *p++ = ' ';
        }
        *p++ = '"';
        const char *s = argv[i];
        while (*s != '\0') {
            size_t slashes = 0;
            while (*s == '\\') {
                slashes++;
                s++;
            }
            size_t emit = (*s == '"' || *s == '\0') ? (2 * slashes) : slashes;
            while (emit--) {
                *p++ = '\\';
            }
            if (*s == '"') {
                *p++ = '\\';
            }
            if (*s != '\0') {
                *p++ = *s++;
            }
        }
        *p++ = '"';
    }
    *p = '\0';
    return out;
}

void waddle_free_argv(char **argv) {
    if (argv == NULL) {
        return;
    }
    for (size_t i = 0; argv[i] != NULL; i++) {
        free(argv[i]);
        argv[i] = NULL;
    }
    free(argv);
}

char **waddle_unquote(const char *command) {
    if (command == NULL) {
        errno = EINVAL;
        return NULL;
    }

    size_t len = strlen(command);
    size_t count = 0;
    char **argv = (char **)calloc((len / 2) + 2, sizeof(*argv));
    if (argv == NULL) {
        return NULL;
    }

    const char *s = command;
    while (*s != '\0') {
        if (*s != '"') {
            goto invalid;
        }
        s++;

        char *a = (char *)malloc(strlen(s) + 1);
        if (a == NULL) {
            goto invalid;
        }
        argv[count++] = a;
        char *p = a;

        for (;;) {
            size_t n = 0;
            while (*s == '\\') {
                n++;
                s++;
            }
            if (*s == '"') {
                for (size_t i = 0; i < n / 2; i++) {
                    *p++ = '\\';
                }
                s++;
                if (n % 2 != 0) {
                    *p++ = '"';
                    continue;
                }
                break;
            }
            while (n--) {
                *p++ = '\\';
            }
            if (*s == '\0') {
                goto invalid;
            }
            *p++ = *s++;
        }
        *p = '\0';

        if (*s == ' ') {
            s++;
        } else if (*s != '\0') {
            goto invalid;
        }
    }

    if (count == 0) {
        goto invalid;
    }
    return argv;

invalid:
    waddle_free_argv(argv);
    errno = EINVAL;
    return NULL;
}

char *waddle_translate_rules(const char *path, const path_rule_t *rules, size_t count) {
    if (path == NULL || count > 64 || (count != 0 && rules == NULL)) {
        errno = EINVAL;
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (rules[i].source == NULL || rules[i].target == NULL ||
            !waddle_path_rule_valid(rules[i].source, rules[i].target)) {
            errno = EINVAL;
            return NULL;
        }
    }
    if (strlen(path) > WaddleMaxPayloadSize) {
        errno = E2BIG;
        return NULL;
    }
    if (path[0] != '/') {
        return strdup(path);
    }
    const char *source = "/";
    const char *target = "Z:\\";
    size_t longest = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(rules[i].source);
        while (n > 1 && rules[i].source[n - 1] == '/') { n--; }
        if (n > longest && strncmp(path, rules[i].source, n) == 0 &&
            (n == 1 || path[n] == '\0' || path[n] == '/')) {
            longest = n;
            source = rules[i].source;
            target = rules[i].target;
        }
    }
    if (count != 0 && longest == 0) {
        errno = EINVAL;
        return NULL;
    }
    size_t capacity = strlen(path) + strlen(target) + 4;
    char *output = malloc(capacity);
    if (output == NULL) { return NULL; }
    int status = waddle_path_rule_apply(path, source, target, (unsigned char *)output, capacity);
    if (status != 0) {
        free(output);
        output = NULL;
        errno = status == -2 ? E2BIG : EINVAL;
    }
    return output;
}

char *waddle_translate_path(const char *path) {
    return waddle_translate_rules(path, NULL, 0);
}
