/**
 * @file window_observer_graph_faults.c
 * @brief Public-API fault tests for the Windows observer's real graph boundary.
 *
 * The fixture owns a genuine graph, a genuine opaque observer, and the same scriptable native-API
 * double used by the ordinary observer suite. GNU ld wraps only documented graph ABI calls; each
 * wrapper either forwards to the real graph or returns one selected status without modifying graph
 * ownership. This tests error containment, not capture, pin-release, or host-completion authority.
 */
#include "window_observer_api_double.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern interop_window_graph_status_t __real_interop_window_graph_stamp_high_water(
    const interop_window_graph_t *, uint64_t *);
extern interop_window_graph_status_t __real_interop_window_graph_get_stats(
    const interop_window_graph_t *, interop_window_graph_stats_t *);
extern interop_window_graph_status_t __real_interop_window_graph_invalidate_all(
    interop_window_graph_t *, uint64_t, uint32_t *);
extern interop_window_graph_status_t __real_interop_window_graph_list_tracked_windows(
    const interop_window_graph_t *, uint64_t, uint64_t *, uint32_t, uint32_t *, uint32_t *);
extern interop_window_graph_status_t __real_interop_window_graph_note_destroyed(
    interop_window_graph_t *, uint64_t, uint64_t, uint64_t, uint32_t,
    interop_window_observation_t *);
extern interop_window_graph_status_t __real_interop_window_graph_observe(
    interop_window_graph_t *, uint64_t, const interop_window_facts_t *,
    interop_window_observation_t *);
extern interop_window_graph_status_t __real_interop_window_graph_refresh_begin(
    interop_window_graph_t *, uint64_t);
extern interop_window_graph_status_t __real_interop_window_graph_refresh_abort(
    interop_window_graph_t *, uint64_t);
extern interop_window_graph_status_t __real_interop_window_graph_refresh_offer(
    interop_window_graph_t *, uint64_t, const interop_window_facts_t *,
    interop_window_observation_t *);
extern interop_window_graph_status_t __real_interop_window_graph_refresh_keep(
    interop_window_graph_t *, uint64_t, uint64_t);
extern interop_window_graph_status_t __real_interop_window_graph_refresh_unseen(
    interop_window_graph_t *, uint64_t, uint64_t *, uint32_t, uint32_t *);
extern interop_window_graph_status_t __real_interop_window_graph_refresh_commit(
    interop_window_graph_t *, uint64_t, uint32_t *);

/** Graph call selector for one-shot, status-preserving test injection. */
typedef enum graph_call_t {
    GraphCallNone = 0,
    GraphCallStamp = 1,
    GraphCallStats = 2,
    GraphCallInvalidate = 3,
    GraphCallList = 4,
    GraphCallDestroy = 5,
    GraphCallObserve = 6,
    GraphCallRefreshBegin = 7,
    GraphCallRefreshAbort = 8,
    GraphCallRefreshOffer = 9,
    GraphCallRefreshKeep = 10,
    GraphCallRefreshUnseen = 11,
    GraphCallRefreshCommit = 12
} graph_call_t;

/** Structurally valid list results that violate the observer's snapshot contract. */
typedef enum malformed_snapshot_t {
    MalformedSnapshotNone = 0,
    MalformedSnapshotCountOverCapacity = 1,
    MalformedSnapshotFreeOverCapacity = 2,
    MalformedSnapshotCombinedOverCapacity = 3,
    MalformedSnapshotZeroWindow = 4,
    MalformedSnapshotDuplicateWindows = 5,
    MalformedSnapshotUnsortedWindows = 6,
    MalformedRefreshObservationSuspended = 7,
    MalformedObserveSuspended = 8
} malformed_snapshot_t;

/** One armed failure or malformed-success response. */
typedef struct graph_fault_t {
    graph_call_t call;
    uint32_t ordinal;
    uint32_t seen;
    interop_window_graph_status_t status;
    uint32_t malformed_success_kind;
} graph_fault_t;

/** Fixture that owns all real test state until close_fixture transfers observer evidence. */
typedef struct fixture_t {
    api_double_t *api_double;
    interop_window_graph_t *graph;
    window_observer_t *observer;
} fixture_t;

static int fault_graph_key(void *context, const interop_window_facts_t *facts, uint8_t *key,
                           uint32_t capacity, uint32_t *length) {
    (void)context;
    (void)facts;
    if (capacity < 6u || key == NULL || length == NULL) return 1;
    memcpy(key, "a-test", 6u);
    *length = 6u;
    return 0;
}

static graph_fault_t fault_state;
static fixture_t retained_fixture;

static int retain_fixture_on_cleanup_failure(fixture_t *fixture) {
    retained_fixture = *fixture;
    memset(fixture, 0, sizeof(*fixture));
    return 0;
}

static int fault_now(graph_call_t call, interop_window_graph_status_t *status,
                     uint32_t *malformed_success) {
    if (fault_state.call != call) return 0;
    fault_state.seen += 1u;
    if (fault_state.seen != fault_state.ordinal) return 0;
    fault_state.call = GraphCallNone;
    if (malformed_success != NULL) *malformed_success = fault_state.malformed_success_kind;
    if (status != NULL) *status = fault_state.status;
    return 1;
}

interop_window_graph_status_t __wrap_interop_window_graph_stamp_high_water(
    const interop_window_graph_t *graph, uint64_t *stamp) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallStamp, &status, NULL)) return status;
    return __real_interop_window_graph_stamp_high_water(graph, stamp);
}

interop_window_graph_status_t __wrap_interop_window_graph_get_stats(
    const interop_window_graph_t *graph, interop_window_graph_stats_t *stats) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallStats, &status, NULL)) return status;
    return __real_interop_window_graph_get_stats(graph, stats);
}

interop_window_graph_status_t __wrap_interop_window_graph_invalidate_all(
    interop_window_graph_t *graph, uint64_t epoch, uint32_t *retired) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallInvalidate, &status, NULL)) return status;
    return __real_interop_window_graph_invalidate_all(graph, epoch, retired);
}

