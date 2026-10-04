#ifndef WADDLE_COMMON_H
#define WADDLE_COMMON_H
#include "waddle/cli_protocol.h"
#include <stdint.h>
#include <stddef.h>
typedef struct { uint8_t *data; size_t off, len; } queue;
typedef struct {
    uint8_t header[32], *body;
    size_t have, length;
    uint32_t next;
    uint16_t type;
    int started;
} decoder;
int queue_init(queue *q);
void queue_free(queue *q);
size_t queue_space(const queue *q);
int queue_append(queue *q, const void *p, size_t n);
int queue_flush(queue *q, int fd);
int wire_send(queue *q, uint32_t *seq, uint16_t type, const void *p, size_t n);
int wire_stream(queue *q, uint32_t *seq, unsigned stream, const void *p, size_t n);
int wire_eof(queue *q, uint32_t *seq, unsigned stream);
/* 1: complete frame, 0: would block, -1: error, -2: clean transport EOF. */
int wire_read(decoder *d, int fd);
void wire_consume(decoder *d);
void wire_destroy(decoder *d);
int nonblock(int fd);
uint64_t monotonic_ms(void);
#endif
