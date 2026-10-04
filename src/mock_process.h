#ifndef WADDLE_MOCK_PROCESS_H
#define WADDLE_MOCK_PROCESS_H
#include "common.h"
#include <sys/types.h>
typedef struct {
    pid_t pid;
    int input, output[2], interactive, reaped, status;
    uint64_t started;
} mock_process;
int mock_spawn(mock_process *p, uint8_t *body, size_t length);
int mock_reap(mock_process *p);
void mock_stop(mock_process *p);
#endif
