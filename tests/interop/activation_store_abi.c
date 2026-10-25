/** @file activation_store_abi.c @brief Independent C ABI consumer for the activation token store.
 *
 * Exercises selection, validity, the 30 s expiry boundary, duplicate
 * replacement, eviction, clock rollback and every status code through the
 * public header only. Links the Zig object built from activation_store.zig.
 */
#include "activation_store.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int put_text(interop_activation_store_t *store, const char *token, uint32_t process_id, uint64_t now_ms) {
    return interop_activation_put(store, (const uint8_t *)token, strlen(token), process_id, now_ms);
}

/** @brief Take into a local buffer and compare; returns the status. */
static int take_text(interop_activation_store_t *store, uint32_t process_id, uint64_t now_ms, const char *expected) {
    uint8_t out[InteropActivationMaxTokenBytes];
    size_t length = 0;
    int status = interop_activation_take(store, process_id, now_ms, out, &length);
    if (status == InteropActivationTakeFound) {
        assert(expected != NULL && length == strlen(expected));
        assert(memcmp(out, expected, length) == 0);
    }
    return status;
}

static void check_layout(void) {
    assert(sizeof(interop_activation_store_t) == InteropActivationStoreBytes);
    assert(sizeof(interop_activation_store_t) % 8u == 0u);
    assert(InteropActivationMaxEntries == 32u && InteropActivationTtlMs == 30000u);
}

static void check_put_take(void) {
    interop_activation_store_t store = { { 0 } };
    assert(interop_activation_store_init(&store) == 0);
    assert(interop_activation_store_init(NULL) == 1);
    assert(put_text(&store, "token-a", 0, 0) == InteropActivationPutInvalidProcess);
    assert(put_text(&store, "", 4, 0) == InteropActivationPutInvalidToken);
    assert(put_text(&store, "two words", 4, 0) == InteropActivationPutInvalidToken);
    assert(interop_activation_put(&store, NULL, 3, 4, 0) == InteropActivationPutInvalidArgument);
    assert(interop_activation_put(NULL, (const uint8_t *)"x", 1, 4, 0) == InteropActivationPutInvalidArgument);
    assert(put_text(&store, "token-a", 42, 1000) == InteropActivationPutOk);
    assert(take_text(&store, 43, 1001, NULL) == InteropActivationTakeNotFound);
    assert(take_text(&store, 0, 1001, NULL) == InteropActivationTakeInvalidProcess);
    assert(take_text(&store, 42, 1001, "token-a") == InteropActivationTakeFound);
    assert(take_text(&store, 42, 1002, NULL) == InteropActivationTakeNotFound);
    uint8_t out[InteropActivationMaxTokenBytes];
    size_t length = 0;
    assert(interop_activation_take(NULL, 42, 1003, out, &length) == InteropActivationTakeInvalidArgument);
    assert(interop_activation_take(&store, 42, 1003, NULL, &length) == InteropActivationTakeInvalidArgument);
    assert(interop_activation_take(&store, 42, 1003, out, NULL) == InteropActivationTakeInvalidArgument);
}

static void check_validity_bounds(void) {
    interop_activation_store_t store = { { 0 } };
    char longest[InteropActivationMaxTokenBytes + 1];
    memset(longest, 't', InteropActivationMaxTokenBytes);
    longest[InteropActivationMaxTokenBytes] = '\0';
    assert(put_text(&store, longest, 1, 0) == InteropActivationPutOk);
    assert(take_text(&store, 1, 0, longest) == InteropActivationTakeFound);
    char too_long[InteropActivationMaxTokenBytes + 2];
    memset(too_long, 't', InteropActivationMaxTokenBytes + 1);
    too_long[InteropActivationMaxTokenBytes + 1] = '\0';
    assert(interop_activation_put(&store, (const uint8_t *)too_long, InteropActivationMaxTokenBytes + 1u, 1, 0) == InteropActivationPutInvalidToken);
    assert(interop_activation_put(&store, (const uint8_t *)"\x7f", 1, 1, 0) == InteropActivationPutInvalidToken);
    assert(interop_activation_put(&store, (const uint8_t *)"\x20", 1, 1, 0) == InteropActivationPutInvalidToken);
    assert(interop_activation_put(&store, (const uint8_t *)"!", 1, 1, 0) == InteropActivationPutOk);
    assert(interop_activation_put(&store, (const uint8_t *)"~", 1, 2, 0) == InteropActivationPutOk);
}

static void check_expiry_boundary(void) {
    interop_activation_store_t live = { { 0 } };
    assert(put_text(&live, "edge", 7, 5000) == InteropActivationPutOk);
    assert(interop_activation_expire(&live, 5000u + InteropActivationTtlMs - 1u) == 0u);
    assert(take_text(&live, 7, 5000u + InteropActivationTtlMs - 1u, "edge") == InteropActivationTakeFound);
    interop_activation_store_t expiring = { { 0 } };
    assert(put_text(&expiring, "edge", 7, 5000) == InteropActivationPutOk);
    assert(take_text(&expiring, 7, 5000u + InteropActivationTtlMs, NULL) == InteropActivationTakeNotFound);
    assert(interop_activation_expire(&expiring, 5000u + InteropActivationTtlMs) == 0u);
}

