/** @file av_deploy.c @brief Allocation-free managed driver-reboot readiness sequence. */
#include "av_deploy.h"
int av_deploy_finish(int guest_status, av_deploy_step_t restart, av_deploy_step_t guest_probe,
                     av_deploy_step_t host_probe, void *context) {
    if (!restart || !guest_probe || !host_probe) return -1;
    if (guest_status == 3) {
        int result = restart(context);
        if (result) return result;
        result = guest_probe(context);
        if (result) return result;
    } else if (guest_status) return guest_status;
    return host_probe(context);
}
