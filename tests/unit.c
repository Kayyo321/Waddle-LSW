/**
 * @file unit.c
 * @brief Comprehensive unit tests for CLI wire framing, CRC-32, queues, quoting, and path translation.
 */

#include "common.h"
#include "path_rules.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_path_rules(void) {
    const path_rule_t rules[] = {{"/", "Z:\\"}, {"/home", "X:\\Users"}, {"/home/dev", "Y:\\work"}};
    char *mapped = waddle_translate_rules("/home/dev/file", rules, 3);
    assert(mapped && strcmp(mapped, "Y:\\work\\file") == 0);
    free(mapped);
    mapped = waddle_translate_rules("/home/device", rules, 3);
    assert(mapped && strcmp(mapped, "X:\\Users\\device") == 0);
    free(mapped);
    mapped = waddle_translate_rules("/other", rules, 3);
    assert(mapped && strcmp(mapped, "Z:\\other") == 0);
    free(mapped);
    assert(waddle_translate_rules("/other", rules + 1, 2) == NULL);
    assert(waddle_translate_rules(NULL, NULL, 0) == NULL);
    assert(waddle_translate_rules("/x", NULL, 1) == NULL);
    const path_rule_t invalid[] = {{NULL, "Z:\\"}, {"/", NULL}, {"bad", "Z:\\"}};
    for (size_t i = 0; i < 3; i++) {
        assert(waddle_translate_rules("/x", invalid + i, 1) == NULL);
        assert(errno == EINVAL);
    }
    assert(waddle_translate_rules("/x", rules, 65) == NULL);
    mapped = waddle_translate_rules("relative", rules, 3);
    assert(mapped && strcmp(mapped, "relative") == 0);
    free(mapped);
}

