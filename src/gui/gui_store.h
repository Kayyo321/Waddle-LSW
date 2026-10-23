/** @file gui_store.h @brief Small private XDG preference/launcher store. */
#ifndef WaddleGuiStoreH
#define WaddleGuiStoreH
#include <gio/gio.h>
/** @brief Main-context-owned store; key file and path freed by gui_store_free. */
typedef struct gui_store_t {
    GKeyFile *keys; /**< Owned preferences and launcher string lists. */
    char *path; /**< Owned absolute configuration filename. */
    gboolean writable; /**< False after malformed/oversized input, protecting recovery data. */
} gui_store_t;
/** @brief Load bounded optional XDG file; return owned store, including on parse failure.
 * @param[out] error Optional caller-owned diagnostic; malformed input is never overwritten automatically.
 * @return Nonnull main-context-owned store; use gui_store_free to release.
 */
gui_store_t *gui_store_load(GError **error);
/** @brief Atomically save mode 0600; borrowed store, owned optional error, TRUE on success.
 * Caller invokes only after explicit preference/launcher edit; main-context only. */
gboolean gui_store_save(gui_store_t *store, GError **error);
/** @brief Free nullable owned store and all members; no retained storage, main-context only. */
void gui_store_free(gui_store_t *store);
/** @brief Read owned text or fallback copy; nullable keys accepted, caller frees. */
char *gui_store_text(gui_store_t *store, const char *group, const char *key, const char *fallback);
/** @brief Read integer with fallback and bounds; pure, no allocation or ownership transfer. */
int gui_store_number(gui_store_t *store, const char *key, int fallback, int minimum, int maximum);
#endif
