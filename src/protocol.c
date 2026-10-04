/**
 * @file protocol.c
 * @brief Wire protocol serialization, CRC-32 checksums, queue buffering, and frame decoders.
 */

#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint16_t waddle_get16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

uint32_t waddle_get32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

uint64_t waddle_get64(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint64_t)waddle_get32(b) | ((uint64_t)waddle_get32(b + 4) << 32);
}

void waddle_put16(void *p, uint16_t n) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(n & 0xff);
    b[1] = (uint8_t)((n >> 8) & 0xff);
}

void waddle_put32(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(n & 0xff);
    b[1] = (uint8_t)((n >> 8) & 0xff);
    b[2] = (uint8_t)((n >> 16) & 0xff);
    b[3] = (uint8_t)((n >> 24) & 0xff);
}

void waddle_put64(void *p, uint64_t n) {
    uint8_t *b = (uint8_t *)p;
    waddle_put32(b, (uint32_t)(n & UINT32_MAX));
    waddle_put32(b + 4, (uint32_t)((n >> 32) & UINT32_MAX));
}

uint32_t waddle_crc32(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t crc = UINT32_MAX;
    while (n--) {
        crc ^= *b++;
        for (unsigned int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

int queue_init(queue_t *q) {
    if (q == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(q, 0, sizeof(*q));
    q->data = (uint8_t *)malloc(WaddleQueueSize);
    if (q->data == NULL) {
        return -1;
    }
    q->off = 0;
    q->len = 0;
    return 0;
}

void queue_free(queue_t *q) {
    if (q != NULL) {
        free(q->data);
        q->data = NULL;
        q->off = 0;
        q->len = 0;
    }
}

size_t queue_space(const queue_t *q) {
    if (q == NULL || q->len >= WaddleQueueSize) {
        return 0;
    }
    return WaddleQueueSize - q->len;
}

int queue_append(queue_t *q, const void *p, size_t n) {
    if (q == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (n > queue_space(q)) {
        errno = ENOBUFS;
        return -1;
    }
    /* Compact buffer if we need contiguous space at tail */
    if (q->off + q->len + n > WaddleQueueSize) {
        if (q->len > 0) {
            memmove(q->data, q->data + q->off, q->len);
        }
        q->off = 0;
    }
    if (n > 0 && p != NULL) {
        memcpy(q->data + q->off + q->len, p, n);
    }
    q->len += n;
    return 0;
}

int queue_flush(queue_t *q, int fd) {
    if (q == NULL || q->len == 0) {
        return 0;
    }
    ssize_t n = write(fd, q->data + q->off, q->len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (n == 0) {
        errno = EIO;
        return -1;
    }
    q->off += (size_t)n;
    q->len -= (size_t)n;
    if (q->len == 0) {
        q->off = 0;
    }
    return 0;
}

int wire_send(queue_t *q, uint32_t *seq, uint16_t type, const void *p, size_t n) {
    if (q == NULL || seq == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (n > WaddleMaxPayloadSize || queue_space(q) < n + WaddleHeaderSize) {
        errno = ENOBUFS;
        return -1;
    }

    uint8_t h[WaddleHeaderSize] = {0};
    waddle_put32(h, WaddleCliMagic);
    waddle_put16(h + 4, WaddleCliVersion);
    waddle_put16(h + 6, type);
    waddle_put64(h + 8, 1); /* Session ID */
    waddle_put32(h + 16, (uint32_t)n);
    waddle_put32(h + 20, 0); /* Flags */
    waddle_put32(h + 24, (*seq)++);
    waddle_put32(h + 28, waddle_crc32(p, n));

    if (queue_append(q, h, WaddleHeaderSize) != 0) {
        return -1;
    }
    if (n > 0 && queue_append(q, p, n) != 0) {
        return -1;
    }
    return 0;
}

int wire_stream(queue_t *q, uint32_t *seq, unsigned int stream, const void *p, size_t n) {
    if (n > WaddleChunkSize) {
        errno = EINVAL;
        return -1;
    }

    uint8_t b[WaddleChunkSize + 8] = {0};
    b[0] = (uint8_t)stream;
    waddle_put32(b + 4, (uint32_t)n);
    if (n > 0 && p != NULL) {
        memcpy(b + 8, p, n);
    }
    return wire_send(q, seq, (uint16_t)WaddleMsgStreamData, b, n + 8);
}

int wire_eof(queue_t *q, uint32_t *seq, unsigned int stream) {
    uint8_t b[4];
    waddle_put32(b, stream);
    return wire_send(q, seq, (uint16_t)WaddleMsgStreamEof, b, sizeof(b));
}

int wire_read(decoder_t *d, int fd) {
    if (d == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!d->started) {
        d->next = 1;
        d->started = 1;
    }

    for (;;) {
        size_t target = (d->body != NULL) ? (d->length + WaddleHeaderSize) : WaddleHeaderSize;
        if (d->have < target) {
            uint8_t *dest = (d->body != NULL) ? (d->body + d->have - WaddleHeaderSize)
                                              : (d->header + d->have);
            size_t remaining = target - d->have;
            ssize_t n = read(fd, dest, remaining);
            if (n == 0) {
                if (d->have > 0) {
                    errno = EPROTO;
                    return -1;
                }
                return -2; /* Clean EOF */
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0; /* More data needed */
                }
                return -1;
            }
            d->have += (size_t)n;
            if (d->have < target) {
                continue;
            }
        }

        if (d->body == NULL) {
            const uint8_t *h = d->header;
            uint32_t magic = waddle_get32(h);
            uint16_t version = waddle_get16(h + 4);
            uint64_t session = waddle_get64(h + 8);
            uint32_t flags = waddle_get32(h + 20);
            uint32_t seq = waddle_get32(h + 24);
            uint32_t payload_len = waddle_get32(h + 16);

            if (magic != WaddleCliMagic || version != WaddleCliVersion || session != 1 ||
                flags != 0 || seq != d->next || payload_len > WaddleMaxPayloadSize) {
                errno = EPROTO;
                return -1;
            }

            d->length = payload_len;
            d->type = waddle_get16(h + 6);
            d->body = (uint8_t *)malloc(d->length > 0 ? d->length : 1);
            if (d->body == NULL) {
                return -1;
            }
            if (d->length > 0) {
                continue;
            }
        }

        uint32_t expected_crc = waddle_get32(d->header + 28);
        uint32_t actual_crc = waddle_crc32(d->body, d->length);
        if (actual_crc != expected_crc) {
            errno = EPROTO;
            return -1;
        }
        return 1; /* Complete frame ready */
    }
}

void wire_consume(decoder_t *d) {
    if (d != NULL) {
        free(d->body);
        d->body = NULL;
        d->have = 0;
        d->length = 0;
        d->next++;
    }
}

void wire_destroy(decoder_t *d) {
    if (d != NULL) {
        free(d->body);
        d->body = NULL;
        memset(d, 0, sizeof(*d));
    }
}

int nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000);
}
