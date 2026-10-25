/**
 * @file session_app.h
 * @brief Application runtime that composes feature handlers over one host control pump.
 *
 * session_app builds one interop_host_feature_ops_t from a registry of session_handler_t
 * tables and runs it through interop_host_control_run_detailed. It offers the union of the
 * handlers' capabilities in Hello, delivers each admitted inbound frame to the single handler
 * that owns its type, multiplexes every handler's descriptors, fans out ready and tick
 * callbacks, and closes every handler exactly once in reverse registration order.
 */
#ifndef WaddleInteropSessionAppH
#define WaddleInteropSessionAppH

#include <stddef.h>

#include "session_control_host.h"
#include "session_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Inputs for one application run; all borrowed until the call returns. */
typedef struct session_app_config_t {
    /** @brief Control inputs. feature_ops must be NULL and admission->local_capabilities must be zero;
     *         the application runtime supplies both. */
    interop_host_control_config_t control;
    /** @brief Handler table array in registration order, or NULL when handler_count is zero. */
    const session_handler_t *handlers;
    /** @brief Number of handlers: 0 (capability-zero, plain control runtime) or 1..SessionAppMaxHandlers. */
    size_t handler_count;
} session_app_config_t;

/** @brief Run one application session with detail.
 * @param[in] config Aligned readable configuration and handler table until return.
 * @param[out] out_owners Aligned zero-valued owner pair, as for interop_host_control_run.
 * @param[out] detail Optional diagnostic detail; NULL allowed.
 * @return The control outcome. InvalidArgument for any configuration rejected in
 *         session_app_validate. Handler failures end the run with HostControlFeatureFailed;
 *         a handler graceful stop ends it with HostControlCancelled.
 * @thread_safety Not thread-safe; the handlers run on the calling pump thread.
 */
interop_host_control_result_t session_app_run_detailed(
    const session_app_config_t *config,
    interop_host_control_owners_t *out_owners,
    interop_host_control_detail_t *detail);

/** @brief Run one application session without detail. Equivalent to session_app_run_detailed(config, owners, NULL). */
interop_host_control_result_t session_app_run(
    const session_app_config_t *config,
    interop_host_control_owners_t *out_owners);

#ifdef __cplusplus
}
#endif
#endif