interop_window_graph_status_t __wrap_interop_window_graph_list_tracked_windows(
    const interop_window_graph_t *graph, uint64_t epoch, uint64_t *windows, uint32_t capacity,
    uint32_t *count, uint32_t *free_count) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    uint32_t malformed = 0u;
    if (fault_now(GraphCallList, &status, &malformed)) {
        if (status != InteropWindowGraphStatusOk) return status;
        if (malformed != MalformedSnapshotNone) {
            if (count != NULL) *count = 0u;
            if (free_count != NULL) *free_count = 0u;
            if (malformed == MalformedSnapshotCountOverCapacity) {
                if (count != NULL) *count = capacity + 1u;
            } else if (malformed == MalformedSnapshotFreeOverCapacity) {
                if (free_count != NULL) *free_count = capacity + 1u;
            } else if (malformed == MalformedSnapshotCombinedOverCapacity) {
                if (count != NULL) *count = 1u;
                if (free_count != NULL) *free_count = capacity;
            } else if (malformed == MalformedSnapshotZeroWindow ||
                       malformed == MalformedSnapshotDuplicateWindows ||
                       malformed == MalformedSnapshotUnsortedWindows) {
                if (count != NULL) *count = 2u;
                if (free_count != NULL) *free_count = capacity - 2u;
                if (windows != NULL) {
                    windows[0] = UINT64_C(0x101);
                    windows[1] = UINT64_C(0x102);
                    if (malformed == MalformedSnapshotZeroWindow) windows[0] = 0u;
                    if (malformed == MalformedSnapshotDuplicateWindows) windows[1] = windows[0];
                    if (malformed == MalformedSnapshotUnsortedWindows) {
                        windows[0] = UINT64_C(0x102);
                        windows[1] = UINT64_C(0x101);
                    }
                }
            }
            return InteropWindowGraphStatusOk;
        }
    }
    return __real_interop_window_graph_list_tracked_windows(graph, epoch, windows, capacity,
                                                            count, free_count);
}

interop_window_graph_status_t __wrap_interop_window_graph_note_destroyed(
    interop_window_graph_t *graph, uint64_t epoch, uint64_t window, uint64_t stamp,
    uint32_t event_time, interop_window_observation_t *observation) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallDestroy, &status, NULL)) return status;
    return __real_interop_window_graph_note_destroyed(graph, epoch, window, stamp,
                                                      event_time, observation);
}

interop_window_graph_status_t __wrap_interop_window_graph_observe(
    interop_window_graph_t *graph, uint64_t epoch, const interop_window_facts_t *facts,
    interop_window_observation_t *observation) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    uint32_t malformed = 0u;
    if (fault_now(GraphCallObserve, &status, &malformed)) {
        if (status != InteropWindowGraphStatusOk) return status;
        status = __real_interop_window_graph_observe(graph, epoch, facts, observation);
        if (status == InteropWindowGraphStatusOk && malformed == MalformedObserveSuspended)
            observation->disposition = InteropWindowDispositionSuspended;
        return status;
    }
    return __real_interop_window_graph_observe(graph, epoch, facts, observation);
}

interop_window_graph_status_t __wrap_interop_window_graph_refresh_begin(
    interop_window_graph_t *graph, uint64_t epoch) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallRefreshBegin, &status, NULL)) return status;
    return __real_interop_window_graph_refresh_begin(graph, epoch);
}

interop_window_graph_status_t __wrap_interop_window_graph_refresh_abort(
    interop_window_graph_t *graph, uint64_t epoch) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallRefreshAbort, &status, NULL)) return status;
    return __real_interop_window_graph_refresh_abort(graph, epoch);
}

interop_window_graph_status_t __wrap_interop_window_graph_refresh_offer(
    interop_window_graph_t *graph, uint64_t epoch, const interop_window_facts_t *facts,
    interop_window_observation_t *observation) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    uint32_t malformed = 0u;
    if (fault_now(GraphCallRefreshOffer, &status, &malformed)) {
        if (status != InteropWindowGraphStatusOk) return status;
        status = __real_interop_window_graph_refresh_offer(graph, epoch, facts, observation);
        if (status == InteropWindowGraphStatusOk &&
            malformed == MalformedRefreshObservationSuspended)
            observation->disposition = InteropWindowDispositionSuspended;
        return status;
    }
    return __real_interop_window_graph_refresh_offer(graph, epoch, facts, observation);
}

interop_window_graph_status_t __wrap_interop_window_graph_refresh_keep(
    interop_window_graph_t *graph, uint64_t epoch, uint64_t window) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallRefreshKeep, &status, NULL)) return status;
    return __real_interop_window_graph_refresh_keep(graph, epoch, window);
}

interop_window_graph_status_t __wrap_interop_window_graph_refresh_unseen(
    interop_window_graph_t *graph, uint64_t epoch, uint64_t *windows, uint32_t capacity,
    uint32_t *count) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallRefreshUnseen, &status, NULL)) return status;
    return __real_interop_window_graph_refresh_unseen(graph, epoch, windows, capacity, count);
}

interop_window_graph_status_t __wrap_interop_window_graph_refresh_commit(
    interop_window_graph_t *graph, uint64_t epoch, uint32_t *retired) {
    interop_window_graph_status_t status = InteropWindowGraphStatusOk;
    if (fault_now(GraphCallRefreshCommit, &status, NULL)) return status;
    return __real_interop_window_graph_refresh_commit(graph, epoch, retired);
}

#define Check(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failed = 1; \
        goto done; \
    } \
} while (0)

static int fixture_open_with_stamp(fixture_t *fixture, int start, uint64_t first_stamp) {
    memset(fixture, 0, sizeof(*fixture));
    memset(&fault_state, 0, sizeof(fault_state));
    fixture->api_double = api_double_new();
    if (fixture->api_double == NULL) return 0;
    interop_window_graph_config_t graph_config;
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(5);
    graph_config.app_key_lookup = fault_graph_key;
    if (interop_window_graph_create(&graph_config, &fixture->graph) != InteropWindowGraphStatusOk)
        return 0;
    window_observer_config_t observer_config;
    api_double_config(&observer_config, fixture->api_double, 0u, first_stamp);
    if (window_observer_create(&observer_config, &fixture->observer) != WindowObserverStatusOk)
        return 0;
    if (window_observer_bind_graph(fixture->observer, fixture->graph, UINT64_C(5)) !=
        WindowObserverStatusOk) return 0;
    return !start || window_observer_start(fixture->observer) == WindowObserverStatusOk;
}

static int fixture_open(fixture_t *fixture, int start) {
    return fixture_open_with_stamp(fixture, start, 1u);
}

