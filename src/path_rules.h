/** @file path_rules.h @brief Bounded, allocation-free Zig path rule boundary. */
#ifndef WaddlePathRulesH
#define WaddlePathRulesH
#include <stddef.h>
/** @brief Borrowed immutable export rule; strings live through the calling session.
 * @note No allocations; safe for concurrent readers, caller serializes mutation. */
typedef struct path_rule_t {
    const char *source; /**< Nonnull absolute POSIX source, borrowed. */
    const char *target; /**< Nonnull drive-absolute Windows target, borrowed. */
} path_rule_t;
/** @brief Validate rule syntax without allocation; thread-safe.
 * @param[in] source Nonnull NUL-terminated borrowed source.
 * @param[in] target Nonnull NUL-terminated borrowed target.
 * @return 1 valid, 0 invalid; no ownership transfer. */
int waddle_path_rule_valid(const char *source, const char *target);
/** @brief Apply one rule using bounds-checked Zig parsing; thread-safe.
 * @param[in] path Nonnull borrowed NUL-terminated POSIX path.
 * @param[in] source Nonnull borrowed NUL-terminated rule source.
 * @param[in] target Nonnull borrowed NUL-terminated rule target.
 * @param[out] output Nonnull caller-owned writable buffer, disjoint from inputs.
 * @param[in] capacity Buffer size including NUL; output may be partial on failure.
 * @return 0 success, -1 invalid/unmapped input, -2 insufficient capacity. */
int waddle_path_rule_apply(const char *path, const char *source, const char *target,
                          unsigned char *output, size_t capacity);
/** @brief Translate with longest-prefix rule selection; thread-safe, no globals.
 * @param[in] path Nonnull borrowed NUL-terminated path; relative paths copied.
 * @param[in] rules Borrowed array of count immutable validated rules, nullable if zero.
 * @param[in] count Number of rules; zero selects the default root mapping.
 * @return Caller-owned malloc string, free with free(); NULL sets errno to EINVAL,
 * E2BIG, or ENOMEM. Unmatched absolute paths fail. */
char *waddle_translate_rules(const char *path, const path_rule_t *rules, size_t count);
#endif
