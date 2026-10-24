/**
 * @file window_observer_private.c
 * @brief White-box boundary tests for the exact portable observer implementation.
 *
 * This translation unit includes the production C source instead of linking a second copy. It
 * exercises private scalar guards that public API state cannot construct without forging native
 * lifetime ownership. Probe state is stack-local. Temporary graphs create no capture surfaces or
 * frame pins; their retirement proof flags dispose only pure graph-test records and do not model or
 * claim any native resource-release evidence.
 */
#include "../../src/interop/window_observer_windows.c"

#include <stdio.h>
#include <string.h>

#define PrivateRequire(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "window_observer_private.c:%d: %s\n", __LINE__, #condition); \
            return __LINE__; \
        } \
    } while (0)

static uint32_t private_thread_id(void *context) {
    return *(const uint32_t *)context;
}

static int private_key_lookup(void *context, const interop_window_facts_t *facts, uint8_t *key,
                              uint32_t capacity, uint32_t *length) {
    (void)context;
    (void)facts;
    if (capacity < 6u) return 1;
    memcpy(key, "a-test", 6u);
    *length = 6u;
    return 0;
}

typedef struct private_query_context_t {
    window_observer_query_result_t result;
    interop_window_facts_t facts;
} private_query_context_t;

static window_observer_query_result_t private_query_facts(void *context, uint64_t window,
                                                          interop_window_facts_t *facts) {
    private_query_context_t *query = (private_query_context_t *)context;
    if (query->result == WindowObserverQueryExists) {
        *facts = query->facts;
        facts->window = window;
    }
    return query->result;
}

static uint32_t private_tick(void *context) {
    (void)context;
    return 100u;
}

typedef struct private_lifecycle_context_t {
    uint32_t thread_id;
    uintptr_t next_hook;
    uint32_t install_count;
    uint32_t remove_count;
    uint32_t fail_install_call;
    uint32_t remove_failures;
    int fail_install_with_token;
    int fault_on_first_install;
    int fault_on_clock;
    window_observer_t *observer;
} private_lifecycle_context_t;

static int private_lifecycle_install(void *context, uint32_t event_min, uint32_t event_max,
                                     uintptr_t *hook) {
    private_lifecycle_context_t *lifecycle = (private_lifecycle_context_t *)context;
    (void)event_min;
    (void)event_max;
    lifecycle->install_count += 1u;
    if (lifecycle->fault_on_first_install && lifecycle->install_count == 1u) {
        window_observer_note_hint(WindowObserverEventObjectDestroy, UINT64_C(0xFA17),
                                  WindowObserverObjectIdWindow, WindowObserverChildIdSelf,
                                  lifecycle->thread_id, 77u);
    }
    if (lifecycle->fail_install_call == lifecycle->install_count) {
        if (lifecycle->fail_install_with_token) {
            lifecycle->next_hook += 1u;
            *hook = lifecycle->next_hook;
        }
        return 1;
    }
    lifecycle->next_hook += 1u;
    *hook = lifecycle->next_hook;
    return 0;
}

static int private_lifecycle_remove(void *context, uintptr_t hook) {
    private_lifecycle_context_t *lifecycle = (private_lifecycle_context_t *)context;
    if (hook == 0u) return 1;
    lifecycle->remove_count += 1u;
    if (lifecycle->remove_failures != 0u) {
        lifecycle->remove_failures -= 1u;
        return 1;
    }
    return 0;
}

static window_observer_query_result_t private_lifecycle_query(void *context, uint64_t window,
                                                              interop_window_facts_t *facts) {
    (void)context;
    (void)window;
    (void)facts;
    return WindowObserverQueryFailed;
}

static window_observer_enum_result_t private_lifecycle_enum(void *context, uint64_t *windows,
                                                            uint32_t capacity, uint32_t *count) {
    (void)context;
    (void)windows;
    (void)capacity;
    *count = 0u;
    return WindowObserverEnumOk;
}

static window_observer_desktop_t private_lifecycle_desktop(void *context) {
    (void)context;
    return WindowObserverDesktopDefault;
}

static uint32_t private_lifecycle_thread_id(void *context) {
    return ((private_lifecycle_context_t *)context)->thread_id;
}

static uint64_t private_lifecycle_monotonic(void *context) {
    private_lifecycle_context_t *lifecycle = (private_lifecycle_context_t *)context;
    if (lifecycle->fault_on_clock) {
        lifecycle->fault_on_clock = 0;
        window_observer_note_hint(WindowObserverEventObjectDestroy, UINT64_C(0xFA18),
                                  WindowObserverObjectIdWindow, WindowObserverChildIdSelf,
                                  lifecycle->thread_id, 78u);
    }
    return 1000u;
}

static uint32_t private_lifecycle_tick(void *context) {
    (void)context;
    return 1000u;
}