static int fixture_close(fixture_t *fixture) {
    if (fixture == NULL) return 0;
    memset(&fault_state, 0, sizeof(fault_state));
    if (fixture->observer != NULL) {
        window_observer_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        if (window_observer_status(fixture->observer, &stats) != WindowObserverStatusOk)
            return retain_fixture_on_cleanup_failure(fixture);
        if (stats.state == WindowObserverStateCreated || stats.state == WindowObserverStateStarted ||
            stats.state == WindowObserverStateFailed || stats.state == WindowObserverStateStopping ||
            stats.state == WindowObserverStateTrackingFault) {
            if (fixture->api_double != NULL) fixture->api_double->remove_fail_at = 0u;
            window_observer_status_t stop_status = window_observer_stop(fixture->observer);
            if (stop_status == WindowObserverStatusUnhookFailed)
                stop_status = window_observer_stop(fixture->observer);
            if (stop_status != WindowObserverStatusOk &&
                stop_status != WindowObserverStatusTrackingFault &&
                !(stats.state == WindowObserverStateCreated &&
                  stop_status == WindowObserverStatusBadState))
                return retain_fixture_on_cleanup_failure(fixture);
        }
        if (fixture->observer != NULL) {
            window_observer_quarantine_record_t record;
            memset(&record, 0, sizeof(record));
            const window_observer_status_t handoff_status = window_observer_quarantine_handoff(
                &fixture->observer, &record);
            if (handoff_status != WindowObserverStatusOk) {
                /* A never-bound Created observer owns no graph evidence and can be destroyed directly. */
                if (stats.state != WindowObserverStateCreated ||
                    window_observer_destroy(&fixture->observer) != WindowObserverStatusOk)
                    return retain_fixture_on_cleanup_failure(fixture);
            }
        }
        if (fixture->observer != NULL) return retain_fixture_on_cleanup_failure(fixture);
    }
    if (fixture->graph != NULL &&
        interop_window_graph_destroy(&fixture->graph) != InteropWindowGraphStatusOk)
        return retain_fixture_on_cleanup_failure(fixture);
    api_double_free(fixture->api_double);
    memset(fixture, 0, sizeof(*fixture));
    return 1;
}

static void arm_fault(graph_call_t call, uint32_t ordinal,
                      interop_window_graph_status_t status, uint32_t malformed_success) {
    fault_state.call = call;
    fault_state.ordinal = ordinal;
    fault_state.seen = 0u;
    fault_state.status = status;
    fault_state.malformed_success_kind = malformed_success;
}

static int fixture_admit_one(fixture_t *fixture, uint64_t window);

static int test_bind_pre_read_failures(void) {
    int failed = 0;
    fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    memset(&fault_state, 0, sizeof(fault_state));
    fixture.api_double = api_double_new();
    Check(fixture.api_double != NULL);
    interop_window_graph_config_t graph_config;
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(5);
    graph_config.app_key_lookup = fault_graph_key;
    Check(interop_window_graph_create(&graph_config, &fixture.graph) == InteropWindowGraphStatusOk);
    window_observer_config_t observer_config;
    api_double_config(&observer_config, fixture.api_double, 0u, 1u);
    Check(window_observer_create(&observer_config, &fixture.observer) == WindowObserverStatusOk);
    arm_fault(GraphCallStamp, 1u, InteropWindowGraphStatusInvalidArgument, 0u);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusGraphError);
    window_observer_stats_t observer_stats;
    memset(&observer_stats, 0, sizeof(observer_stats));
    Check(window_observer_status(fixture.observer, &observer_stats) == WindowObserverStatusOk);
    Check(observer_stats.state == WindowObserverStateCreated);
    arm_fault(GraphCallStamp, 1u, InteropWindowGraphStatusWrongThread, 0u);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusWrongThread);
    memset(&observer_stats, 0xA5, sizeof(observer_stats));
    Check(window_observer_status(fixture.observer, &observer_stats) == WindowObserverStatusOk);
    Check(observer_stats.state == WindowObserverStateCreated);
    arm_fault(GraphCallStats, 1u, InteropWindowGraphStatusReentrant, 0u);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusReentrant);
    arm_fault(GraphCallStats, 1u, InteropWindowGraphStatusWrongThread, 0u);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusWrongThread);
    arm_fault(GraphCallStats, 1u, InteropWindowGraphStatusInvalidArgument, 0u);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusGraphError);
    memset(&observer_stats, 0, sizeof(observer_stats));
    Check(window_observer_status(fixture.observer, &observer_stats) == WindowObserverStatusOk);
    Check(observer_stats.state == WindowObserverStateCreated);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_bind_snapshot_faults(void) {
    int failed = 0;
    fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    memset(&fault_state, 0, sizeof(fault_state));
    fixture.api_double = api_double_new();
    Check(fixture.api_double != NULL);
    interop_window_graph_config_t graph_config;
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(5);
    graph_config.app_key_lookup = fault_graph_key;
    Check(interop_window_graph_create(&graph_config, &fixture.graph) == InteropWindowGraphStatusOk);
    window_observer_config_t observer_config;
    api_double_config(&observer_config, fixture.api_double, 0u, 1u);
    Check(window_observer_create(&observer_config, &fixture.observer) == WindowObserverStatusOk);
    arm_fault(GraphCallList, 1u, InteropWindowGraphStatusWrongThread, 0u);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusTrackingFault);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_status(fixture.observer, &stats) == WindowObserverStatusOk);
    Check(stats.state == WindowObserverStateTrackingFault);
    window_observer_quarantine_record_t record;
    memset(&record, 0, sizeof(record));
    Check(window_observer_quarantine_handoff(&fixture.observer, &record) == WindowObserverStatusOk);
    Check(record.tracking_fault == 1u && record.borrowed_graph == fixture.graph);
    Check(interop_window_graph_destroy(&fixture.graph) == InteropWindowGraphStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_bind_invalidate_failure_faults(void) {
    int failed = 0;
    fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.api_double = api_double_new();
    Check(fixture.api_double != NULL);
    interop_window_graph_config_t graph_config;
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(5);
    graph_config.app_key_lookup = fault_graph_key;
    Check(interop_window_graph_create(&graph_config, &fixture.graph) == InteropWindowGraphStatusOk);
    window_observer_config_t observer_config;
    api_double_config(&observer_config, fixture.api_double, 0u, 1u);
    Check(window_observer_create(&observer_config, &fixture.observer) == WindowObserverStatusOk);
    arm_fault(GraphCallInvalidate, 1u, InteropWindowGraphStatusInvalidArgument, 0u);
    Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
          WindowObserverStatusTrackingFault);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_status(fixture.observer, &stats) == WindowObserverStatusOk);
    Check(stats.state == WindowObserverStateTrackingFault &&
          stats.last_graph_status == InteropWindowGraphStatusInvalidArgument);
    window_observer_quarantine_record_t record;
    memset(&record, 0, sizeof(record));
    Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
          WindowObserverStatusOk);
    Check(record.borrowed_graph == fixture.graph && record.tracking_fault == 1u);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_stop_invalidation_failures(void) {
    int failed = 0;
    fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    for (uint32_t mode = 0u; mode < 2u; ++mode) {
        Check(fixture_open(&fixture, 1));
        if (mode == 0u)
            arm_fault(GraphCallInvalidate, 1u, InteropWindowGraphStatusInvalidArgument, 0u);
        else
            arm_fault(GraphCallList, 1u, InteropWindowGraphStatusWrongThread, 0u);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
        window_observer_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        Check(window_observer_status(fixture.observer, &stats) == WindowObserverStatusOk);
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(fixture.api_double->remove_calls == 3u);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.tracking_fault == 1u);
        if (mode == 0u)
            Check(record.last_graph_status == InteropWindowGraphStatusInvalidArgument);
        else
            Check(record.last_graph_status == InteropWindowGraphStatusWrongThread);
        if (!fixture_close(&fixture)) failed = 1;
        if (failed) return failed;
    }