static void check_duplicates_and_newest(void) {
    interop_activation_store_t store = { { 0 } };
    assert(put_text(&store, "dup", 1, 0) == InteropActivationPutOk);
    assert(put_text(&store, "dup", 2, 20000) == InteropActivationPutOk);
    assert(take_text(&store, 1, 20001, NULL) == InteropActivationTakeNotFound);
    assert(put_text(&store, "older", 2, 20010) == InteropActivationPutOk);
    assert(put_text(&store, "newer", 2, 20020) == InteropActivationPutOk);
    assert(take_text(&store, 2, 20030, "newer") == InteropActivationTakeFound);
    assert(take_text(&store, 2, 20031, "older") == InteropActivationTakeFound);
    assert(take_text(&store, 2, 20032, "dup") == InteropActivationTakeFound);
    assert(take_text(&store, 2, 20033, NULL) == InteropActivationTakeNotFound);
}

static void check_eviction(void) {
    interop_activation_store_t store = { { 0 } };
    char name[16];
    for (uint32_t index = 0; index < InteropActivationMaxEntries; index++) {
        snprintf(name, sizeof(name), "t%u", (unsigned)index);
        assert(put_text(&store, name, index + 1u, 100) == InteropActivationPutOk);
    }
    assert(put_text(&store, "fresh", 99, 100) == InteropActivationPutOk);
    assert(take_text(&store, 1, 100, NULL) == InteropActivationTakeNotFound);
    assert(take_text(&store, 2, 100, "t1") == InteropActivationTakeFound);
    /* The put expires all 31 stale pairs first, so a full store of stale entries evicts nothing. */
    assert(put_text(&store, "after-expiry", 500, 40000) == InteropActivationPutOk);
    assert(interop_activation_expire(&store, 40000) == 0u);
    assert(take_text(&store, 99, 40000, NULL) == InteropActivationTakeNotFound);
}

static void check_rollback(void) {
    interop_activation_store_t store = { { 0 } };
    assert(put_text(&store, "before", 9, 1000) == InteropActivationPutOk);
    assert(take_text(&store, 9, 500, NULL) == InteropActivationTakeNotFound);
    assert(put_text(&store, "fresh", 9, 600) == InteropActivationPutOk);
    assert(take_text(&store, 9, 601, "fresh") == InteropActivationTakeFound);
    assert(put_text(&store, "a", 3, 700) == InteropActivationPutOk);
    interop_activation_clear(&store);
    assert(take_text(&store, 3, 701, NULL) == InteropActivationTakeNotFound);
    interop_activation_clear(NULL);
    assert(interop_activation_expire(NULL, 0) == 0u);
}

static void check_pick(void) {
    const char *token = NULL;
    size_t length = 0;
    assert(interop_activation_pick_token("xdg-1", "startup", &token, &length) == InteropActivationPickFound);
    assert(length == 5u && memcmp(token, "xdg-1", 5) == 0);
    assert(interop_activation_pick_token("bad token", "startup", &token, &length) == InteropActivationPickFound);
    assert(length == 7u && memcmp(token, "startup", 7) == 0);
    assert(interop_activation_pick_token(NULL, "s2", &token, &length) == InteropActivationPickFound);
    assert(length == 2u && memcmp(token, "s2", 2) == 0);
    assert(interop_activation_pick_token("", "bad token", &token, &length) == InteropActivationPickNone);
    assert(interop_activation_pick_token(NULL, NULL, &token, &length) == InteropActivationPickNone);
    char longest[InteropActivationMaxTokenBytes + 2];
    memset(longest, 'q', InteropActivationMaxTokenBytes + 1);
    longest[InteropActivationMaxTokenBytes + 1] = '\0';
    assert(interop_activation_pick_token(longest, NULL, &token, &length) == InteropActivationPickNone);
    assert(interop_activation_pick_token(longest, "ok", &token, &length) == InteropActivationPickFound);
    assert(length == 2u);
    assert(interop_activation_pick_token("x", "y", NULL, &length) == InteropActivationPickInvalidArgument);
    assert(interop_activation_pick_token("x", "y", &token, NULL) == InteropActivationPickInvalidArgument);
}

static void check_clear_and_expire(void) {
    interop_activation_store_t store = { { 0 } };
    assert(put_text(&store, "one", 1, 0) == InteropActivationPutOk);
    assert(put_text(&store, "two", 2, 10) == InteropActivationPutOk);
    assert(interop_activation_expire(&store, 29999u) == 0u);
    assert(interop_activation_expire(&store, 30010u) == 2u);
    assert(put_text(&store, "three", 3, 30011) == InteropActivationPutOk);
    interop_activation_clear(&store);
    assert(take_text(&store, 3, 30012, NULL) == InteropActivationTakeNotFound);
}

int main(void) {
    check_layout();
    check_put_take();
    check_validity_bounds();
    check_expiry_boundary();
    check_duplicates_and_newest();
    check_eviction();
    check_rollback();
    check_pick();
    check_clear_and_expire();
    printf("activation store ABI: passed\n");
    return 0;
}
