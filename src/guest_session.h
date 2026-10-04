/** @file guest_session.h @brief Sequential guest session dispatch. */
#ifndef WaddleGuestSessionH
/** @brief Include guard; compile-time marker with no storage or ownership. */
#define WaddleGuestSessionH
#include "guest_wire.h"
/** @brief Run exactly one guest command and release its child tree and workers.
 * @param[in] socket Borrowed connected blocking socket; listener closes after return.
 * @return 0 completed/spawn rejected, -1 transport/protocol/worker failure.
 * @note Single main caller per socket; child, buffers, events and worker handles
 * are session-owned and released before return. Inherit no session resources. */
int guest_session(SOCKET socket);
#endif