done:
    if (fixture.observer != NULL || fixture.graph != NULL || fixture.api_double != NULL)
        if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_start_preflight_graph_failure(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 0));
    arm_fault(GraphCallList, 1u, InteropWindowGraphStatusReentrant, 0u);
    Check(window_observer_start(fixture.observer) == WindowObserverStatusReentrant);
    Check(fixture.api_double->install_calls == 0u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_status(fixture.observer, &stats) == WindowObserverStatusOk);
    Check(stats.state == WindowObserverStateCreated);
    Check(window_observer_start(fixture.observer) == WindowObserverStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_bind_malformed_success_faults(void) {
    int failed = 0;
    fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    for (uint32_t malformed = MalformedSnapshotCountOverCapacity;
         malformed <= MalformedSnapshotUnsortedWindows; ++malformed) {
        memset(&fixture, 0, sizeof(fixture));
        memset(&fault_state, 0, sizeof(fault_state));
        fixture.api_double = api_double_new();
        Check(fixture.api_double != NULL);
        interop_window_graph_config_t graph_config;
        memset(&graph_config, 0, sizeof(graph_config));
        graph_config.session_epoch = UINT64_C(5);
        graph_config.app_key_lookup = fault_graph_key;
        Check(interop_window_graph_create(&graph_config, &fixture.graph) == InteropWindowGraphStatusOk);
        window_observer_config_t observer_config;
        api_double_config(&observer_config, fixture.api_double, 0u, 1u);
        Check(window_observer_create(&observer_config, &fixture.observer) == WindowObserverStatusOk);
        arm_fault(GraphCallList, 1u, InteropWindowGraphStatusOk, malformed);
        Check(window_observer_bind_graph(fixture.observer, fixture.graph, UINT64_C(5)) ==
              WindowObserverStatusTrackingFault);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.tracking_fault == 1u);
        if (!fixture_close(&fixture)) failed = 1;
        if (failed) return failed;
    }
done:
    if (fixture.observer != NULL || fixture.graph != NULL || fixture.api_double != NULL)
        if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_prepare_transient_failure_retains_candidate(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x100), 0u) ==
          WindowObserverStatusOk);
    arm_fault(GraphCallList, 1u, InteropWindowGraphStatusReentrant, 0u);
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x200), 0u) ==
          WindowObserverStatusReentrant);
    arm_fault(GraphCallList, 1u, InteropWindowGraphStatusWrongThread, 0u);
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x300), 0u) ==
          WindowObserverStatusWrongThread);
    arm_fault(GraphCallList, 2u, InteropWindowGraphStatusReentrant, 0u);
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x400), 0u) ==
          WindowObserverStatusReentrant);
    arm_fault(GraphCallList, 2u, InteropWindowGraphStatusWrongThread, 0u);
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x500), 0u) ==
          WindowObserverStatusWrongThread);
    Check(window_observer_stop(fixture.observer) == WindowObserverStatusOk);
    window_observer_quarantine_record_t record;
    memset(&record, 0, sizeof(record));
    Check(window_observer_quarantine_handoff(&fixture.observer, &record) == WindowObserverStatusOk);
    Check(record.candidate_window == UINT64_C(0x100));
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_prepare_malformed_second_snapshot(uint32_t malformed, int with_tracked_window) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    if (with_tracked_window) Check(fixture_admit_one(&fixture, UINT64_C(0x515)));
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x100), 0u) ==
          WindowObserverStatusOk);
    arm_fault(GraphCallList, 2u, InteropWindowGraphStatusOk, malformed);
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x200), 0u) ==
          WindowObserverStatusTrackingFault);
    Check(fault_state.call == GraphCallNone);
    Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
    window_observer_quarantine_record_t record;
    memset(&record, 0, sizeof(record));
    Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
          WindowObserverStatusOk);
    Check(record.tracking_fault == 1u && record.evidence_incomplete == 1u);
    Check(record.candidate_window == UINT64_C(0x100));
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int fixture_admit_one(fixture_t *fixture, uint64_t window) {
    uint64_t list[1] = { window };
    api_double_add(fixture->api_double, window);
    api_double_enum_set(fixture->api_double, list, 1u);
    return window_observer_refresh(fixture->observer, fixture->graph, UINT64_C(5), NULL) ==
           WindowObserverStatusOk;
}

static void exhaust_stamp_from_tick(api_double_t *dbl, void *arg) {
    (void)arg;
    dbl->tick_hook = NULL;
    api_double_hint(dbl, WindowObserverEventObjectCreate, UINT64_C(0xA01));
}

static void exhaust_stamp_from_query(api_double_t *dbl, uint64_t window, void *arg) {
    (void)window;
    (void)arg;
    api_double_hint(dbl, WindowObserverEventObjectCreate, UINT64_C(0xA02));
    api_double_hint(dbl, WindowObserverEventObjectCreate, UINT64_C(0xA03));
}