static int private_lifecycle_probe(void) {
    int result = 0;
    interop_window_graph_config_t graph_config;
    interop_window_graph_t *graph = NULL;
    window_observer_config_t observer_config;
    window_observer_api_t api;
    window_observer_t *observer = NULL;
    window_observer_t *observer_slot;
    window_observer_quarantine_record_t quarantine;
    uint32_t pending = UINT32_MAX;
    private_lifecycle_context_t context = { .thread_id = 72u, .next_hook = 0x400u };
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(0xA56);
    graph_config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&graph_config, &graph) != InteropWindowGraphStatusOk) return __LINE__;

    memset(&api, 0, sizeof(api));
    api.install_hook = private_lifecycle_install;
    api.remove_hook = private_lifecycle_remove;
    api.query_facts = private_lifecycle_query;
    api.enum_top_level_windows = private_lifecycle_enum;
    api.query_input_desktop = private_lifecycle_desktop;
    api.current_thread_id = private_lifecycle_thread_id;
    api.monotonic_ms = private_lifecycle_monotonic;
    api.tick_ms = private_lifecycle_tick;
    memset(&observer_config, 0, sizeof(observer_config));
    observer_config.api = &api;
    observer_config.api_context = &context;
    observer_config.first_stamp = 1u;
    observer_config.own_process_id = 11u;
    if (window_observer_create(&observer_config, &observer) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    observer->dirty_count = 1u;
    if (window_observer_destroy(&observer) != WindowObserverStatusProtectedStatePending || observer == NULL) {
        result = __LINE__;
        goto done;
    }
    observer->dirty_count = 0u;
    if (window_observer_start(observer) != WindowObserverStatusBadState ||
        window_observer_stop(observer) != WindowObserverStatusBadState ||
        window_observer_bind_graph(observer, graph, graph_config.session_epoch) != WindowObserverStatusOk ||
        window_observer_bind_graph(observer, graph, graph_config.session_epoch) != WindowObserverStatusBadState ||
        window_observer_start(observer) != WindowObserverStatusOk || context.install_count != 3u) {
        result = __LINE__;
        goto done;
    }
    observer->refresh_requested = 0u;
    observer->have_attempt = 1u;
    observer->last_attempt_ms = 1000u;
    window_observer_note_hint(WindowObserverEventObjectShow, UINT64_C(0xA560),
                              WindowObserverObjectIdWindow, WindowObserverChildIdSelf,
                              context.thread_id, 41u);
    if (observer->dirty_count != 1u || window_observer_next_timeout_ms(observer, 1000u) != 0u ||
        window_observer_sync_tracked(observer) != WindowObserverStatusOk ||
        window_observer_prepare_callbacks(observer, 0u, 1u) != WindowObserverStatusOk ||
        window_observer_destroy_pending(observer, 99u, &pending) != WindowObserverStatusOk || pending != 0u) {
        result = __LINE__;
        goto done;
    }

    memset(&quarantine, 0xA5, sizeof(quarantine));
    observer_slot = observer;
    if (window_observer_quarantine_handoff(&observer_slot, &quarantine) !=
            WindowObserverStatusProtectedStatePending || observer_slot != observer ||
        window_observer_quarantine_handoff(&observer_slot, &quarantine) !=
            WindowObserverStatusProtectedStatePending || observer_slot != observer ||
        window_observer_destroy(&observer_slot) != WindowObserverStatusProtectedStatePending ||
        observer_slot != observer ||
        window_observer_stop(observer) != WindowObserverStatusOk || context.remove_count != 3u ||
        window_observer_quarantine_handoff(NULL, &quarantine) != WindowObserverStatusInvalidArgument ||
        window_observer_quarantine_handoff(&observer_slot,
            (window_observer_quarantine_record_t *)(void *)observer) != WindowObserverStatusInvalidArgument) {
        result = __LINE__;
        goto done;
    }
    observer->dirty_count = WindowObserverDirtyCapacity + 1u;
    if (window_observer_quarantine_handoff(&observer_slot, &quarantine) !=
        WindowObserverStatusTrackingFault) {
        result = __LINE__;
        goto done;
    }
    observer->dirty_count = 0u;
    observer->destroy_count = WindowObserverDestroyCapacity + 1u;
    if (window_observer_quarantine_handoff(&observer_slot, &quarantine) !=
        WindowObserverStatusTrackingFault) {
        result = __LINE__;
        goto done;
    }
    observer->destroy_count = 0u;
    observer->tracked_count = WindowObserverTrackedCapacity + 1u;
    if (window_observer_quarantine_handoff(&observer_slot, &quarantine) !=
        WindowObserverStatusTrackingFault) {
        result = __LINE__;
        goto done;
    }
    observer->tracked_count = 0u;
    window_observer_t *null_observer = NULL;
    if (window_observer_quarantine_handoff(&null_observer, &quarantine) !=
            WindowObserverStatusInvalidArgument ||
        window_observer_quarantine_handoff(&observer_slot, &quarantine) != WindowObserverStatusOk ||
        observer_slot != NULL || quarantine.observer_state != WindowObserverStateStopped ||
        quarantine.borrowed_graph != graph || quarantine.session_epoch != graph_config.session_epoch) {
        result = __LINE__;
        goto done;
    }
    observer = NULL; /* successful handoff owns and frees the observer object */
done:
    if (observer != NULL && window_observer_destroy(&observer) != WindowObserverStatusOk && result == 0)
        result = __LINE__;
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

/** Exercise destroy gates using only live API-created objects and graph-owned test records. */
static int private_destroy_state_probe(void) {
    int result = 0;
    interop_window_graph_config_t graph_config;
    interop_window_graph_t *graph = NULL;
    window_observer_config_t observer_config;
    window_observer_api_t api;
    window_observer_t *observer = NULL;
    window_observer_quarantine_record_t record;
    interop_window_facts_t facts;
    interop_window_observation_t observation;
    interop_window_event_t event;
    interop_window_retire_proof_t proof;
    private_lifecycle_context_t context = { .thread_id = 73u, .next_hook = 0x500u };
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(0xA57);
    graph_config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&graph_config, &graph) != InteropWindowGraphStatusOk) return __LINE__;
    memset(&api, 0, sizeof(api));
    api.install_hook = private_lifecycle_install;
    api.remove_hook = private_lifecycle_remove;
    api.query_facts = private_lifecycle_query;
    api.enum_top_level_windows = private_lifecycle_enum;
    api.query_input_desktop = private_lifecycle_desktop;
    api.current_thread_id = private_lifecycle_thread_id;
    api.monotonic_ms = private_lifecycle_monotonic;
    api.tick_ms = private_lifecycle_tick;
    memset(&observer_config, 0, sizeof(observer_config));
    observer_config.api = &api;
    observer_config.api_context = &context;
    observer_config.first_stamp = 1u;
    observer_config.own_process_id = 11u;
    if (window_observer_create(&observer_config, &observer) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    if (window_observer_start(observer) != WindowObserverStatusBadState ||
        window_observer_stop(observer) != WindowObserverStatusBadState ||
        window_observer_bind_graph(observer, graph, graph_config.session_epoch) != WindowObserverStatusOk ||
        window_observer_quarantine_handoff(&observer, &record) !=
            WindowObserverStatusProtectedStatePending || observer == NULL ||
        window_observer_destroy(&observer) != WindowObserverStatusProtectedStatePending || observer == NULL ||
        window_observer_stop(observer) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }

    memset(&facts, 0, sizeof(facts));
    facts.window = UINT64_C(0xA570);
    facts.root = facts.window;
    facts.stamp = 1u;
    facts.captured_ms = 100u;
    facts.style = 0x10CF0000u;
    facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled |
                  (uint32_t)InteropWindowFactOnInputDesktop;
    facts.process_id = 4546u;
    facts.thread_id = 80u;
    facts.process_start_time = 3001u;
    facts.width = 640u;
    facts.height = 480u;
    facts.dpi = 96u;
    memcpy(facts.class_name, "TestWindow", 10u);
    facts.class_length = 10u;
    memcpy(facts.image_name, "app.exe", 7u);
    facts.image_length = 7u;
    memcpy(facts.title, "title", 5u);
    facts.title_length = 5u;
    if (interop_window_graph_observe(graph, graph_config.session_epoch, &facts, &observation) !=
            InteropWindowGraphStatusOk ||
        window_observer_destroy(&observer) != WindowObserverStatusProtectedStatePending || observer == NULL ||
        interop_window_graph_next_event(graph, graph_config.session_epoch, &event) !=
            InteropWindowGraphStatusOk || event.kind != InteropWindowEventAnnounce ||
        interop_window_graph_invalidate_all(graph, graph_config.session_epoch, NULL) !=
            InteropWindowGraphStatusOk ||
        interop_window_graph_next_event(graph, graph_config.session_epoch, &event) !=
            InteropWindowGraphStatusOk || event.kind != InteropWindowEventRetireIntent) {
        result = __LINE__;
        goto done;
    }
    memset(&proof, 0, sizeof(proof));
    proof.capture_stopped = 1u;
    proof.pins_drained = 1u;
    if (interop_window_graph_complete_retire(graph, graph_config.session_epoch, event.window,
                                              event.generation, &proof) != InteropWindowGraphStatusOk ||
        window_observer_destroy(&observer) != WindowObserverStatusOk || observer != NULL) {
        result = __LINE__;
        goto done;
    }
done:
    if (observer != NULL && window_observer_destroy(&observer) != WindowObserverStatusOk && result == 0)
        result = __LINE__;
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

/** Exercise hook rollback and callback-fault cleanup through the injected API contract. */
static int private_start_failure_probe(uint32_t fail_install_call, int fail_with_token,
                                       uint32_t remove_failures, int fault_on_install) {
    int result = 0;
    interop_window_graph_config_t graph_config;
    interop_window_graph_t *graph = NULL;
    window_observer_config_t observer_config;
    window_observer_api_t api;
    window_observer_t *observer = NULL;
    window_observer_t *observer_slot;
    window_observer_quarantine_record_t record;
    private_lifecycle_context_t context = {
        .thread_id = 74u,
        .next_hook = 0x600u,
        .fail_install_call = fail_install_call,
        .remove_failures = remove_failures,
        .fail_install_with_token = fail_with_token,
        .fault_on_first_install = fault_on_install,
    };
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(0xA58) + fail_install_call + (uint64_t)fault_on_install;
    graph_config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&graph_config, &graph) != InteropWindowGraphStatusOk) return __LINE__;
    memset(&api, 0, sizeof(api));
    api.install_hook = private_lifecycle_install;
    api.remove_hook = private_lifecycle_remove;
    api.query_facts = private_lifecycle_query;
    api.enum_top_level_windows = private_lifecycle_enum;
    api.query_input_desktop = private_lifecycle_desktop;
    api.current_thread_id = private_lifecycle_thread_id;
    api.monotonic_ms = private_lifecycle_monotonic;
    api.tick_ms = private_lifecycle_tick;
    memset(&observer_config, 0, sizeof(observer_config));
    observer_config.api = &api;
    observer_config.api_context = &context;
    observer_config.first_stamp = 1u;
    observer_config.own_process_id = 11u;
    if (window_observer_create(&observer_config, &observer) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    context.observer = observer;
    if (fault_on_install) observer->next_stamp = UINT64_MAX;
    if (window_observer_bind_graph(observer, graph, graph_config.session_epoch) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    if (fault_on_install) {
        const window_observer_status_t start_status = window_observer_start(observer);
        if (observer->state != WindowObserverStateTrackingFault || context.install_count != 1u ||
            context.remove_count != 1u ||
            window_observer_destroy(&observer) != WindowObserverStatusTrackingFault || observer == NULL) {
            result = __LINE__;
            goto done;
        }
        if (remove_failures != 0u) {
            observer_slot = observer;
            if (start_status != WindowObserverStatusUnhookFailed ||
                window_observer_quarantine_handoff(&observer_slot, &record) !=
                    WindowObserverStatusProtectedStatePending || observer_slot != observer ||
                window_observer_stop(observer) != WindowObserverStatusTrackingFault ||
                context.remove_failures != 0u) {
                result = __LINE__;
                goto done;
            }
        } else if (start_status != WindowObserverStatusTrackingFault) {
            result = __LINE__;
            goto done;
        }
        observer_slot = observer;
        if (window_observer_quarantine_handoff(&observer_slot, &record) != WindowObserverStatusOk ||
            observer_slot != NULL || record.tracking_fault == 0u || record.evidence_incomplete == 0u) {
            result = __LINE__;
            goto done;
        }
        observer = NULL;
    } else {
        if (window_observer_start(observer) != WindowObserverStatusHookFailed ||
            context.install_count != fail_install_call) {
            result = __LINE__;
            goto done;
        }
        if (remove_failures != 0u) {
            if (observer->state != WindowObserverStateFailed ||
                window_observer_quarantine_handoff(&observer, &record) !=
                    WindowObserverStatusProtectedStatePending || observer == NULL ||
                window_observer_stop(observer) != WindowObserverStatusOk || context.remove_failures != 0u) {
                result = __LINE__;
                goto done;
            }
        } else if (observer->state != WindowObserverStateStopped) {
            result = __LINE__;
            goto done;
        }
        if (window_observer_destroy(&observer) != WindowObserverStatusOk || observer != NULL) {
            result = __LINE__;
            goto done;
        }
    }
done:
    if (observer != NULL && window_observer_destroy(&observer) != WindowObserverStatusOk && result == 0)
        result = __LINE__;
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

/** Reach the started preedit clock-fault path with a real owner and injected monotonic callback. */
static int private_preedit_clock_fault_probe(void) {
    int result = 0;
    interop_window_graph_config_t graph_config;
    interop_window_graph_t *graph = NULL;
    window_observer_config_t observer_config;
    window_observer_api_t api;
    window_observer_t *observer = NULL;
    window_observer_t *observer_slot;
    window_observer_quarantine_record_t record;
    private_lifecycle_context_t context = { .thread_id = 75u, .next_hook = 0x700u };
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(0xA59);
    graph_config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&graph_config, &graph) != InteropWindowGraphStatusOk) return __LINE__;
    memset(&api, 0, sizeof(api));
    api.install_hook = private_lifecycle_install;
    api.remove_hook = private_lifecycle_remove;
    api.query_facts = private_lifecycle_query;
    api.enum_top_level_windows = private_lifecycle_enum;
    api.query_input_desktop = private_lifecycle_desktop;
    api.current_thread_id = private_lifecycle_thread_id;
    api.monotonic_ms = private_lifecycle_monotonic;
    api.tick_ms = private_lifecycle_tick;
    memset(&observer_config, 0, sizeof(observer_config));
    observer_config.api = &api;
    observer_config.api_context = &context;
    observer_config.first_stamp = 1u;
    observer_config.own_process_id = 11u;
    if (window_observer_create(&observer_config, &observer) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    context.observer = observer;
    if (window_observer_bind_graph(observer, graph, graph_config.session_epoch) != WindowObserverStatusOk ||
        window_observer_start(observer) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    observer->next_stamp = UINT64_MAX;
    context.fault_on_clock = 1;
    if (window_observer_set_preedit_window(observer, UINT64_C(0xFA19)) !=
            WindowObserverStatusTrackingFault || observer->preedit_window != 0u ||
        observer->busy != 0u || observer->tracking_fault == 0u ||
        window_observer_stop(observer) != WindowObserverStatusTrackingFault) {
        result = __LINE__;
        goto done;
    }
    observer_slot = observer;
    if (window_observer_quarantine_handoff(&observer_slot, &record) != WindowObserverStatusOk ||
        observer_slot != NULL || record.observer_state != WindowObserverStateTrackingFault) {
        result = __LINE__;
        goto done;
    }
    observer = NULL;
done:
    if (observer != NULL && window_observer_destroy(&observer) != WindowObserverStatusOk && result == 0)
        result = __LINE__;
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

/** Exercise a recoverable stop-time unhook failure on an API-created started observer. */
static int private_stop_unhook_retry_probe(void) {
    int result = 0;
    interop_window_graph_config_t graph_config;
    interop_window_graph_t *graph = NULL;
    window_observer_config_t observer_config;
    window_observer_api_t api;
    window_observer_t *observer = NULL;
    private_lifecycle_context_t context = {
        .thread_id = 76u,
        .next_hook = 0x800u,
        .remove_failures = 1u,
    };
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = UINT64_C(0xA5A);
    graph_config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&graph_config, &graph) != InteropWindowGraphStatusOk) return __LINE__;
    memset(&api, 0, sizeof(api));
    api.install_hook = private_lifecycle_install;
    api.remove_hook = private_lifecycle_remove;
    api.query_facts = private_lifecycle_query;
    api.enum_top_level_windows = private_lifecycle_enum;
    api.query_input_desktop = private_lifecycle_desktop;
    api.current_thread_id = private_lifecycle_thread_id;
    api.monotonic_ms = private_lifecycle_monotonic;
    api.tick_ms = private_lifecycle_tick;
    memset(&observer_config, 0, sizeof(observer_config));
    observer_config.api = &api;
    observer_config.api_context = &context;
    observer_config.first_stamp = 1u;
    observer_config.own_process_id = 11u;
    if (window_observer_create(&observer_config, &observer) != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    if (window_observer_bind_graph(observer, graph, graph_config.session_epoch) != WindowObserverStatusOk ||
        window_observer_start(observer) != WindowObserverStatusOk ||
        window_observer_stop(observer) != WindowObserverStatusUnhookFailed ||
        observer->state != WindowObserverStateFailed || context.remove_failures != 0u ||
        window_observer_stop(observer) != WindowObserverStatusOk ||
        observer->state != WindowObserverStateStopped ||
        window_observer_destroy(&observer) != WindowObserverStatusOk || observer != NULL) {
        result = __LINE__;
        goto done;
    }
done:
    if (observer != NULL && window_observer_destroy(&observer) != WindowObserverStatusOk && result == 0)
        result = __LINE__;
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

/** Exercise early lifecycle state guards without installing or releasing any resource. */
static int private_state_guard_probe(void) {
    window_observer_t observer;
    if (atomic_load(&g_active_observer) != NULL || atomic_load(&g_lifecycle_observer) != NULL ||
        atomic_load(&g_hint_inflight) != 0u) return __LINE__;
    memset(&observer, 0, sizeof(observer));
    observer.graph_bound = 1u;
    observer.state = WindowObserverStateStarting;
    if (start_locked(&observer) != WindowObserverStatusBadState ||
        stop_locked(&observer) != WindowObserverStatusBadState) return __LINE__;
    observer.state = WindowObserverStateStopping;
    if (start_locked(&observer) != WindowObserverStatusBadState) return __LINE__;
    observer.state = WindowObserverStateCreated;
    observer.graph_bound = 0u;
    if (stop_locked(&observer) != WindowObserverStatusBadState) return __LINE__;

    interop_window_graph_config_t config;
    interop_window_graph_t *graph = NULL;
    memset(&config, 0, sizeof(config));
    config.session_epoch = UINT64_C(0xA5B);
    config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&config, &graph) != InteropWindowGraphStatusOk) return __LINE__;
    memset(&observer, 0, sizeof(observer));
    observer.bound_graph = graph;
    observer.bound_epoch = config.session_epoch;
    observer.graph_bound = 1u;
    observer.state = WindowObserverStateStopping;
    observer.mailbox_inflight_index = UINT32_MAX;
    const window_observer_status_t stopped = stop_locked(&observer);
    const int snapshot_ok = observer.state == WindowObserverStateStopped && observer.stop_invalidation_done != 0u &&
                           observer.tracked_snapshot_valid != 0u && observer.tracked_count == 0u;
    const int globals_ok = atomic_load(&g_active_observer) == NULL &&
                           atomic_load(&g_lifecycle_observer) == NULL && atomic_load(&g_hint_inflight) == 0u;
    const int graph_ok = interop_window_graph_destroy(&graph) == InteropWindowGraphStatusOk;
    return stopped == WindowObserverStatusOk && snapshot_ok && globals_ok && graph_ok ? 0 : __LINE__;
}

/** Private note_hint_active policy probes; stack records do not model hooks or native activity. */
static int private_note_hint_guard_probe(void) {
    uint32_t callback_thread = 77u;
    window_observer_t observer;
    memset(&observer, 0, sizeof(observer));
    observer.api.current_thread_id = private_thread_id;
    observer.api_context = &callback_thread;
    observer.owner_thread = callback_thread;
    observer.state = WindowObserverStateCreated;
    observer.next_stamp = 1u;
    note_hint_active(&observer, WindowObserverEventObjectShow, 1u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1u);
    if (observer.stats.hints_filtered != 0u || observer.next_stamp != 1u) return __LINE__;

    observer.state = WindowObserverStateStarted;
    callback_thread += 1u;
    note_hint_active(&observer, WindowObserverEventObjectShow, 1u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 2u);
    if (atomic_load(&observer.hints_wrong_thread) != 1u || observer.dirty_count != 0u) return __LINE__;
    observer.owner_thread = callback_thread;
    note_hint_active(&observer, WindowObserverEventObjectShow, 1u, 1, WindowObserverChildIdSelf, 3u);
    note_hint_active(&observer, WindowObserverEventObjectShow, 1u,
                     WindowObserverObjectIdWindow, 1, 4u);
    note_hint_active(&observer, WindowObserverEventObjectShow, 0u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 5u);
    note_hint_active(&observer, UINT32_MAX, 1u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 6u);
    if (observer.stats.hints_filtered != 4u || observer.dirty_count != 0u) return __LINE__;

    observer.state = WindowObserverStateStarting;
    note_hint_active(&observer, WindowObserverEventObjectShow, 1u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 7u);
    if (observer.refresh_requested == 0u || observer.next_stamp != 1u) return __LINE__;
    const uint32_t restart_states[] = {
        WindowObserverStateStopping,
        WindowObserverStateFailed,
        WindowObserverStateTrackingFault,
    };
    for (size_t index = 0u; index < sizeof(restart_states) / sizeof(restart_states[0]); ++index) {
        observer.state = restart_states[index];
        observer.refresh_requested = 0u;
        note_hint_active(&observer, WindowObserverEventObjectShow, 1u,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 70u + (uint32_t)index);
        if (observer.refresh_requested == 0u || observer.next_stamp != 1u) return __LINE__;
    }
    observer.state = WindowObserverStateStarted;
    observer.graph_bound = 0u;
    observer.next_stamp = 3u;
    note_hint_active(&observer, WindowObserverEventObjectDestroy, 3u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 7u);
    if (observer.dirty_count != 1u || observer.dirty[0].window != 3u || observer.destroy_count != 0u)
        return __LINE__;
    observer.next_stamp = UINT64_MAX;
    note_hint_active(&observer, WindowObserverEventObjectDestroy, 2u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 8u);
    if (observer.overflow == 0u || observer.tracking_fault != 0u) return __LINE__;
    observer.graph_bound = 1u;
    note_hint_active(&observer, WindowObserverEventObjectDestroy, 2u,
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 9u);
    if (observer.tracking_fault == 0u || observer.evidence_incomplete == 0u ||
        observer.state != WindowObserverStateTrackingFault) return __LINE__;

    memset(&observer, 0, sizeof(observer));
    observer.api.current_thread_id = private_thread_id;
    observer.api_context = &callback_thread;
    observer.owner_thread = callback_thread;
    observer.state = WindowObserverStateStarted;
    observer.graph_bound = 1u;
    observer.next_stamp = 1u;
    observer.dirty_count = WindowObserverDirtyCapacity;
    note_hint_active(&observer, WindowObserverEventObjectDestroy, UINT64_C(0xFA1A),
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 9u);
    if (observer.tracking_fault == 0u || observer.evidence_incomplete == 0u ||
        observer.stats.hint_overflows != 1u || observer.dirty_count != WindowObserverDirtyCapacity)
        return __LINE__;

    /* An unbound scalar mailbox records local overflow without claiming graph continuity. */
    memset(&observer, 0, sizeof(observer));
    observer.api.current_thread_id = private_thread_id;
    observer.api_context = &callback_thread;
    observer.owner_thread = callback_thread;
    observer.state = WindowObserverStateStarted;
    observer.next_stamp = 1u;
    observer.dirty_count = WindowObserverDirtyCapacity;
    note_hint_active(&observer, WindowObserverEventObjectDestroy, UINT64_C(0xFA1B),
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 9u);
    if (observer.tracking_fault != 0u || observer.evidence_incomplete != 0u ||
        observer.overflow == 0u || observer.stats.hint_overflows != 1u ||
        observer.dirty_count != WindowObserverDirtyCapacity) return __LINE__;

    /* A full scalar mailbox probes the private fail-closed guard, not delivered native evidence. */
    memset(&observer, 0, sizeof(observer));
    observer.api.current_thread_id = private_thread_id;
    observer.api_context = &callback_thread;
    observer.owner_thread = callback_thread;
    observer.state = WindowObserverStateStarted;
    observer.graph_bound = 1u;
    observer.next_stamp = 1u;
    observer.tracked_count = 1u;
    observer.tracked_windows[0] = UINT64_C(0xFA20);
    observer.destroy_count = WindowObserverDestroyCapacity;
    for (uint32_t index = 0u; index < WindowObserverDestroyCapacity; ++index)
        observer.destroys[index].window = UINT64_C(0x10000) + index;
    note_hint_active(&observer, WindowObserverEventObjectDestroy, UINT64_C(0xFA20),
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 10u);
    if (observer.tracking_fault == 0u || observer.evidence_incomplete == 0u ||
        observer.destroy_count != WindowObserverDestroyCapacity) return __LINE__;

    memset(&observer, 0, sizeof(observer));
    observer.api.current_thread_id = private_thread_id;
    observer.api_context = &callback_thread;
    observer.owner_thread = callback_thread;
    observer.state = WindowObserverStateStarted;
    observer.graph_bound = 1u;
    observer.next_stamp = 1u;
    observer.tracked_count = 1u;
    observer.tracked_windows[0] = UINT64_C(0xFA21);
    note_hint_active(&observer, WindowObserverEventObjectDestroy, UINT64_C(0xFA21),
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 11u);
    if (observer.destroy_count != 1u || observer.destroys[0].destroy_stamp != 1u) return __LINE__;
    note_hint_active(&observer, WindowObserverEventObjectDestroy, UINT64_C(0xFA21),
                     WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 12u);
    return observer.destroy_count == 1u && observer.destroys[0].destroy_stamp == 2u &&
           observer.destroys[0].event_time_ms == 12u && observer.stats.hints_coalesced == 2u ? 0 : __LINE__;
}

/* These scalar timing/counter probes do not model an installed hook or active native owner. */
static int private_cadence_scalar_probe(void) {
    uint32_t callback_thread = 91u;
    window_observer_t observer;
    memset(&observer, 0, sizeof(observer));
    observer.api.current_thread_id = private_thread_id;
    observer.api_context = &callback_thread;
    observer.owner_thread = callback_thread;
    observer.state = WindowObserverStateStarted;

    if (window_observer_refresh_due(&observer, 100u) != 1 ||
        window_observer_next_timeout_ms(&observer, 100u) != 0u) return __LINE__;

    observer.have_attempt = 1u;
    observer.last_attempt_ms = 1000u;
    if (window_observer_refresh_due(&observer, 1000u) != 0 ||
        window_observer_next_timeout_ms(&observer, 1000u) != InteropWindowGraphRefreshIntervalMs)
        return __LINE__;
    if (window_observer_next_timeout_ms(&observer, 999u) != InteropWindowGraphRefreshIntervalMs)
        return __LINE__;
    if (window_observer_refresh_due(&observer,
                                    1000u + InteropWindowGraphRefreshIntervalMs) != 1 ||
        window_observer_next_timeout_ms(&observer,
                                        1000u + InteropWindowGraphRefreshIntervalMs) != 0u)
        return __LINE__;

    observer.last_attempt_ms = 1000u;
    observer.refresh_requested = 1u;
    observer.latch_unconsumed = 1u;
    if (window_observer_refresh_due(&observer, 1001u) != 1) return __LINE__;
    observer.refresh_requested = 0u;
    observer.latch_unconsumed = 0u;
    observer.dirty_count = 1u;
    if (window_observer_next_timeout_ms(&observer, 1001u) != 0u) return __LINE__;
    observer.dirty_count = 0u;
    observer.overflow = 1u;
    if (window_observer_next_timeout_ms(&observer, 1001u) != 0u) return __LINE__;
    observer.desktop_blocked = 1u;
    if (window_observer_next_timeout_ms(&observer, 1001u) !=
        InteropWindowGraphRefreshIntervalMs - 1u) return __LINE__;

    _Atomic uint64_t counter = 0u;
    bump_atomic(&counter);
    if (atomic_load(&counter) != 1u) return __LINE__;
    counter = UINT64_MAX;
    bump_atomic(&counter);
    return atomic_load(&counter) == UINT64_MAX ? 0 : __LINE__;
}

/* Defensive preconditions for pure helpers; no slot or graph ownership is fabricated. */
static int private_protected_helper_guard_probe(void) {
    window_observer_t observer;
    memset(&observer, 0, sizeof(observer));
    if (apply_protected_destroy(&observer, 0u) != WindowObserverStatusBadState) return __LINE__;
    observer.tracking_fault = 1u;
    if (apply_protected_destroy(&observer, 0u) != WindowObserverStatusTrackingFault) return __LINE__;
    observer.tracking_fault = 0u;

    uint64_t promoted[2] = {0u, 0u};
    observer.tracked_count = 1u;
    observer.tracked_windows[0] = UINT64_C(0xFA30);
    observer.dirty_count = 1u;
    observer.dirty[0].window = UINT64_C(0xFA30);
    observer.dirty[0].destroy_stamp = 5u;
    observer.dirty[0].destroy_event_ms = 50u;
    observer.destroy_count = WindowObserverDestroyCapacity;
    for (uint32_t index = 0u; index < WindowObserverDestroyCapacity; ++index)
        observer.destroys[index].window = UINT64_C(0x20000) + index;
    if (promote_tracked_dirty_destroys(&observer, promoted, 2u) != -1 ||
        observer.tracking_fault == 0u || observer.evidence_incomplete == 0u ||
        observer.destroy_count != WindowObserverDestroyCapacity) return __LINE__;
    return 0;
}

static int private_process_probe(window_observer_query_result_t query_result, int exhaust_stamp,
                                 int with_destroy, int preexisting_fault) {
    int result = 0;
    interop_window_graph_config_t config;
    interop_window_graph_t *graph = NULL;
    interop_window_event_t event;
    interop_window_retire_proof_t proof;
    private_query_context_t query;
    window_observer_t observer;
    dirty_entry_t dirty;
    memset(&config, 0, sizeof(config));
    config.session_epoch = UINT64_C(0xA55);
    config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&config, &graph) != InteropWindowGraphStatusOk) return __LINE__;

    memset(&query, 0, sizeof(query));
    query.result = query_result;
    query.facts.window = UINT64_C(0xA550);
    query.facts.root = query.facts.window;
    query.facts.owner_root = 0u;
    query.facts.style = 0x10CF0000u;
    query.facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled;
    query.facts.process_id = 4545u;
    query.facts.thread_id = 79u;
    query.facts.process_start_time = 3000u;
    query.facts.width = 800u;
    query.facts.height = 600u;
    query.facts.dpi = 96u;
    memcpy(query.facts.class_name, "TestWindow", 10u);
    query.facts.class_length = 10u;
    memcpy(query.facts.image_name, "app.exe", 7u);
    query.facts.image_length = 7u;
    memcpy(query.facts.title, "title", 5u);
    query.facts.title_length = 5u;

    memset(&observer, 0, sizeof(observer));
    observer.api.query_facts = private_query_facts;
    observer.api.tick_ms = private_tick;
    observer.api_context = &query;
    observer.bound_graph = graph;
    observer.bound_epoch = config.session_epoch;
    observer.graph_bound = 1u;
    observer.state = WindowObserverStateStarted;
    observer.next_stamp = exhaust_stamp ? UINT64_MAX : (with_destroy ? 3u : 1u);
    if (preexisting_fault) {
        observer.tracking_fault = 1u;
        observer.state = WindowObserverStateTrackingFault;
    }
    observer.mailbox_inflight_index = UINT32_MAX;
    memset(&dirty, 0, sizeof(dirty));
    dirty.window = query.facts.window;
    dirty.kinds = DirtyKindShow | (with_destroy ? DirtyKindDestroy : 0u);
    dirty.first_stamp = 1u;
    dirty.last_stamp = 1u;
    if (with_destroy) {
        dirty.destroy_stamp = 2u;
        dirty.destroy_event_ms = 100u;
    }

    const window_observer_status_t status = process_key(&observer, graph, config.session_epoch, &dirty);
    if (preexisting_fault) {
        if (status != WindowObserverStatusTrackingFault || observer.dirty_count != 1u ||
            observer.dirty[0].window != dirty.window || observer.tracking_fault != 1u) result = __LINE__;
        goto done;
    }
    if (exhaust_stamp) {
        if (status != WindowObserverStatusTrackingFault || observer.tracking_fault != 1u ||
            observer.evidence_incomplete != 1u || observer.dirty_count != 1u) result = __LINE__;
        goto done;
    }
    if (status != WindowObserverStatusOk) {
        result = __LINE__;
        goto done;
    }
    if (query_result == WindowObserverQueryFailed) {
        if (observer.dirty_count != 1u || observer.dirty[0].retries != 1u) result = __LINE__;
    } else if (query_result == WindowObserverQueryGone) {
        if (observer.stats.queries_gone != 1u || observer.dirty_count != 0u) result = __LINE__;
    } else {
        const interop_window_graph_status_t event_status = interop_window_graph_next_event(
            graph, config.session_epoch, &event);
        if (observer.tracked_count != 1u || observer.stats.queries_exists != 1u ||
            event_status != InteropWindowGraphStatusOk || event.kind != InteropWindowEventAnnounce ||
            event.window != query.facts.window) {
            fprintf(stderr, "exists state tracked=%u query=%llu status=%u kind=%u win=%llu dirty=%u graph=%u\n",
                    (unsigned)observer.tracked_count, (unsigned long long)observer.stats.queries_exists,
                    (unsigned)event_status, (unsigned)event.kind, (unsigned long long)event.window,
                    (unsigned)observer.dirty_count, (unsigned)observer.stats.last_graph_status);
            result = __LINE__;
            goto done;
        }
        uint32_t retired = 0u;
        if (interop_window_graph_invalidate_all(graph, config.session_epoch, &retired) !=
                InteropWindowGraphStatusOk || retired != 1u ||
            interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk ||
            event.kind != InteropWindowEventRetireIntent) {
            result = __LINE__;
            goto done;
        }
        memset(&proof, 0, sizeof(proof));
        proof.capture_stopped = 1u;
        proof.pins_drained = 1u;
        if (interop_window_graph_complete_retire(graph, config.session_epoch, event.window, event.generation,
                                                  &proof) != InteropWindowGraphStatusOk) result = __LINE__;
    }
