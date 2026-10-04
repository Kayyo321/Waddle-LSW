#ifndef WADDLE_TERMINAL_H
#define WADDLE_TERMINAL_H
#include "common.h"
int waddle_terminal_init(int interactive);
int waddle_standard_nonblock(void);
void waddle_restore(void);
void waddle_terminal_close(void);
int waddle_signal_fd(void);
void waddle_drain_signals(void);
int waddle_send_pending(queue *tx, uint32_t *seq, int interactive);
#endif