static int test_refresh_callback_stamp_fault(void) {
    int failed = 0;
    fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    for (uint32_t query_phase = 0u; query_phase < 2u; ++query_phase) {
        memset(&fixture, 0, sizeof(fixture));
        const uint64_t first_stamp = query_phase == 0u ? UINT64_MAX - 1u : UINT64_MAX - 2u;
        Check(fixture_open_with_stamp(&fixture, 1, first_stamp));
        Check(api_double_add(fixture.api_double, UINT64_C(0xA00)) != NULL);
        uint64_t listed[1] = { UINT64_C(0xA00) };
        api_double_enum_set(fixture.api_double, listed, 1u);
        if (query_phase == 0u) {
            fixture.api_double->tick_hook = exhaust_stamp_from_tick;
        } else {
            fixture.api_double->query_hook = exhaust_stamp_from_query;
            fixture.api_double->query_hook_once = 1u;
            fixture.api_double->query_hook_window = UINT64_C(0xA00);
        }
        window_observer_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
              WindowObserverStatusTrackingFault);
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(__real_interop_window_graph_refresh_begin(fixture.graph, UINT64_C(5)) ==
              InteropWindowGraphStatusOk);
        Check(__real_interop_window_graph_refresh_abort(fixture.graph, UINT64_C(5)) ==
              InteropWindowGraphStatusOk);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.tracking_fault == 1u && record.evidence_incomplete == 1u);
        if (!fixture_close(&fixture)) failed = 1;
        if (failed) return failed;
    }