done:
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

static int private_snapshot_probe(void) {
    int result = 0;
    interop_window_graph_config_t config;
    interop_window_graph_t *graph = NULL;
    window_observer_t observer;
    memset(&config, 0, sizeof(config));
    config.session_epoch = UINT64_C(0xA51);
    config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&config, &graph) != InteropWindowGraphStatusOk) return __LINE__;
    memset(&observer, 0, sizeof(observer));
    observer.bound_graph = graph;
    observer.bound_epoch = config.session_epoch;
    if (!refresh_tracked_snapshot(&observer, 0) || observer.tracked_count != 0u ||
        observer.tracked_snapshot_valid != 1u) result = __LINE__;
    observer.bound_epoch += 1u;
    if (result == 0 && (refresh_tracked_snapshot(&observer, 0) || observer.tracking_fault != 0u ||
                        observer.stats.last_graph_status != 0u || observer.stats.graph_errors != 0u))
        result = __LINE__;
    if (result == 0 && (refresh_tracked_snapshot(&observer, 1) || observer.tracking_fault != 1u ||
                        observer.state != WindowObserverStateTrackingFault ||
                        observer.stats.last_graph_status != InteropWindowGraphStatusStaleEpoch ||
                        observer.stats.graph_errors != 1u))
        result = __LINE__;
    if (interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0) result = __LINE__;
    return result;
}

