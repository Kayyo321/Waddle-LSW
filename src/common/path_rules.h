/** @file path_rules.h @brief Bounded lexical and filesystem-aware path rule APIs. */
#ifndef WaddlePathRulesH
/** @brief Include guard; compile-time marker with no storage or ownership. */
#define WaddlePathRulesH
#include <stddef.h>
/** @brief Maximum borrowed path rules accepted by the filesystem-aware API. */
#define PathRulesMaxCount 64U
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
/** @brief Validate and canonicalize a complete set of existing export roots.
 * @param[in] roots Nullable array of count borrowed absolute POSIX roots.
 * @param[in] count Number of roots; at most PathRulesMaxCount.
 * @param[out] canonical_roots Caller-owned pointer array of at least count
 * entries; each success entry owns a malloc string and is NULL on entry. This
 * pointer array must not overlap the roots array; its entries must be distinct.
 * @return count on success, or -1 with errno EINVAL, ENOENT, ENOTDIR, ELOOP,
 * EACCES or ENAMETOOLONG. On error all produced strings are freed and outputs
 * reset to NULL, with no partial result ownership exposed. Duplicate
 * normalized roots fail with EINVAL.
 * @note Roots must exist as directories. Every component is opened with
 * O_DIRECTORY|O_NOFOLLOW. Caller frees successful results with
 * waddle_path_roots_free(). No global state; independent calls are thread-safe.
 */
int waddle_path_roots_validate(const char *const *roots, size_t count,
                               char **canonical_roots);
/** @brief Free a result from waddle_path_roots_validate().
 * @param[in,out] canonical_roots Caller-owned pointer array.
 * @param[in] count Number of entries to free; each is defensively NULLed.
 * @note Safe for NULL array; no-op for count zero.
 */
void waddle_path_roots_free(char **canonical_roots, size_t count);
/** @brief Translate using canonical filesystem-validated export roots.
 * @param[in] path Borrowed NUL-terminated POSIX path, relative path, or
 * guest-native Windows path.
 * @param[in] rules Borrowed rules and targets; may be NULL when count is zero.
 * @param[in] count Number of rules, at most PathRulesMaxCount.
 * @return Owned malloc string, free with free(), or NULL with errno EINVAL for
 * malformed path/rules, EXDEV for an absolute path outside all roots, E2BIG
 * for oversized input, ENAMETOOLONG for canonical capacity overflow, or the
 * underlying root/input filesystem error. Existing absolute paths resolve via
 * realpath. Missing suffixes append only after the deepest existing ancestor.
 * Relative and Windows-native paths pass through unchanged without filesystem
 * lookup. Root validation errors leave no output allocations owned by caller.
 * @note This is a path conversion boundary, not access authorization; roots
 * must remain stable during a VM lifetime. Independent calls are thread-safe.
 */
char *waddle_translate_filesystem_rules(const char *path,
                                        const path_rule_t *rules, size_t count);
#endif