done:
    if (fixture.observer != NULL || fixture.graph != NULL || fixture.api_double != NULL)
        if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int assert_destroy_failure_retains_evidence(interop_window_graph_status_t graph_status,
                                                    window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(fixture_admit_one(&fixture, UINT64_C(0x880)));
    api_double_hint(fixture.api_double, WindowObserverEventObjectDestroy, UINT64_C(0x880));
    arm_fault(GraphCallDestroy, 1u, graph_status, 0u);
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x880), 0u) == expected_status);
    uint32_t pending = UINT32_C(0xA5A5A5A5);
    Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x880), &pending) ==
          WindowObserverStatusOk);
    Check(pending == 1u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_status(fixture.observer, &stats) == WindowObserverStatusOk);
    if (expected_status == WindowObserverStatusTrackingFault) {
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
    } else {
        Check(stats.state == WindowObserverStateStarted);
        Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x880), 0u) ==
              WindowObserverStatusOk);
    }
    window_observer_quarantine_record_t record;
    memset(&record, 0, sizeof(record));
    if (expected_status != WindowObserverStatusTrackingFault)
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusOk);
    Check(window_observer_quarantine_handoff(&fixture.observer, &record) == WindowObserverStatusOk);
    if (expected_status == WindowObserverStatusTrackingFault) {
        Check(record.protected_count == 1u);
        Check(record.protected_destroys[0].window == UINT64_C(0x880));
    } else {
        Check(record.protected_count == 0u); /* the preserved entry was accepted on the retry */
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_postmutation_snapshot_failure_faults(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(fixture_admit_one(&fixture, UINT64_C(0x881)));
    api_double_hint(fixture.api_double, WindowObserverEventObjectDestroy, UINT64_C(0x881));
    /* prepare: initial snapshot succeeds, note_destroyed succeeds, then its post-mutation snapshot fails. */
    arm_fault(GraphCallList, 3u, InteropWindowGraphStatusWrongThread, 0u);
    Check(window_observer_prepare_callbacks(fixture.observer, UINT64_C(0x881), 0u) ==
          WindowObserverStatusTrackingFault);
    uint32_t pending = 0u;
    Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x881), &pending) ==
          WindowObserverStatusOk && pending == 1u);
    Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
    window_observer_quarantine_record_t record;
    memset(&record, 0, sizeof(record));
    Check(window_observer_quarantine_handoff(&fixture.observer, &record) == WindowObserverStatusOk);
    Check(record.tracking_fault == 1u && record.protected_count == 1u);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_pump_observe_status(interop_window_graph_status_t graph_status,
                                    window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x910)) != NULL);
    api_double_hint(fixture.api_double, WindowObserverEventObjectCreate, UINT64_C(0x910));
    arm_fault(GraphCallObserve, 1u, graph_status, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0xA5, sizeof(stats));
    const window_observer_status_t status = window_observer_pump(
        fixture.observer, fixture.graph, UINT64_C(5), 1u, &stats);
    Check(status == expected_status);
    if (expected_status == WindowObserverStatusReentrant) {
        const unsigned char *bytes = (const unsigned char *)&stats;
        for (size_t index = 0; index < sizeof(stats); ++index) Check(bytes[index] == 0xA5u);
    } else {
        Check(stats.last_graph_status == (uint32_t)graph_status);
    }
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_status(fixture.observer, &stats) == WindowObserverStatusOk);
    if (graph_status == InteropWindowGraphStatusInvalidFacts) {
        Check(stats.dirty_pending == 0u && stats.state == WindowObserverStateStarted);
    } else {
        Check(stats.dirty_pending == 1u && stats.state == WindowObserverStateStarted);
        Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, NULL) ==
              WindowObserverStatusOk);
        uint64_t tracked[InteropMaxWindows];
        uint32_t count = 0u;
        uint32_t free_count = 0u;
        Check(__real_interop_window_graph_list_tracked_windows(fixture.graph, UINT64_C(5),
              tracked, InteropMaxWindows, &count, &free_count) == InteropWindowGraphStatusOk);
        Check(count == 1u && tracked[0] == UINT64_C(0x910));
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_pump_gone_note_status(interop_window_graph_status_t graph_status,
                                      window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    api_double_window_t *window = api_double_add(fixture.api_double, UINT64_C(0x911));
    Check(window != NULL);
    window->mode = ApiDoubleModeGone;
    api_double_hint(fixture.api_double, WindowObserverEventObjectCreate, UINT64_C(0x911));
    arm_fault(GraphCallDestroy, 1u, graph_status, 0u);
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, NULL) ==
          expected_status);
    uint32_t pending = 0u;
    Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x911), &pending) ==
          WindowObserverStatusOk);
    Check(pending == 1u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_status(fixture.observer, &stats) == WindowObserverStatusOk);
    if (expected_status == WindowObserverStatusTrackingFault) {
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.protected_count == 1u && record.protected_destroys[0].window ==
              UINT64_C(0x911));
    } else {
        Check(stats.state == WindowObserverStateStarted && stats.dirty_pending == 1u);
        Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, NULL) ==
              WindowObserverStatusOk);
        Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x911), &pending) ==
              WindowObserverStatusOk && pending == 0u);
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_pump_prepare_list_fault(uint32_t event, uint64_t window_id) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, window_id) != NULL);
    api_double_hint(fixture.api_double, event, window_id);
    /* Pump preflight consumes two list reads; process_key's second prepare reads twice more. */
    arm_fault(GraphCallList, 4u, InteropWindowGraphStatusWrongThread, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, &stats) ==
          WindowObserverStatusWrongThread);
    Check(fault_state.call == GraphCallNone);
    Check(stats.state == WindowObserverStateStarted && stats.dirty_pending == 1u);
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, NULL) ==
          WindowObserverStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_process_key_destroy_transition_failure(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    api_double_hint(fixture.api_double, WindowObserverEventObjectDestroy, UINT64_C(0x943));
    /* Pump entry and its internal preflight each read twice before process_key's Destroy reservation. */
    arm_fault(GraphCallList, 5u, InteropWindowGraphStatusWrongThread, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, &stats) ==
          WindowObserverStatusWrongThread);
    Check(fault_state.call == GraphCallNone);
    Check(stats.state == WindowObserverStateStarted && stats.dirty_pending == 1u);
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, NULL) ==
          WindowObserverStatusOk);
    uint32_t pending = UINT32_MAX;
    Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x943), &pending) ==
          WindowObserverStatusOk && pending == 0u);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_process_key_destroy_apply_failure(
    interop_window_graph_status_t graph_status,
    window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    api_double_hint(fixture.api_double, WindowObserverEventObjectDestroy, UINT64_C(0x944));
    arm_fault(GraphCallDestroy, 1u, graph_status, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0xA6, sizeof(stats));
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, &stats) ==
          expected_status);
    Check(fault_state.call == GraphCallNone);
    uint32_t pending = 0u;
    Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x944), &pending) ==
          WindowObserverStatusOk);
    Check(pending == 1u);
    if (expected_status == WindowObserverStatusReentrant) {
        const unsigned char *bytes = (const unsigned char *)&stats;
        for (size_t index = 0; index < sizeof(stats); ++index) Check(bytes[index] == 0xA6u);
        Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, NULL) ==
              WindowObserverStatusOk);
    } else if (expected_status == WindowObserverStatusTrackingFault) {
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.tracking_fault == 1u);
    } else {
        Check(stats.state == WindowObserverStateStarted && stats.dirty_pending == 1u);
        Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, NULL) ==
              WindowObserverStatusOk);
        Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x944), &pending) ==
              WindowObserverStatusOk && pending == 0u);
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_process_key_observe_followup_list_failure(
    uint32_t list_ordinal, window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x945)) != NULL);
    api_double_hint(fixture.api_double, WindowObserverEventObjectCreate, UINT64_C(0x945));
    arm_fault(GraphCallList, list_ordinal, InteropWindowGraphStatusWrongThread, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0xB4, sizeof(stats));
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, &stats) ==
          expected_status);
    Check(fault_state.call == GraphCallNone);
    if (expected_status == WindowObserverStatusTrackingFault) {
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.tracking_fault == 1u);
    } else {
        Check(stats.state == WindowObserverStateStarted);
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_process_key_malformed_suspended_observe(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x946)) != NULL);
    api_double_hint(fixture.api_double, WindowObserverEventObjectCreate, UINT64_C(0x946));
    arm_fault(GraphCallObserve, 1u, InteropWindowGraphStatusOk, MalformedObserveSuspended);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_pump(fixture.observer, fixture.graph, UINT64_C(5), 1u, &stats) ==
          WindowObserverStatusDesktopInactive);
    Check(fault_state.call == GraphCallNone);
    Check(stats.state == WindowObserverStateStarted && stats.dirty_pending == 1u);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_begin_and_abort_failures(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    arm_fault(GraphCallRefreshBegin, 1u, InteropWindowGraphStatusWrongThread, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusGraphError);
    Check(stats.last_graph_status == InteropWindowGraphStatusWrongThread);

    Check(__real_interop_window_graph_refresh_begin(fixture.graph, UINT64_C(5)) ==
          InteropWindowGraphStatusOk);
    /* The already-open real refresh makes refresh_open take its abort-and-reopen path. */
    arm_fault(GraphCallRefreshAbort, 1u, InteropWindowGraphStatusReentrant, 0u);
    memset(&stats, 0x5A, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusReentrant);
    const unsigned char *bytes = (const unsigned char *)&stats;
    for (size_t index = 0; index < sizeof(stats); ++index) Check(bytes[index] == 0x5Au);
    Check(__real_interop_window_graph_refresh_abort(fixture.graph, UINT64_C(5)) ==
          InteropWindowGraphStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_offer_and_keep_failures(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x920)) != NULL);
    uint64_t listed[1] = { UINT64_C(0x920) };
    api_double_enum_set(fixture.api_double, listed, 1u);
    arm_fault(GraphCallRefreshOffer, 1u, InteropWindowGraphStatusInvalidFacts, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusOk);
    Check(stats.last_graph_status == InteropWindowGraphStatusInvalidFacts);
    uint64_t tracked[InteropMaxWindows];
    uint32_t count = 0u;
    uint32_t free_count = 0u;
    Check(__real_interop_window_graph_list_tracked_windows(fixture.graph, UINT64_C(5), tracked,
          InteropMaxWindows, &count, &free_count) == InteropWindowGraphStatusOk);
    Check(count == 0u);

    api_double_window_t *window = api_double_find(fixture.api_double, UINT64_C(0x920));
    Check(window != NULL);
    window->mode = ApiDoubleModeFailed;
    arm_fault(GraphCallRefreshKeep, 1u, InteropWindowGraphStatusWrongThread, 0u);
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusGraphError);
    Check(stats.last_graph_status == InteropWindowGraphStatusWrongThread);
    window->mode = ApiDoubleModeExists;
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
          WindowObserverStatusOk);

    window->mode = ApiDoubleModeFailed;
    arm_fault(GraphCallRefreshKeep, 1u, InteropWindowGraphStatusReentrant, 0u);
    memset(&stats, 0x6B, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusReentrant);
    const unsigned char *bytes = (const unsigned char *)&stats;
    for (size_t index = 0; index < sizeof(stats); ++index) Check(bytes[index] == 0x6Bu);
    window->mode = ApiDoubleModeExists;
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
          WindowObserverStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_offer_stop_status(interop_window_graph_status_t graph_status,
                                          window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x925)) != NULL);
    uint64_t listed[1] = { UINT64_C(0x925) };
    api_double_enum_set(fixture.api_double, listed, 1u);
    arm_fault(GraphCallRefreshOffer, 1u, graph_status, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0x3C, sizeof(stats));
    const window_observer_status_t status = window_observer_refresh(
        fixture.observer, fixture.graph, UINT64_C(5), &stats);
    Check(status == expected_status);
    if (expected_status == WindowObserverStatusReentrant) {
        const unsigned char *bytes = (const unsigned char *)&stats;
        for (size_t index = 0; index < sizeof(stats); ++index) Check(bytes[index] == 0x3Cu);
    } else {
        Check(stats.last_graph_status == (uint32_t)graph_status);
    }
    Check(__real_interop_window_graph_refresh_begin(fixture.graph, UINT64_C(5)) ==
          InteropWindowGraphStatusOk);
    Check(__real_interop_window_graph_refresh_abort(fixture.graph, UINT64_C(5)) ==
          InteropWindowGraphStatusOk);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
          WindowObserverStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_offer_followup_failure(uint32_t list_ordinal,
                                               interop_window_graph_status_t graph_status,
                                               window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x926)) != NULL);
    uint64_t listed[1] = { UINT64_C(0x926) };
    api_double_enum_set(fixture.api_double, listed, 1u);
    arm_fault(GraphCallList, list_ordinal, graph_status, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0x7D, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          expected_status);
    Check(fault_state.call == GraphCallNone);
    if (expected_status == WindowObserverStatusReentrant) {
        const unsigned char *bytes = (const unsigned char *)&stats;
        for (size_t index = 0; index < sizeof(stats); ++index) Check(bytes[index] == 0x7Du);
    } else if (expected_status == WindowObserverStatusTrackingFault) {
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.tracking_fault == 1u);
    } else {
        Check(stats.state == WindowObserverStateStarted);
        Check(__real_interop_window_graph_refresh_begin(fixture.graph, UINT64_C(5)) ==
              InteropWindowGraphStatusOk);
        Check(__real_interop_window_graph_refresh_abort(fixture.graph, UINT64_C(5)) ==
              InteropWindowGraphStatusOk);
        Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
              WindowObserverStatusOk);
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_unseen_post_destroy_failures(uint32_t list_ordinal,
                                                     interop_window_graph_status_t graph_status,
                                                     window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    api_double_window_t *window = api_double_add(fixture.api_double, UINT64_C(0x932));
    Check(window != NULL);
    uint64_t listed[1] = { UINT64_C(0x932) };
    api_double_enum_set(fixture.api_double, listed, 1u);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
          WindowObserverStatusOk);
    api_double_enum_set(fixture.api_double, NULL, 0u);
    window->mode = ApiDoubleModeGone;
    arm_fault(GraphCallList, list_ordinal, graph_status, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0x2D, sizeof(stats));
    window_observer_status_t actual_status =
        window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats);
    Check(actual_status == expected_status);
    Check(fault_state.call == GraphCallNone);
    if (expected_status == WindowObserverStatusTrackingFault) {
        Check(stats.state == WindowObserverStateTrackingFault);
        Check(window_observer_stop(fixture.observer) == WindowObserverStatusTrackingFault);
        window_observer_quarantine_record_t record;
        memset(&record, 0, sizeof(record));
        Check(window_observer_quarantine_handoff(&fixture.observer, &record) ==
              WindowObserverStatusOk);
        Check(record.tracking_fault == 1u && record.protected_count == 1u);
    } else {
        const unsigned char *bytes = (const unsigned char *)&stats;
        for (size_t index = 0; index < sizeof(stats); ++index) Check(bytes[index] == 0x2Du);
        Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
              WindowObserverStatusOk);
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_malformed_suspended_observation(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x927)) != NULL);
    uint64_t listed[1] = { UINT64_C(0x927) };
    api_double_enum_set(fixture.api_double, listed, 1u);
    arm_fault(GraphCallRefreshOffer, 1u, InteropWindowGraphStatusOk,
              MalformedRefreshObservationSuspended);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
          WindowObserverStatusDesktopInactive);
    Check(fault_state.call == GraphCallNone);
    Check(__real_interop_window_graph_refresh_begin(fixture.graph, UINT64_C(5)) ==
          InteropWindowGraphStatusOk);
    Check(__real_interop_window_graph_refresh_abort(fixture.graph, UINT64_C(5)) ==
          InteropWindowGraphStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_unseen_gone_note_status(interop_window_graph_status_t graph_status,
                                                window_observer_status_t expected_status) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    api_double_window_t *window = api_double_add(fixture.api_double, UINT64_C(0x931));
    Check(window != NULL);
    uint64_t listed[1] = { UINT64_C(0x931) };
    api_double_enum_set(fixture.api_double, listed, 1u);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
          WindowObserverStatusOk);
    api_double_enum_set(fixture.api_double, NULL, 0u);
    window->mode = ApiDoubleModeGone;
    arm_fault(GraphCallDestroy, 1u, graph_status, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          expected_status);
    uint32_t pending = 0u;
    Check(window_observer_destroy_pending(fixture.observer, UINT64_C(0x931), &pending) ==
          WindowObserverStatusOk);
    if (expected_status == WindowObserverStatusTrackingFault) {
        Check(pending == 1u && stats.state == WindowObserverStateTrackingFault);
    } else {
        Check(pending == 1u);
        window->mode = ApiDoubleModeExists;
        api_double_enum_set(fixture.api_double, listed, 1u);
        Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
              WindowObserverStatusOk);
    }
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