static int private_prepare_probe(void) {
    int result = 0;
    interop_window_graph_config_t config;
    interop_window_graph_t *graph = NULL;
    interop_window_facts_t facts;
    interop_window_observation_t observation;
    interop_window_event_t event;
    window_observer_t observer;
    memset(&config, 0, sizeof(config));
    config.session_epoch = UINT64_C(0xA53);
    config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&config, &graph) != InteropWindowGraphStatusOk) return __LINE__;

    memset(&facts, 0, sizeof(facts));
    facts.window = UINT64_C(0xA530);
    facts.root = facts.window;
    facts.stamp = 1u;
    facts.captured_ms = 100u;
    facts.style = 0x10CF0000u;
    facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled |
                  (uint32_t)InteropWindowFactOnInputDesktop;
    facts.process_id = 4343u;
    facts.thread_id = 78u;
    facts.process_start_time = 2000u;
    facts.width = 800u;
    facts.height = 600u;
    facts.dpi = 96u;
    memcpy(facts.class_name, "TestWindow", 10u);
    facts.class_length = 10u;
    memcpy(facts.image_name, "app.exe", 7u);
    facts.image_length = 7u;
    memcpy(facts.title, "title", 5u);
    facts.title_length = 5u;
    memset(&observation, 0, sizeof(observation));
    if (interop_window_graph_observe(graph, config.session_epoch, &facts, &observation) !=
        InteropWindowGraphStatusOk || observation.generation == 0u) {
        result = __LINE__;
        goto done;
    }
    if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk ||
        event.kind != InteropWindowEventAnnounce) {
        result = __LINE__;
        goto done;
    }

    memset(&observer, 0, sizeof(observer));
    observer.bound_graph = graph;
    observer.bound_epoch = config.session_epoch;
    observer.graph_bound = 1u;
    observer.state = WindowObserverStateStopped;
    observer.candidate_window = facts.window;
    observer.dirty_count = 1u;
    observer.dirty[0].window = facts.window;
    observer.dirty[0].kinds = DirtyKindDestroy;
    observer.dirty[0].destroy_stamp = 2u;
    observer.dirty[0].destroy_event_ms = 222u;
    if (prepare_callbacks_locked(&observer, facts.window, 1, 0) != WindowObserverStatusOk ||
        observer.destroy_count != 0u || observer.dirty[0].destroy_stamp != 0u ||
        observer.candidate_window != facts.window || observer.tracked_count != 1u) {
        result = __LINE__;
        goto done;
    }
    if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk ||
        event.kind != InteropWindowEventRetireIntent || event.window != facts.window ||
        event.reason != InteropWindowReasonDestroyed) {
        result = __LINE__;
        goto done;
    }
    interop_window_retire_proof_t proof;
    memset(&proof, 0, sizeof(proof));
    proof.capture_stopped = 1u;
    proof.pins_drained = 1u;
    if (interop_window_graph_complete_retire(graph, config.session_epoch, event.window, event.generation,
                                              &proof) != InteropWindowGraphStatusOk)
        result = __LINE__;
