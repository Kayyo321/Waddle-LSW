/** @file legacy_fixture.c @brief Isolated pinned old peer for asymmetric upgrade tests.
 * The historical peer and codec retain their original wire rejection behavior;
 * symbol renaming is the only runtime adaptation. No production link uses them.
 */
#define av_control_decode legacy_av_control_decode
#define av_control_encode legacy_av_control_encode
#define av_peer_send legacy_av_peer_send
#define av_peer_pump legacy_av_peer_pump
#include "legacy/av_peer.c"
static unsigned notifications;
static int unexpected_message(const av_message_t *message, void *context) {
    (void)message; (void)context;
    ++notifications;
    return 99; /* Unknown CreateV2 must fail in decoder, before this callback. */
}
/** @brief Pump complete new-guest first frame with the frozen old host peer.
 * @param[in] socket Borrowed nonblocking socket containing one complete frame.
 * @return Old peer status; -1 means decoder rejected. No allocation/ownership transfer.
 * @note Test-thread only; this wrapper represents one old-host event-loop iteration.
 */
int av_legacy_pump(uintptr_t socket) {
    av_peer_t peer = {.socket = socket};
    notifications = 0;
    int result = av_peer_pump(&peer, unexpected_message, NULL);
    return notifications ? -2 : result;
}