static int test_refresh_unseen_commit_and_retry(void) {
    int failed = 0;
    fixture_t fixture;
    Check(fixture_open(&fixture, 1));
    Check(api_double_add(fixture.api_double, UINT64_C(0x930)) != NULL);
    uint64_t listed[1] = { UINT64_C(0x930) };
    api_double_enum_set(fixture.api_double, listed, 1u);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), NULL) ==
          WindowObserverStatusOk);
    api_double_enum_set(fixture.api_double, NULL, 0u);
    arm_fault(GraphCallRefreshUnseen, 1u, InteropWindowGraphStatusWrongThread, 0u);
    window_observer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusGraphError);
    Check(stats.last_graph_status == InteropWindowGraphStatusWrongThread);
    uint64_t tracked[InteropMaxWindows];
    uint32_t count = 0u;
    uint32_t free_count = 0u;
    Check(__real_interop_window_graph_list_tracked_windows(fixture.graph, UINT64_C(5), tracked,
          InteropMaxWindows, &count, &free_count) == InteropWindowGraphStatusOk);
    Check(count == 1u && tracked[0] == UINT64_C(0x930));

    api_double_enum_set(fixture.api_double, listed, 1u);
    arm_fault(GraphCallRefreshCommit, 1u, InteropWindowGraphStatusQueueFull, 0u);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusGraphQueueFull);
    /* QueueFull deliberately leaves the graph refresh open; the next public attempt aborts/reopens it. */
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusOk);
    arm_fault(GraphCallRefreshCommit, 1u, InteropWindowGraphStatusWrongThread, 0u);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusGraphError);
    Check(stats.last_graph_status == InteropWindowGraphStatusWrongThread);
    Check(window_observer_refresh(fixture.observer, fixture.graph, UINT64_C(5), &stats) ==
          WindowObserverStatusOk);