done:
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

static int private_apply_probe(void) {
    int result = 0;
    interop_window_graph_config_t config;
    interop_window_graph_t *graph = NULL;
    interop_window_facts_t facts;
    interop_window_observation_t observation;
    interop_window_event_t event;
    interop_window_retire_proof_t proof;
    window_observer_t observer;
    memset(&config, 0, sizeof(config));
    config.session_epoch = UINT64_C(0xA52);
    config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&config, &graph) != InteropWindowGraphStatusOk) return __LINE__;

    memset(&facts, 0, sizeof(facts));
    facts.window = UINT64_C(0xA520);
    facts.root = facts.window;
    facts.stamp = 1u;
    facts.captured_ms = 100u;
    facts.style = 0x10CF0000u;
    facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled |
                  (uint32_t)InteropWindowFactOnInputDesktop;
    facts.process_id = 4242u;
    facts.thread_id = 77u;
    facts.process_start_time = 1000u;
    facts.width = 800u;
    facts.height = 600u;
    facts.dpi = 96u;
    memcpy(facts.class_name, "TestWindow", 10u);
    facts.class_length = 10u;
    memcpy(facts.image_name, "app.exe", 7u);
    facts.image_length = 7u;
    memcpy(facts.title, "title", 5u);
    facts.title_length = 5u;
    memset(&observation, 0, sizeof(observation));
    if (interop_window_graph_observe(graph, config.session_epoch, &facts, &observation) !=
        InteropWindowGraphStatusOk) {
        result = __LINE__;
        goto done;
    }
    if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk) {
        result = __LINE__;
        goto done;
    }

    memset(&observer, 0, sizeof(observer));
    observer.bound_graph = graph;
    observer.bound_epoch = config.session_epoch;
    observer.graph_bound = 1u;
    observer.tracked_count = 1u;
    observer.tracked_windows[0] = facts.window;
    observer.destroy_count = 1u;
    observer.destroys[0].window = facts.window;
    observer.destroys[0].destroy_stamp = 2u;
    observer.destroys[0].event_time_ms = 222u;
    observer.dirty_count = 1u;
    observer.dirty[0].window = facts.window;
    observer.dirty[0].kinds = DirtyKindDestroy;
    observer.dirty[0].destroy_stamp = 2u;
    observer.dirty[0].destroy_event_ms = 222u;
    observer.mailbox_inflight_index = UINT32_MAX;
    if (apply_protected_destroy(&observer, 0u) != WindowObserverStatusOk || observer.destroy_count != 0u ||
        observer.dirty[0].destroy_stamp != 0u || (observer.dirty[0].kinds & DirtyKindDestroy) != 0u ||
        observer.tracked_count != 1u) {
        result = __LINE__;
        goto done;
    }
    if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk ||
        event.kind != InteropWindowEventRetireIntent || event.window != facts.window ||
        event.reason != InteropWindowReasonDestroyed) {
        result = __LINE__;
        goto done;
    }
    memset(&proof, 0, sizeof(proof));
    proof.capture_stopped = 1u;
    proof.pins_drained = 1u;
    if (interop_window_graph_complete_retire(graph, config.session_epoch, event.window, event.generation,
                                              &proof) != InteropWindowGraphStatusOk) {
        result = __LINE__;
        goto done;
    }
    memset(&observer, 0, sizeof(observer));
    if (apply_protected_destroy(&observer, 0u) != WindowObserverStatusBadState) {
        result = __LINE__;
        goto done;
    }
    observer.tracking_fault = 1u;
    if (apply_protected_destroy(&observer, 0u) != WindowObserverStatusTrackingFault) result = __LINE__;
