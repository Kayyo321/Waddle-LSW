/** @file session_guest_main.c @brief Fail-closed standalone session entry point. */
#include "session_control_windows.h"
#include <stdio.h>

/**
 * @brief Standalone entry point that refuses to manufacture freshness authority.
 * @param[in] argc CRT argument count, ignored.
 * @param[in] argv CRT argument vector, ignored and never retained.
 * @return Nonzero because this standalone process has no trusted fresh-exclusive
 *         authorization provider and must not initialize guest mapping metadata.
 * @note No Viosock, mapping, credential, image, or secret operation is attempted.
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    config.connection_token = 1;
    config.fresh_exclusive_authorized = 0;
    interop_guest_session_result_t result = interop_guest_session_run(&config, &owner);
    if (result == InteropGuestSessionFreshAuthorizationRequired && owner == NULL) {
        fputs("guest session disabled: fresh-exclusive authorization is unavailable\n", stderr);
    }
    return 1;
}