done:
    if (!fixture_close(&fixture)) failed = 1;
    return failed;
}

int main(void) {
    /* Keep the shared double's optional lifecycle/query helpers available to combined harnesses. */
    (void)api_double_run_on_other_thread;
    (void)api_double_queries_of;
    (void)api_double_remove;
    if (test_bind_pre_read_failures() != 0) return 1;
    if (test_bind_snapshot_faults() != 0) return 1;
    if (test_bind_invalidate_failure_faults() != 0) return 1;
    if (test_bind_malformed_success_faults() != 0) return 1;
    if (test_stop_invalidation_failures() != 0) return 1;
    if (test_start_preflight_graph_failure() != 0) return 1;
    if (test_prepare_transient_failure_retains_candidate() != 0) return 1;
    if (test_prepare_malformed_second_snapshot(MalformedSnapshotFreeOverCapacity, 0) != 0)
        return 1;
    if (test_prepare_malformed_second_snapshot(MalformedSnapshotCombinedOverCapacity, 1) != 0)
        return 1;
    if (assert_destroy_failure_retains_evidence(InteropWindowGraphStatusQueueFull,
                                                WindowObserverStatusGraphQueueFull) != 0) return 1;
    if (assert_destroy_failure_retains_evidence(InteropWindowGraphStatusReentrant,
                                                WindowObserverStatusReentrant) != 0) return 1;
    if (assert_destroy_failure_retains_evidence(InteropWindowGraphStatusInvalidArgument,
                                                WindowObserverStatusTrackingFault) != 0) return 1;
    if (test_postmutation_snapshot_failure_faults() != 0) return 1;
    if (test_pump_observe_status(InteropWindowGraphStatusInvalidFacts,
                                 WindowObserverStatusOk) != 0) return 1;
    if (test_pump_observe_status(InteropWindowGraphStatusQueueFull,
                                 WindowObserverStatusGraphQueueFull) != 0) return 1;
    if (test_pump_observe_status(InteropWindowGraphStatusReentrant,
                                 WindowObserverStatusReentrant) != 0) return 1;
    if (test_pump_observe_status(InteropWindowGraphStatusInvalidArgument,
                                 WindowObserverStatusGraphError) != 0) return 1;
    if (test_pump_gone_note_status(InteropWindowGraphStatusQueueFull,
                                   WindowObserverStatusGraphQueueFull) != 0) return 1;
    if (test_pump_gone_note_status(InteropWindowGraphStatusReentrant,
                                   WindowObserverStatusReentrant) != 0) return 1;
    if (test_pump_gone_note_status(InteropWindowGraphStatusInvalidArgument,
                                   WindowObserverStatusTrackingFault) != 0) return 1;
    if (test_pump_prepare_list_fault(WindowObserverEventObjectDestroy, UINT64_C(0x941)) != 0)
        return 1;
    if (test_pump_prepare_list_fault(WindowObserverEventObjectCreate, UINT64_C(0x942)) != 0)
        return 1;
    if (test_process_key_destroy_transition_failure() != 0) return 1;
    if (test_process_key_destroy_apply_failure(InteropWindowGraphStatusQueueFull,
                                               WindowObserverStatusGraphQueueFull) != 0) return 1;
    if (test_process_key_destroy_apply_failure(InteropWindowGraphStatusReentrant,
                                               WindowObserverStatusReentrant) != 0) return 1;
    if (test_process_key_destroy_apply_failure(InteropWindowGraphStatusInvalidArgument,
                                               WindowObserverStatusTrackingFault) != 0) return 1;
    if (test_process_key_observe_followup_list_failure(7u,
                                                       WindowObserverStatusTrackingFault) != 0)
        return 1;
    if (test_process_key_observe_followup_list_failure(8u,
                                                       WindowObserverStatusWrongThread) != 0)
        return 1;
    if (test_process_key_malformed_suspended_observe() != 0) return 1;
    if (test_refresh_begin_and_abort_failures() != 0) return 1;
    if (test_refresh_offer_and_keep_failures() != 0) return 1;
    if (test_refresh_offer_followup_failure(7u, InteropWindowGraphStatusWrongThread,
                                           WindowObserverStatusTrackingFault) != 0) return 1;
    if (test_refresh_offer_followup_failure(8u, InteropWindowGraphStatusReentrant,
                                           WindowObserverStatusReentrant) != 0) return 1;
    if (test_refresh_malformed_suspended_observation() != 0) return 1;
    if (test_refresh_offer_stop_status(InteropWindowGraphStatusQueueFull,
                                       WindowObserverStatusGraphQueueFull) != 0) return 1;
    if (test_refresh_offer_stop_status(InteropWindowGraphStatusReentrant,
                                       WindowObserverStatusReentrant) != 0) return 1;
    if (test_refresh_offer_stop_status(InteropWindowGraphStatusInvalidArgument,
                                       WindowObserverStatusGraphError) != 0) return 1;
    if (test_refresh_unseen_gone_note_status(InteropWindowGraphStatusQueueFull,
                                             WindowObserverStatusGraphQueueFull) != 0) return 1;
    if (test_refresh_unseen_gone_note_status(InteropWindowGraphStatusReentrant,
                                             WindowObserverStatusReentrant) != 0) return 1;
    if (test_refresh_unseen_gone_note_status(InteropWindowGraphStatusInvalidArgument,
                                             WindowObserverStatusTrackingFault) != 0) return 1;
    if (test_refresh_unseen_post_destroy_failures(7u, InteropWindowGraphStatusWrongThread,
                                                  WindowObserverStatusTrackingFault) != 0) return 1;
    if (test_refresh_unseen_post_destroy_failures(9u, InteropWindowGraphStatusReentrant,
                                                  WindowObserverStatusReentrant) != 0) return 1;
    if (test_refresh_unseen_commit_and_retry() != 0) return 1;
    if (test_refresh_callback_stamp_fault() != 0) return 1;
    puts("window observer graph fault fixture passed");
    return 0;
}