done:
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

/** Fill the real graph event ring, then verify protected Destroy retention and retry. */
static int private_apply_queue_full_probe(void) {
    int result = 0;
    interop_window_graph_config_t config;
    interop_window_graph_t *graph = NULL;
    interop_window_facts_t facts;
    interop_window_observation_t observation;
    interop_window_event_t event;
    interop_window_retire_proof_t proof;
    window_observer_t observer;
    memset(&config, 0, sizeof(config));
    config.session_epoch = UINT64_C(0xA54);
    config.app_key_lookup = private_key_lookup;
    if (interop_window_graph_create(&config, &graph) != InteropWindowGraphStatusOk) return __LINE__;

    uint64_t next_stamp = 1u;
    uint64_t generations[InteropMaxWindows];
    for (uint32_t index = 0u; index < InteropMaxWindows; ++index) {
        memset(&facts, 0, sizeof(facts));
        facts.window = UINT64_C(0xA540) + index;
        facts.root = facts.window;
        facts.stamp = next_stamp++;
        facts.captured_ms = 100u;
        facts.style = 0x10CF0000u;
        facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled |
                      (uint32_t)InteropWindowFactOnInputDesktop;
        facts.process_id = 4343u;
        facts.thread_id = 78u;
        facts.process_start_time = 2000u;
        facts.width = 800u;
        facts.height = 600u;
        facts.dpi = 96u;
        memcpy(facts.class_name, "TestWindow", 10u);
        facts.class_length = 10u;
        memcpy(facts.image_name, "app.exe", 7u);
        facts.image_length = 7u;
        facts.title[0] = 'a';
        facts.title_length = 1u;
        if (interop_window_graph_observe(graph, config.session_epoch, &facts, &observation) !=
            InteropWindowGraphStatusOk) {
            result = __LINE__;
            goto done;
        }
        generations[index] = observation.generation;
    }
    for (uint32_t index = 0u; index < InteropMaxWindows; ++index) {
        if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk ||
            event.kind != InteropWindowEventAnnounce) {
            result = __LINE__;
            goto done;
        }
    }
    for (uint32_t index = 0u; index < InteropMaxWindows; ++index) {
        memset(&facts, 0, sizeof(facts));
        facts.window = UINT64_C(0xA540) + index;
        facts.root = facts.window;
        facts.stamp = next_stamp++;
        facts.captured_ms = 100u;
        facts.style = 0x10CF0000u;
        facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled |
                      (uint32_t)InteropWindowFactOnInputDesktop;
        facts.process_id = 4343u;
        facts.thread_id = 78u;
        facts.process_start_time = 2000u;
        facts.width = 801u;
        facts.height = 600u;
        facts.dpi = 96u;
        memcpy(facts.class_name, "TestWindow", 10u);
        facts.class_length = 10u;
        memcpy(facts.image_name, "app.exe", 7u);
        facts.image_length = 7u;
        if (interop_window_graph_observe(graph, config.session_epoch, &facts, &observation) !=
            InteropWindowGraphStatusOk || observation.generation != generations[index]) {
            result = __LINE__;
            goto done;
        }
    }
    for (uint32_t index = 0u; index < InteropMaxWindows; ++index) {
        if (interop_window_graph_note_destroyed(graph, config.session_epoch, UINT64_C(0xA540) + index,
                                                next_stamp++, 100u, &observation) !=
            InteropWindowGraphStatusOk) {
            result = __LINE__;
            goto done;
        }
        interop_window_retire_proof_t local_proof;
        memset(&local_proof, 0, sizeof(local_proof));
        local_proof.capture_stopped = 1u;
        local_proof.pins_drained = 1u;
        const interop_window_graph_status_t complete_status = interop_window_graph_complete_retire(
            graph, config.session_epoch, UINT64_C(0xA540) + index, generations[index], &local_proof);
        if (complete_status != InteropWindowGraphStatusOk) {
            fprintf(stderr, "complete first generation %u: status %u gen %llu\n", (unsigned)index,
                    (unsigned)complete_status, (unsigned long long)generations[index]);
            result = __LINE__;
            goto done;
        }
    }
    for (uint32_t index = 0u; index < InteropMaxWindows; ++index) {
        memset(&facts, 0, sizeof(facts));
        facts.window = UINT64_C(0xA540) + index;
        facts.root = facts.window;
        facts.stamp = next_stamp++;
        facts.captured_ms = 200u;
        facts.style = 0x10CF0000u;
        facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled |
                      (uint32_t)InteropWindowFactOnInputDesktop;
        facts.process_id = 4343u;
        facts.thread_id = 78u;
        facts.process_start_time = 2000u;
        facts.width = 800u;
        facts.height = 600u;
        facts.dpi = 96u;
        memcpy(facts.class_name, "TestWindow", 10u);
        facts.class_length = 10u;
        memcpy(facts.image_name, "app.exe", 7u);
        facts.image_length = 7u;
        if (interop_window_graph_observe(graph, config.session_epoch, &facts, &observation) !=
            InteropWindowGraphStatusOk || observation.generation == generations[index]) {
            result = __LINE__;
            goto done;
        }
        generations[index] = observation.generation;
    }
    for (uint32_t index = 0u; index < InteropMaxWindows; ++index) {
        if (interop_window_graph_note_destroyed(graph, config.session_epoch, UINT64_C(0xA540) + index,
                                                next_stamp++, 200u, &observation) !=
            InteropWindowGraphStatusOk) {
            result = __LINE__;
            goto done;
        }
        interop_window_retire_proof_t local_proof;
        memset(&local_proof, 0, sizeof(local_proof));
        local_proof.capture_stopped = 1u;
        local_proof.pins_drained = 1u;
        if (interop_window_graph_complete_retire(graph, config.session_epoch, UINT64_C(0xA540) + index,
                                                  generations[index], &local_proof) != InteropWindowGraphStatusOk) {
            result = __LINE__;
            goto done;
        }
    }
    if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk) {
        result = __LINE__;
        goto done;
    }
    memset(&facts, 0, sizeof(facts));
    facts.window = UINT64_C(0xA540);
    facts.root = facts.window;
    facts.stamp = next_stamp++;
    facts.captured_ms = 300u;
    facts.style = 0x10CF0000u;
    facts.flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled |
                  (uint32_t)InteropWindowFactOnInputDesktop;
    facts.process_id = 4343u;
    facts.thread_id = 78u;
    facts.process_start_time = 2000u;
    facts.width = 800u;
    facts.height = 600u;
    facts.dpi = 96u;
    memcpy(facts.class_name, "TestWindow", 10u);
    facts.class_length = 10u;
    memcpy(facts.image_name, "app.exe", 7u);
    facts.image_length = 7u;
    if (interop_window_graph_observe(graph, config.session_epoch, &facts, &observation) !=
        InteropWindowGraphStatusOk) {
        result = __LINE__;
        goto done;
    }

    memset(&observer, 0, sizeof(observer));
    observer.bound_graph = graph;
    observer.bound_epoch = config.session_epoch;
    observer.graph_bound = 1u;
    observer.mailbox_inflight_index = UINT32_MAX;
    observer.destroy_count = 1u;
    observer.destroys[0].window = UINT64_C(0xA540);
    observer.destroys[0].destroy_stamp = UINT64_C(0xA541);
    observer.destroys[0].event_time_ms = 333u;
    interop_window_graph_stats_t graph_stats;
    if (interop_window_graph_get_stats(graph, &graph_stats) != InteropWindowGraphStatusOk ||
        graph_stats.events_pending != InteropWindowGraphEventCapacity) {
        fprintf(stderr, "event ring fill was %u/%u\n", (unsigned)graph_stats.events_pending,
                (unsigned)InteropWindowGraphEventCapacity);
        result = __LINE__;
        goto done;
    }
    const window_observer_status_t apply_status = apply_protected_destroy(&observer, 0u);
    if (apply_status != WindowObserverStatusGraphQueueFull ||
        observer.destroy_count != 1u || observer.destroys[0].destroy_stamp != UINT64_C(0xA541) ||
        observer.mailbox_inflight_index != UINT32_MAX) {
        fprintf(stderr, "protected destroy status %u, mailbox %u, stamp %llu\n", (unsigned)apply_status,
                (unsigned)observer.destroy_count, (unsigned long long)observer.destroys[0].destroy_stamp);
        result = __LINE__;
        goto done;
    }
    uint32_t drained = 0u;
    while (drained < InteropWindowGraphEventCapacity) {
        if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusOk) {
            result = __LINE__;
            goto done;
        }
        drained += 1u;
    }
    if (interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusNone ||
        apply_protected_destroy(&observer, 0u) != WindowObserverStatusOk || observer.destroy_count != 0u) {
        result = __LINE__;
        goto done;
    }
    interop_window_graph_status_t cleanup_status = interop_window_graph_next_event(
        graph, config.session_epoch, &event);
    if (cleanup_status != InteropWindowGraphStatusOk || event.kind != InteropWindowEventRetireIntent ||
        event.window != UINT64_C(0xA540)) {
        fprintf(stderr, "retry event status %u kind %u window %llu\n", (unsigned)cleanup_status,
                (unsigned)event.kind, (unsigned long long)event.window);
        result = __LINE__;
        goto done;
    }
    memset(&proof, 0, sizeof(proof));
    proof.capture_stopped = 1u;
    proof.pins_drained = 1u;
    cleanup_status = interop_window_graph_complete_retire(graph, config.session_epoch, event.window,
                                                           event.generation, &proof);
    if (cleanup_status != InteropWindowGraphStatusOk ||
        interop_window_graph_next_event(graph, config.session_epoch, &event) != InteropWindowGraphStatusNone) {
        fprintf(stderr, "retry complete status %u\n", (unsigned)cleanup_status);
        result = __LINE__;
        goto done;
    }