static void test_quoting(void) {
    /* Test null and empty argument edge cases */
    char *empty_quote = waddle_quote(NULL);
    assert(empty_quote != NULL);
    assert(empty_quote[0] == '\0');
    free(empty_quote);

    char *empty_args[] = {NULL};
    empty_quote = waddle_quote(empty_args);
    assert(empty_quote != NULL);
    assert(empty_quote[0] == '\0');
    free(empty_quote);

    assert(waddle_unquote(NULL) == NULL);

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
    assert(waddle_unquote("\"quote\"trailing") == NULL);

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

static void test_queue_operations(void) {
    assert(queue_init(NULL) == -1);
    assert(queue_space(NULL) == 0);
    assert(queue_append(NULL, "a", 1) == -1);

    queue_t q;
    assert(queue_init(&q) == 0);
    assert(queue_space(&q) == WaddleQueueSize);
    assert(queue_append(&q, "abc", 3) == 0);
    assert(q.len == 3);
    assert(queue_space(&q) == WaddleQueueSize - 3);

    /* Test compaction */
    q.off = WaddleQueueSize - 5;
    memcpy(q.data + q.off, "123", 3);
    q.len = 3;
    assert(queue_append(&q, "456", 3) == 0);
    assert(q.off == 0);
    assert(q.len == 6);
    assert(memcmp(q.data, "123456", 6) == 0);

    /* Test overflow */
    assert(queue_append(&q, q.data, queue_space(&q) + 1) == -1);
    assert(errno == ENOBUFS);

    /* Test queue flush */
    assert(queue_flush(NULL, -1) == 0);
    queue_t empty_q;
    memset(&empty_q, 0, sizeof(empty_q));
    assert(queue_flush(&empty_q, -1) == 0);

    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    assert(nonblock(fd[0]) == 0);
    assert(nonblock(fd[1]) == 0);

    assert(queue_flush(&q, fd[0]) == 0);
    assert(q.len == 0);
    assert(q.off == 0);

    char buf[16] = {0};
    assert(read(fd[1], buf, sizeof(buf)) == 6);
    assert(memcmp(buf, "123456", 6) == 0);

    close(fd[0]);
    close(fd[1]);
    queue_free(&q);
    queue_free(NULL);
}

static void test_frames(void) {
    uint32_t seq = 1;
    assert(wire_send(NULL, &seq, (uint16_t)WaddleMsgSignalEvent, "abcd", 4) == -1);

    queue_t q;
    assert(queue_init(&q) == 0);
    assert(wire_send(&q, &seq, (uint16_t)WaddleMsgSignalEvent, "abcd", 4) == 0);
    assert(seq == 2);

    /* Test stream and eof helpers */
    assert(wire_stream(&q, &seq, (unsigned int)WaddleStreamStdout, "chunk", 5) == 0);
    assert(wire_stream(&q, &seq, (unsigned int)WaddleStreamStdout, "too_big", WaddleChunkSize + 1) == -1);
    assert(wire_eof(&q, &seq, (unsigned int)WaddleStreamStderr) == 0);

    /* Clear queue for decode tests */
    q.len = 0;
    q.off = 0;
    seq = 1;
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

    /* Would block */
    assert(wire_read(&d, fd[1]) == 0);

    /* Corrupt CRC in wire header */
    q.data[24] = 2; /* Sequence 2 */
    q.data[28] ^= 1; /* Invert CRC bit */
    assert(write(fd[0], q.data, q.len) == (ssize_t)q.len);
    assert(wire_read(&d, fd[1]) == -1);
    wire_destroy(&d);

    /* Corrupt magic */
    waddle_put32(q.data, 0x11223344);
    assert(write(fd[0], q.data, WaddleHeaderSize) == (ssize_t)WaddleHeaderSize);
    assert(wire_read(&d, fd[1]) == -1);
    wire_destroy(&d);

    /* Oversized payload in wire header */
    waddle_put32(q.data, WaddleCliMagic);
    waddle_put32(q.data + 16, WaddleMaxPayloadSize + 1);
    assert(write(fd[0], q.data, WaddleHeaderSize) == (ssize_t)WaddleHeaderSize);
    assert(wire_read(&d, fd[1]) == -1);
    wire_destroy(&d);

    /* Empty payload message */
    q.len = 0;
    q.off = 0;
    seq = 1;
    assert(wire_send(&q, &seq, (uint16_t)WaddleMsgHeartbeatPing, NULL, 0) == 0);
    assert(write(fd[0], q.data, q.len) == (ssize_t)q.len);
    assert(wire_read(&d, fd[1]) == 1);
    assert(d.length == 0);
    wire_consume(&d);

    /* Clean EOF */
    close(fd[0]);
    assert(wire_read(&d, fd[1]) == -2);
    wire_destroy(&d);
    close(fd[1]);

    /* Partial frame then EOF */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    assert(write(fd[0], "bad", 3) == 3);
    close(fd[0]);
    assert(wire_read(&d, fd[1]) == -1);
    wire_destroy(&d);
    close(fd[1]);

    queue_free(&q);
    wire_consume(NULL);
    wire_destroy(NULL);
    assert(wire_read(NULL, -1) == -1);
}

int main(void) {
    test_path_rules();
    assert(waddle_crc32("123456789", 9) == UINT32_C(0xcbf43926));
    assert(waddle_crc32(NULL, 0) == 0);

    uint8_t b[8];
    waddle_put16(b, 0x1234);
    assert(waddle_get16(b) == 0x1234);

    waddle_put32(b, 0x12345678);
    assert(waddle_get32(b) == 0x12345678);

    waddle_put64(b, UINT64_C(0x0123456789abcdef));
    assert(b[0] == 0xef && b[7] == 1);
    assert(waddle_get64(b) == UINT64_C(0x0123456789abcdef));

    assert(monotonic_ms() > 0);

    test_quoting();
    test_queue_operations();
    test_frames();

    assert(waddle_translate_path(NULL) == NULL);

    char *p = waddle_translate_path("/home/dev/a b");
    assert(p != NULL && strcmp(p, "Z:\\home\\dev\\a b") == 0);
    free(p);

    p = waddle_translate_path("/");
    assert(p != NULL && strcmp(p, "Z:\\") == 0);
    free(p);

    p = waddle_translate_path("relative/file");
    assert(p != NULL && strcmp(p, "relative/file") == 0);
    free(p);

    assert(waddle_translate_path("/a/../b") == NULL);
    assert(waddle_translate_path("/a\\b") == NULL);

    puts("unit: layouts, CRC, queues, 7776 quoting cases, paths, fragmented/corrupt/truncated frames passed");
    return 0;
}
