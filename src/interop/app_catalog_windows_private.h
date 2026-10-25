/** @file app_catalog_windows_private.h @brief Internal Windows-only catalog validation seam. */
#ifndef AppCatalogWindowsPrivateHeaderGuard
#define AppCatalogWindowsPrivateHeaderGuard

#include "app_catalog.h"

/** @brief Test-only request to fail one real close without consuming its handle.
 * @param[in] context Borrowed test state owned for the enumeration call.
 * @param[in] kind Exact native close type requested.
 * @return True to inject one close failure before the real close; false to run
 * the real close. The callback never receives, owns, or closes the handle.
 * @note Internal error-test seam. Synchronous on the enumeration thread;
 * caller serializes and retains context for the whole call.
 */
typedef bool (*app_catalog_native_close_fault_fn_t)(
    void *context,
    app_catalog_cleanup_handle_kind_t kind);

/** @brief Borrowed roots used only by native catalog filesystem fixtures.
 * @note Each path points to a fixture-owned directory and is read only during
 * the call. The helper never creates, deletes, or traverses outside these
 * roots. Pass normalized absolute paths; the helper skips AppsFolder so the
 * test corpus remains deterministic. This type/API is internal and does not
 * change the public catalog interface.
 */
typedef struct app_catalog_windows_source_roots_t {
    const wchar_t *user_programs; /**< Absolute temporary fixture root. */
    const wchar_t *common_programs; /**< Absolute temporary fixture root. */
    const wchar_t *user_desktop; /**< Absolute temporary fixture root. */
    const wchar_t *public_desktop; /**< Absolute temporary fixture root. */
    app_catalog_native_close_fault_fn_t close_fault; /**< Optional test-only one-shot close failure. */
    void *close_fault_context; /**< Borrowed callback context; may be NULL. */
} app_catalog_windows_source_roots_t;

/** @brief Enumerate only supplied temporary known-folder fixture roots.
 * @param[in] allocator Complete caller-owned allocator retained by output.
 * @param[in] options Bounded borrowed options with max_nodes in 1..65536.
 * @param[in] roots Non-NULL borrowed absolute roots, readable for the call.
 * @param[out] output Initialized empty snapshot; unchanged on error.
 * @return Catalog status; no file or directory is created or deleted.
 * @note Internal Windows test seam. It uses real STA COM, shell-link, file, and
 * path APIs but maps known folders to the four supplied roots and skips
 * AppsFolder. Caller owns roots and serializes output access for the call.
 */
app_catalog_status_t app_catalog_windows_enumerate_fixture_roots(
    app_catalog_allocator_t allocator,
    const app_catalog_enumeration_options_t *options,
    const app_catalog_windows_source_roots_t *roots,
    app_catalog_snapshot_t *output);

/** @brief Run native provider fault cases on private temporary shortcut roots.
 * @return Zero when every injected failure preserves outputs and native
 * ownership; nonzero on the first failed assertion.
 * @note Test-only entrypoint implemented by the native fixture profile TU.
 * It creates and removes only files and directories under a unique temporary
 * directory, and uses real Win32 handles with fail-before-consumption wrappers.
 * It does not alter the production provider or public catalog ABI.
 */
int app_catalog_windows_fault_fixture_run(void);

#endif
