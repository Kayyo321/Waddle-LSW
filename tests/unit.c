/**
 * @file unit.c
 * @brief Unit tests for CLI wire framing, CRC-32 checksums, argument quoting, and path translation.
 */

#include "common.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_quoting(void) {
    char *cases[] = {
        "",
        "plain",
        "hello world",
        "foo\"bar",
        "C:\\Program Files\\",
        "a\\\\\"b",
        "tab\tvalue",
        "\\",
        "日本語",
        NULL
    };

    char *q = waddle_quote(cases);
    assert(q != NULL);
    char **a = waddle_unquote(q);
    assert(a != NULL);
    for (size_t i = 0; cases[i] != NULL; i++) {
        assert(a[i] != NULL && strcmp(a[i], cases[i]) == 0);
    }
    assert(a[9] == NULL);
    waddle_free_argv(a);
    free(q);

    char *one[] = {"C:\\Program Files\\", NULL};
    q = waddle_quote(one);
    assert(q != NULL);
    assert(strcmp(q, "\"C:\\Program Files\\\\\"") == 0);
    free(q);

    assert(waddle_unquote("unquoted") == NULL);
    assert(waddle_unquote("\"unterminated") == NULL);

    /* Exhaust all short strings of quote-sensitive characters. */
    const char alphabet[] = "a \\\"\t";
    for (unsigned int n = 0; n < 7776; n++) {
        unsigned int v = n;
        char s[6];
        for (unsigned int i = 0; i < 5; i++) {
            s[i] = alphabet[v % 6];
            v /= 6;
        }
        s[5] = '\0';

        char *args[] = {s, NULL};
        q = waddle_quote(args);
        assert(q != NULL);
        a = waddle_unquote(q);
        assert(a != NULL && strcmp(a[0], s) == 0 && a[1] == NULL);
        free(q);
        waddle_free_argv(a);
    }
}

static void test_frames(void) {
    queue_t q;
    assert(queue_init(&q) == 0);
    uint32_t seq = 1;
    assert(wire_send(&q, &seq, (uint16_t)WaddleMsgSignalEvent, "abcd", 4) == 0);

    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    assert(nonblock(fd[1]) == 0);

    decoder_t d;
    memset(&d, 0, sizeof(d));

    for (size_t i = 0; i < q.len; i++) {
        assert(write(fd[0], q.data + i, 1) == 1);
        int res = wire_read(&d, fd[1]);
        assert(res == (i == q.len - 1 ? 1 : 0));
    }

    assert(d.type == (uint16_t)WaddleMsgSignalEvent && d.length == 4 && memcmp(d.body, "abcd", 4) == 0);
    wire_consume(&d);

    /* Corrupt CRC in wire header */
    q.data[24] = 2; /* Sequence 2 */
    q.data[28] ^= 1; /* Invert CRC bit */
    assert(write(fd[0], q.data, q.len) == (ssize_t)q.len);
    assert(wire_read(&d, fd[1]) == -1);
    wire_destroy(&d);

    /* Oversized payload in wire header */
    waddle_put32(q.data + 16, WaddleMaxPayloadSize + 1);
    assert(write(fd[0], q.data, WaddleHeaderSize) == (ssize_t)WaddleHeaderSize);
    assert(wire_read(&d, fd[1]) == -1);
    wire_destroy(&d);

    close(fd[0]);
    close(fd[1]);
    queue_free(&q);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    assert(write(fd[0], "bad", 3) == 3);
    close(fd[0]);
    assert(wire_read(&d, fd[1]) == -1);
    wire_destroy(&d);
    close(fd[1]);
}

int main(void) {
    assert(waddle_crc32("123456789", 9) == UINT32_C(0xcbf43926));
    assert(waddle_crc32(NULL, 0) == 0);

    uint8_t b[8];
    waddle_put64(b, UINT64_C(0x0123456789abcdef));
    assert(b[0] == 0xef && b[7] == 1);
    assert(waddle_get64(b) == UINT64_C(0x0123456789abcdef));

    test_quoting();
    test_frames();

    char *p = waddle_translate_path("/home/dev/a b");
    assert(p != NULL && strcmp(p, "Z:\\home\\dev\\a b") == 0);
    free(p);

    p = waddle_translate_path("relative/file");
    assert(p != NULL && strcmp(p, "relative/file") == 0);
    free(p);

    assert(waddle_translate_path("/a/../b") == NULL);
    assert(waddle_translate_path("/a\\b") == NULL);

    puts("unit: layouts, CRC, 7776 quoting cases, paths, fragmented/corrupt/truncated frames passed");
    return 0;
}
