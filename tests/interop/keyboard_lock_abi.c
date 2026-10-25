/** @file keyboard_lock_abi.c @brief Independent C consumer for the keyboard lock-state C ABI. */
#include "keyboard_lock.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(waddle_keyboard_lock_modifiers_t) == 12, "modifiers layout");
_Static_assert(sizeof(waddle_keyboard_lock_indices_t) == 3, "indices layout");
_Static_assert(sizeof(waddle_keyboard_lock_host_t) == 2, "host state layout");
_Static_assert(sizeof(waddle_keyboard_lock_host_decision_t) == 2, "host decision layout");
_Static_assert(sizeof(waddle_keyboard_lock_guest_t) == 24, "guest state layout");
_Static_assert(sizeof(waddle_keyboard_lock_action_t) == 4, "action layout");
_Static_assert(sizeof(waddle_keyboard_lock_plan_t) == 26, "plan layout");
_Static_assert(sizeof(waddle_keyboard_lock_update_t) == 28, "update layout");

static const waddle_keyboard_lock_indices_t indices_default = {0, 1, 2};

static void test_mask_and_null_arguments(void) {
    waddle_keyboard_lock_modifiers_t mods = {0, 0, 0x7u << 0};
    uint8_t mask = 0xEEu;
    assert(waddle_keyboard_lock_mask(&mods, &indices_default, &mask) == WaddleKeyboardLockOk);
    assert(mask == WaddleKeyboardLockAll);
    mask = 0xEEu;
    assert(waddle_keyboard_lock_mask(NULL, &indices_default, &mask) == WaddleKeyboardLockNullArgument);
    assert(waddle_keyboard_lock_mask(&mods, NULL, &mask) == WaddleKeyboardLockNullArgument);
    assert(waddle_keyboard_lock_mask(&mods, &indices_default, NULL) == WaddleKeyboardLockNullArgument);
    assert(mask == 0xEEu);

    waddle_keyboard_lock_indices_t bad = {0, 32, 2};
    assert(waddle_keyboard_lock_mask(&mods, &bad, &mask) == WaddleKeyboardLockInvalidIndex);
    assert(mask == 0xEEu);
    waddle_keyboard_lock_indices_t absent = {
        WaddleKeyboardLockModifierAbsent, WaddleKeyboardLockModifierAbsent, WaddleKeyboardLockModifierAbsent};
    assert(waddle_keyboard_lock_mask(&mods, &absent, &mask) == WaddleKeyboardLockOk);
    assert(mask == 0u);
}

static void test_host_session(void) {
    waddle_keyboard_lock_host_t host;
    memset(&host, 0, sizeof(host));
    waddle_keyboard_lock_modifiers_t num_on = {0, 0, 1u << 1};
    waddle_keyboard_lock_modifiers_t caps_on = {0, 0, 1u << 0};
    waddle_keyboard_lock_host_decision_t decision = {false, 0};

    assert(waddle_keyboard_lock_host_observe(&host, &num_on, &indices_default, &decision) == WaddleKeyboardLockOk);
    assert(!decision.send && !host.focused);
    assert(waddle_keyboard_lock_host_enter(NULL, &indices_default) == WaddleKeyboardLockNullArgument);
    assert(waddle_keyboard_lock_host_enter(&host, NULL) == WaddleKeyboardLockNullArgument);
    waddle_keyboard_lock_indices_t bad = {0, 1, 99};
    assert(waddle_keyboard_lock_host_enter(&host, &bad) == WaddleKeyboardLockInvalidIndex);
    assert(!host.focused && !host.pending_sync);

    assert(waddle_keyboard_lock_host_enter(&host, &indices_default) == WaddleKeyboardLockOk);
    assert(host.focused && host.pending_sync);
    assert(waddle_keyboard_lock_host_observe(&host, &num_on, &indices_default, &decision) == WaddleKeyboardLockOk);
    assert(decision.send && decision.mask == WaddleKeyboardLockNum);
    assert(!host.pending_sync);

    assert(waddle_keyboard_lock_host_observe(&host, &num_on, &indices_default, &decision) == WaddleKeyboardLockOk);
    assert(!decision.send);
    assert(waddle_keyboard_lock_host_observe(&host, &caps_on, &indices_default, &decision) == WaddleKeyboardLockOk);
    assert(!decision.send && decision.mask == WaddleKeyboardLockCaps);

    decision.mask = 0x55u;
    assert(waddle_keyboard_lock_host_observe(&host, &caps_on, &bad, &decision) == WaddleKeyboardLockInvalidIndex);
    assert(decision.mask == 0x55u && !host.pending_sync);

    assert(waddle_keyboard_lock_host_leave(NULL) == WaddleKeyboardLockNullArgument);
    assert(waddle_keyboard_lock_host_leave(&host) == WaddleKeyboardLockOk);
    assert(!host.focused && !host.pending_sync);
    assert(waddle_keyboard_lock_host_observe(&host, &num_on, &indices_default, &decision) == WaddleKeyboardLockOk);
    assert(!decision.send);
}