done:
    if (graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk && result == 0)
        result = __LINE__;
    return result;
}

/** @brief Run stack-only private helper boundary probes; zero means every assertion passed. */
int window_observer_private_guard_tests(void) {
    uint64_t plain_counter = UINT64_MAX;
    bump(&plain_counter);
    PrivateRequire(plain_counter == UINT64_MAX);
    plain_counter = 0u;
    bump(&plain_counter);
    PrivateRequire(plain_counter == 1u);

    _Atomic uint64_t atomic_counter;
    atomic_init(&atomic_counter, UINT64_MAX);
    bump_atomic(&atomic_counter);
    PrivateRequire(atomic_load(&atomic_counter) == UINT64_MAX);
    atomic_store(&atomic_counter, 0u);
    bump_atomic(&atomic_counter);
    PrivateRequire(atomic_load(&atomic_counter) == 1u);

    window_observer_t observer;
    memset(&observer, 0, sizeof(observer));
    observer.state = WindowObserverStateStarted;
    latch_tracking_fault(&observer, 0);
    PrivateRequire(observer.tracking_fault == 1u && observer.evidence_incomplete == 0u &&
                   observer.state == WindowObserverStateTrackingFault);
    observer.tracking_fault = 0u;
    observer.evidence_incomplete = 0u;
    observer.state = WindowObserverStateStopped;
    latch_tracking_fault(&observer, 1);
    PrivateRequire(observer.tracking_fault == 1u && observer.evidence_incomplete == 1u &&
                   observer.state == WindowObserverStateTrackingFault);

    uint32_t thread_id = 41u;
    observer.api.current_thread_id = private_thread_id;
    observer.api_context = &thread_id;
    observer.owner_thread = 41u;
    PrivateRequire(on_owner_thread(&observer) == 1);
    thread_id = 42u;
    PrivateRequire(entry_gate(&observer) == WindowObserverStatusWrongThread);
    thread_id = 41u;
    observer.busy = 1u;
    PrivateRequire(entry_gate(&observer) == WindowObserverStatusReentrant);
    observer.busy = 0u;
    PrivateRequire(entry_gate(&observer) == WindowObserverStatusOk);

    memset(&observer, 0, sizeof(observer));
    observer.destroy_count = 3u;
    observer.destroys[0].window = 10u;
    observer.destroys[1].window = 20u;
    observer.destroys[2].window = 30u;
    destroy_remove(&observer, 3u);
    PrivateRequire(observer.destroy_count == 3u); /* out-of-range index is inert */
    observer.mailbox_inflight_index = UINT32_MAX;
    destroy_remove(&observer, 1u);
    PrivateRequire(observer.destroy_count == 2u && observer.destroys[0].window == 10u &&
                   observer.destroys[1].window == 30u && observer.mailbox_inflight_index == UINT32_MAX);
    observer.destroy_count = 3u;
    observer.destroys[0].window = 10u;
    observer.destroys[1].window = 20u;
    observer.destroys[2].window = 30u;
    observer.mailbox_inflight_index = 1u;
    destroy_remove(&observer, 1u);
    PrivateRequire(observer.mailbox_inflight_index == UINT32_MAX);
    observer.destroy_count = 3u;
    observer.destroys[0].window = 10u;
    observer.destroys[1].window = 20u;
    observer.destroys[2].window = 30u;
    observer.mailbox_inflight_index = 2u;
    destroy_remove(&observer, 0u);
    PrivateRequire(observer.mailbox_inflight_index == 1u);
    observer.destroy_count = 3u;
    observer.destroys[0].window = 10u;
    observer.destroys[1].window = 20u;
    observer.destroys[2].window = 30u;
    observer.mailbox_inflight_index = 0u;
    destroy_remove(&observer, 1u);
    PrivateRequire(observer.mailbox_inflight_index == 0u);

    dirty_entry_t merged;
    dirty_entry_t incoming;
    memset(&merged, 0, sizeof(merged));
    memset(&incoming, 0, sizeof(incoming));
    incoming.kinds = DirtyKindCreate | DirtyKindDestroy;
    incoming.first_stamp = 5u;
    incoming.last_stamp = 8u;
    incoming.destroy_stamp = 7u;
    incoming.destroy_event_ms = 70u;
    dirty_merge(&merged, &incoming);
    PrivateRequire(merged.kinds == incoming.kinds && merged.first_stamp == 5u && merged.last_stamp == 8u &&
                   merged.destroy_stamp == 7u && merged.destroy_event_ms == 70u);
    incoming.kinds = DirtyKindShow;
    incoming.first_stamp = 9u;
    incoming.last_stamp = 3u;
    incoming.destroy_stamp = 6u;
    incoming.destroy_event_ms = 60u;
    dirty_merge(&merged, &incoming);
    PrivateRequire(merged.kinds == (DirtyKindCreate | DirtyKindDestroy | DirtyKindShow) &&
                   merged.first_stamp == 5u && merged.last_stamp == 8u &&
                   merged.destroy_stamp == 7u && merged.destroy_event_ms == 70u);

    memset(&observer, 0, sizeof(observer));
    memset(&incoming, 0, sizeof(incoming));
    PrivateRequire(dirty_pop_front(&observer, &merged) == 0);
    incoming.window = 100u;
    incoming.first_stamp = 1u;
    dirty_push_front(&observer, &incoming);
    PrivateRequire(observer.dirty_count == 1u && observer.dirty[0].window == 100u);
    incoming.kinds = DirtyKindShow;
    incoming.first_stamp = 0u;
    incoming.last_stamp = 4u;
    dirty_push_front(&observer, &incoming);
    PrivateRequire(observer.dirty_count == 1u && observer.dirty[0].kinds == DirtyKindShow &&
                   observer.dirty[0].first_stamp == 1u && observer.dirty[0].last_stamp == 4u);
    dirty_push_back_retry(&observer, &incoming);
    PrivateRequire(observer.dirty_count == 1u && observer.dirty[0].last_stamp == 4u);
    PrivateRequire(dirty_pop_front(&observer, &merged) == 1 && merged.window == 100u &&
                   observer.dirty_count == 0u);
    PrivateRequire(dirty_pop_front(&observer, &merged) == 0);

    observer.dirty_count = WindowObserverDirtyCapacity;
    for (uint32_t index = 0u; index < WindowObserverDirtyCapacity; ++index) {
        observer.dirty[index].window = (uint64_t)index + 1u;
    }
    observer.overflow = 0u;
    incoming.window = UINT64_C(0x10000);
    dirty_push_front(&observer, &incoming);
    PrivateRequire(observer.dirty_count == WindowObserverDirtyCapacity && observer.overflow == 1u);
    observer.overflow = 0u;
    dirty_push_back_retry(&observer, &incoming);
    PrivateRequire(observer.dirty_count == WindowObserverDirtyCapacity && observer.overflow == 1u);

    PrivateRequire(dirty_find(&observer, 1u) == 0);
    PrivateRequire(dirty_find(&observer, UINT64_C(0x20000)) == -1);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventObjectCreate) == DirtyKindCreate);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventObjectDestroy) == DirtyKindDestroy);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventObjectShow) == DirtyKindShow);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventObjectHide) == DirtyKindHide);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventSystemMinimizeStart) == DirtyKindMinimizeStart);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventSystemMinimizeEnd) == DirtyKindMinimizeEnd);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventObjectCloaked) == DirtyKindCloak);
    PrivateRequire(dirty_kind_for_event(WindowObserverEventObjectUncloaked) == DirtyKindCloak);
    PrivateRequire(dirty_kind_for_event(UINT32_MAX) == DirtyKindObjectOther);
    PrivateRequire(event_in_hooked_range(WindowObserverEventObjectCreate) == 1);
    PrivateRequire(event_in_hooked_range(WindowObserverEventSystemMinimizeEnd) == 1);
    PrivateRequire(event_in_hooked_range(UINT32_MAX) == 0);

    memset(&observer, 0, sizeof(observer));
    const interop_window_graph_status_t drop_statuses[] = {
        InteropWindowGraphStatusInvalidFacts,
        InteropWindowGraphStatusStaleFacts,
        InteropWindowGraphStatusCycle,
        InteropWindowGraphStatusAtCapacity,
        InteropWindowGraphStatusGenerationExhausted,
        InteropWindowGraphStatusLookupFailed,
        InteropWindowGraphStatusAppKeyInvalid,
        InteropWindowGraphStatusStale,
    };
    PrivateRequire(fold_status(&observer, InteropWindowGraphStatusOk) == FoldContinue);
    PrivateRequire(fold_status(&observer, InteropWindowGraphStatusNone) == FoldContinue);
    for (size_t index = 0u; index < sizeof(drop_statuses) / sizeof(drop_statuses[0]); ++index)
        PrivateRequire(fold_status(&observer, drop_statuses[index]) == FoldDrop);
    PrivateRequire(fold_status(&observer, InteropWindowGraphStatusQueueFull) == FoldQueueFull);
    PrivateRequire(fold_status(&observer, InteropWindowGraphStatusReentrant) == FoldReentrant);
    PrivateRequire(fold_status(&observer, InteropWindowGraphStatusInvalidArgument) == FoldFatal);
    PrivateRequire(observer.stats.graph_errors == 11u &&
                   observer.stats.last_graph_status == InteropWindowGraphStatusInvalidArgument);
    PrivateRequire(stop_status(FoldQueueFull) == WindowObserverStatusGraphQueueFull);
    PrivateRequire(stop_status(FoldReentrant) == WindowObserverStatusReentrant);
    PrivateRequire(stop_status(FoldDrop) == WindowObserverStatusGraphError);
    PrivateRequire(stop_status(FoldFatal) == WindowObserverStatusGraphError);

    memset(&observer, 0, sizeof(observer));
    observer.tracked_count = 2u;
    observer.tracked_windows[0] = 11u;
    observer.tracked_windows[1] = 12u;
    observer.destroy_count = 1u;
    observer.destroys[0].window = 11u;
    PrivateRequire(tracked_contains(&observer, 11u) == 1 && tracked_contains(&observer, 13u) == 0);
    PrivateRequire(protected_union_count(&observer, 13u, 0u) == 3u);
    PrivateRequire(protected_union_count(&observer, 11u, 12u) == 2u);
    observer.candidate_window = 12u;
    observer.transition_candidate_window = 13u;
    PrivateRequire(candidate_protected(&observer, 12u) && candidate_protected(&observer, 13u));
    PrivateRequire(!candidate_protected(&observer, 14u) && !candidate_protected(&observer, 0u));
    observer.destroys[0].window = 12u;
    PrivateRequire(protected_union_count(&observer, 12u, 13u) == 3u);
    PrivateRequire(protected_union_count(&observer, 12u, 12u) == 2u);
    memset(&observer, 0, sizeof(observer));
    PrivateRequire(prepare_callbacks_locked(&observer, 1u, 0, 0) == WindowObserverStatusBadState);
    observer.graph_bound = 1u;
    observer.tracking_fault = 1u;
    PrivateRequire(prepare_callbacks_locked(&observer, 1u, 0, 0) == WindowObserverStatusTrackingFault);
    PrivateRequire(private_snapshot_probe() == 0);
    const int prepare_probe_result = private_prepare_probe();
    if (prepare_probe_result != 0) {
        fprintf(stderr, "private_prepare_probe failed at source line %d\n", prepare_probe_result);
        return __LINE__;
    }
    const int apply_probe_result = private_apply_probe();
    if (apply_probe_result != 0) {
        fprintf(stderr, "private_apply_probe failed at source line %d\n", apply_probe_result);
        return __LINE__;
    }
    const int apply_queue_full_result = private_apply_queue_full_probe();
    if (apply_queue_full_result != 0) {
        fprintf(stderr, "private_apply_queue_full_probe failed at source line %d\n", apply_queue_full_result);
        return __LINE__;
    }
    const int process_exists_result = private_process_probe(WindowObserverQueryExists, 0, 0, 0);
    if (process_exists_result != 0) {
        fprintf(stderr, "private_process_exists failed at source line %d\n", process_exists_result);
        return __LINE__;
    }
    PrivateRequire(private_process_probe(WindowObserverQueryExists, 0, 1, 0) == 0);
    PrivateRequire(private_process_probe(WindowObserverQueryGone, 0, 0, 0) == 0);
    PrivateRequire(private_process_probe(WindowObserverQueryFailed, 0, 0, 0) == 0);
    PrivateRequire(private_process_probe(WindowObserverQueryExists, 1, 0, 0) == 0);
    PrivateRequire(private_process_probe(WindowObserverQueryExists, 0, 0, 1) == 0);
    const int lifecycle_result = private_lifecycle_probe();
    if (lifecycle_result != 0) {
        fprintf(stderr, "private_lifecycle_probe failed at source line %d\n", lifecycle_result);
        return __LINE__;
    }
    const int destroy_state_result = private_destroy_state_probe();
    if (destroy_state_result != 0) {
        fprintf(stderr, "private_destroy_state_probe failed at source line %d\n", destroy_state_result);
        return __LINE__;
    }
    const int first_start_failure = private_start_failure_probe(1u, 0, 0u, 0);
    if (first_start_failure != 0) fprintf(stderr, "first start-failure probe %d\n", first_start_failure);
    PrivateRequire(first_start_failure == 0);
    const int second_start_failure = private_start_failure_probe(2u, 1, 1u, 0);
    if (second_start_failure != 0) fprintf(stderr, "second start-failure probe %d\n", second_start_failure);
    PrivateRequire(second_start_failure == 0);
    const int callback_start_failure = private_start_failure_probe(0u, 0, 0u, 1);
    if (callback_start_failure != 0) fprintf(stderr, "callback start-failure probe %d\n", callback_start_failure);
    PrivateRequire(callback_start_failure == 0);
    PrivateRequire(private_start_failure_probe(0u, 0, 1u, 1) == 0);
    PrivateRequire(private_preedit_clock_fault_probe() == 0);
    PrivateRequire(private_stop_unhook_retry_probe() == 0);
    PrivateRequire(private_state_guard_probe() == 0);
    const int note_hint_guard_result = private_note_hint_guard_probe();
    if (note_hint_guard_result != 0)
        fprintf(stderr, "private_note_hint_guard_probe failed at source line %d\n", note_hint_guard_result);
    PrivateRequire(note_hint_guard_result == 0);
    const int cadence_result = private_cadence_scalar_probe();
    if (cadence_result != 0)
        fprintf(stderr, "private_cadence_scalar_probe failed at source line %d\n", cadence_result);
    PrivateRequire(cadence_result == 0);
    const int protected_helper_result = private_protected_helper_guard_probe();
    if (protected_helper_result != 0)
        fprintf(stderr, "private_protected_helper_guard_probe failed at source line %d\n",
                protected_helper_result);
    PrivateRequire(protected_helper_result == 0);

    memset(&observer, 0, sizeof(observer));
    observer.tracked_count = 2u;
    observer.tracked_windows[0] = 11u;
    observer.tracked_windows[1] = 12u;
    observer.candidate_window = 12u;
    observer.dirty_count = 2u;
    observer.dirty[0].window = 11u;
    observer.dirty[0].destroy_stamp = 3u;
    observer.dirty[0].destroy_event_ms = 30u;
    observer.dirty[1].window = 12u;
    observer.dirty[1].destroy_stamp = 4u;
    observer.dirty[1].destroy_event_ms = 40u;
    uint64_t promoted[2] = {0u, 0u};
    PrivateRequire(promote_tracked_dirty_destroys(&observer, promoted, 2u) == 2);
    PrivateRequire(promoted[0] == 11u && promoted[1] == 12u && observer.destroy_count == 2u);
    observer.destroys[0].destroy_stamp = 9u; /* newer protected stamp wins over an older dirty copy */
    observer.destroys[0].event_time_ms = 90u;
    observer.dirty[0].destroy_stamp = 8u;
    observer.dirty[0].destroy_event_ms = 80u;
    PrivateRequire(promote_tracked_dirty_destroys(&observer, promoted, 2u) == 2);
    PrivateRequire(observer.destroys[0].destroy_stamp == 9u && observer.destroys[0].event_time_ms == 90u);
    observer.dirty_count = 1u;
    observer.dirty[0].window = 99u;
    observer.dirty[0].destroy_stamp = 1u;
    PrivateRequire(promote_tracked_dirty_destroys(&observer, promoted, 2u) == 0);
    observer.dirty[0].window = 11u;
    PrivateRequire(promote_tracked_dirty_destroys(&observer, promoted, 0u) == -1);
    PrivateRequire(observer.tracking_fault == 1u && observer.evidence_incomplete == 1u);

    PrivateRequire(!observer_span_valid(NULL, sizeof(uint32_t), _Alignof(uint32_t)));
    PrivateRequire(observer_span_valid(&plain_counter, sizeof(plain_counter), _Alignof(uint64_t)));
    PrivateRequire(!observer_span_valid((const void *)(uintptr_t)(UINTPTR_MAX & ~(uintptr_t)7u),
                                        sizeof(uint64_t), _Alignof(uint64_t)));
    PrivateRequire(observer_span_valid((const void *)(uintptr_t)3u, 0u, 1u));
    PrivateRequire(!observer_span_valid((const void *)(uintptr_t)3u, 1u, 4u));
    PrivateRequire(observer_spans_overlap((const void *)10u, 5u, (const void *)14u, 2u));
    PrivateRequire(observer_spans_overlap((const void *)14u, 2u, (const void *)10u, 5u));
    PrivateRequire(!observer_spans_overlap((const void *)10u, 4u, (const void *)14u, 2u));
    PrivateRequire(!observer_spans_overlap((const void *)10u, 0u, (const void *)10u, 5u));
    PrivateRequire(!observer_spans_overlap((const void *)10u, 5u, (const void *)10u, 0u));
    return 0;
}
