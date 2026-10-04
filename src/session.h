#ifndef WADDLE_SESSION_H
#define WADDLE_SESSION_H
#include "common.h"
int waddle_session(int fd, queue *tx, uint32_t seq, uint64_t deadline, int interactive);
#endif
