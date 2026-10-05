#ifndef WaddleAvDeployH
#define WaddleAvDeployH
/** @brief Synchronous managed deployment step; no ownership transfer.
 * @param[in,out] context Optional borrowed caller context, valid for the call.
 * @return 0 success; nonzero readiness/restart error, propagated unchanged.
 * @note Deployment caller thread only; callbacks must clean owned resources
 * before returning and must not publish an incomplete deployment selector.
 */
typedef int (*av_deploy_step_t)(void *context);
/** @brief Complete readiness after native driver setup, restarting only when required.
 * @param[in] guest_status Native setup status: 0 ready, 3 reboot-required, others failure.
 * @param[in] restart Nonnull synchronous graceful managed-device restart callback.
 * @param[in] guest_probe Nonnull synchronous fresh guest probe callback.
 * @param[in] host_probe Nonnull synchronous native host readiness callback.
 * @param[in,out] context Optional borrowed callback context; never retained.
 * @return 0 both peers ready; native error or step error unchanged; -1 null callback.
 * @note Single caller thread, no allocation. Reboot-required setup executes one
 * restart, one fresh guest probe and one host probe, stopping at the first error.
 * Ordinary ready setup executes only the host probe. Caller may atomically publish
 * its complete immutable bundle only on zero; existing selection survives errors.
 */
int av_deploy_finish(int guest_status, av_deploy_step_t restart, av_deploy_step_t guest_probe,
                     av_deploy_step_t host_probe, void *context);
#endif