static void test_guest_gate_and_plan(void) {
    waddle_keyboard_lock_guest_t guest;
    memset(&guest, 0, sizeof(guest));
    waddle_keyboard_lock_update_t update;
    memset(&update, 0x5a, sizeof(update));

    assert(waddle_keyboard_lock_guest_update(NULL, 1, 1, 1, 0, 0, &update) == WaddleKeyboardLockNullArgument);
    assert(waddle_keyboard_lock_guest_update(&guest, 1, 1, 1, 0, 0, NULL) == WaddleKeyboardLockNullArgument);
    assert(waddle_keyboard_lock_guest_update(&guest, 0, 0, 1, 0, 0, &update) == WaddleKeyboardLockInvalidEpoch);
    assert(waddle_keyboard_lock_guest_update(&guest, 1, 1, 0, 0, 0, &update) == WaddleKeyboardLockInvalidSerial);
    assert(waddle_keyboard_lock_guest_update(&guest, 1, 1, 1, 8, 0, &update) == WaddleKeyboardLockInvalidMask);
    assert(waddle_keyboard_lock_guest_update(&guest, 1, 1, 1, 0, 0x80, &update) == WaddleKeyboardLockInvalidMask);
    assert(!guest.accepted);

    assert(waddle_keyboard_lock_guest_update(&guest, 3, 3, 100, WaddleKeyboardLockAll, 0, &update) == WaddleKeyboardLockOk);
    assert(update.applied && update.plan.count == 6);
    assert(update.plan.actions[0].vk == 0x14 && update.plan.actions[0].pressed);
    assert(update.plan.actions[1].vk == 0x14 && !update.plan.actions[1].pressed);
    assert(update.plan.actions[4].vk == 0x91 && update.plan.actions[4].pressed);

    assert(waddle_keyboard_lock_guest_update(&guest, 3, 3, 100, 0, 0, &update) == WaddleKeyboardLockOk);
    assert(!update.applied && update.plan.count == 0);
    assert(waddle_keyboard_lock_guest_update(&guest, 3, 3, 99, 0, 0, &update) == WaddleKeyboardLockOk);
    assert(!update.applied);
    assert(waddle_keyboard_lock_guest_update(&guest, 3, 2, 101, 0, 0, &update) == WaddleKeyboardLockEpochMismatch);
    assert(waddle_keyboard_lock_guest_update(&guest, 4, 4, 101, 0, WaddleKeyboardLockNum, &update) == WaddleKeyboardLockOk);
    assert(update.applied && update.plan.count == 2 && update.plan.actions[0].vk == 0x90);
    assert(guest.epoch == 4 && guest.serial == 101);
}

static void test_plan_only(void) {
    waddle_keyboard_lock_plan_t plan;
    memset(&plan, 0x3c, sizeof(plan));
    assert(waddle_keyboard_lock_plan_toggles(WaddleKeyboardLockScroll, 0, NULL) == WaddleKeyboardLockNullArgument);
    assert(waddle_keyboard_lock_plan_toggles(0x10, 0, &plan) == WaddleKeyboardLockInvalidMask);
    assert(waddle_keyboard_lock_plan_toggles(WaddleKeyboardLockScroll, WaddleKeyboardLockScroll, &plan) == WaddleKeyboardLockOk);
    assert(plan.count == 0);
    assert(waddle_keyboard_lock_plan_toggles(WaddleKeyboardLockScroll, 0, &plan) == WaddleKeyboardLockOk);
    assert(plan.count == 2 && plan.actions[0].vk == 0x91 && plan.actions[0].pressed && !plan.actions[1].pressed);
}

int main(void) {
    test_mask_and_null_arguments();
    test_host_session();
    test_guest_gate_and_plan();
    test_plan_only();
    return 0;
}
