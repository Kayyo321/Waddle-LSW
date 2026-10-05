/** @file deploy.c @brief Restart/probe ordering and preservation of deployment failures. */
#include "av_deploy.h"
#include <assert.h>
#include <stddef.h>
/** @brief Test-owned ordered step outcomes, valid only during synchronous calls. */
typedef struct deploy_fixture_t {
    int next;
    int results[3];
} deploy_fixture_t;
static int step(deploy_fixture_t *fixture, int index) {
    assert(fixture && fixture->next == index);
    ++fixture->next;
    return fixture->results[index];
}
static int restart(void *context) { return step(context, 0); }
static int guest_probe(void *context) { return step(context, 1); }
static int host_probe(void *context) { return step(context, 2); }
int main(void) {
    assert(av_driver_install_status(0) == 0);
    assert(av_driver_install_status(259) == 0);
    assert(av_driver_install_status(5) == 1);
    assert(av_driver_install_status(UINT32_MAX) == 1);
    for (unsigned i = 0; i < 2; ++i) {
        deploy_fixture_t reboot = {0};
        uint32_t native_status = i ? 1641 : 3010;
        assert(av_driver_install_status(native_status) == 3);
        assert(av_deploy_finish(av_driver_install_status(native_status), restart, guest_probe,
                               host_probe, &reboot) == 0 && reboot.next == 3);
    }
    deploy_fixture_t fixture = {0};
    assert(av_deploy_finish(3, restart, guest_probe, host_probe, &fixture) == 0);
    assert(fixture.next == 3);
    for (int failed = 0; failed < 3; ++failed) {
        fixture = (deploy_fixture_t){0};
        fixture.results[failed] = failed + 10;
        assert(av_deploy_finish(3, restart, guest_probe, host_probe, &fixture) == failed + 10);
        assert(fixture.next == failed + 1);
    }
    fixture = (deploy_fixture_t){.next = 2};
    assert(av_deploy_finish(0, restart, guest_probe, host_probe, &fixture) == 0);
    assert(fixture.next == 3);
    fixture = (deploy_fixture_t){0};
    assert(av_deploy_finish(1, restart, guest_probe, host_probe, &fixture) == 1);
    assert(fixture.next == 0);
    assert(av_deploy_finish(0, NULL, guest_probe, host_probe, &fixture) == -1);
    assert(av_deploy_finish(0, restart, NULL, host_probe, &fixture) == -1);
    assert(av_deploy_finish(0, restart, guest_probe, NULL, &fixture) == -1);
    return 0;
}
