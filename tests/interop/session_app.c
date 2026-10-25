/*
 * Application runtime tests. The session_control_host fixture supplies the in-process fake daemon,
 * the AF_VSOCK socketpair seam and the guest peer; this file substitutes session_app_run_detailed for
 * the runtime entry and drives fake handlers through it.
 */
#define main session_control_host_fixture_entry_unused
#include "session_control_host.c"
#undef main

#include "../../src/interop/session_app.h"
#include "../../src/interop/session_supervisor.h"
#include "../../src/interop/session_handlers_builtin.h"

#include <stdio.h>

void session_supervisor_unit_tests(void);

/* ---- Fake handler: records every callback and the sender results it observes. ---- */

typedef struct fake_t {
    uint32_t owned_type;
    int ready_status;
    int frame_status;
    int tick_status;
    int fds_status;
    int poll_status;
    uint32_t poll_count;
    uint32_t poll_report; /* when nonzero, poll_fds reports this count without writing any entries */
    int pipe_read[4];
    int pipe_write[4];
    bool write_on_first_tick;
    uint32_t send_on_tick; /* 0 none, 1 WindowClose, 2 LaunchRequest with the identifier recorded */
    uint32_t ready_calls;
    uint32_t frame_calls;
    uint32_t tick_calls;
    uint32_t fds_calls;
    uint32_t fds_ready_mask;
    uint32_t closing_calls;
    uint32_t closing_order;
    uint32_t closing_reason;
    uint32_t sent;
    uint32_t request_id;
    uint32_t last_frame_id;
    uint32_t last_frame_type;
    uint64_t negotiated;
    uint64_t deadline;
    interop_host_feature_result_t last_send;
    interop_host_feature_result_t closing_send;
    bool skip_closing;
    bool skip_error;
    uint32_t error_calls;
    uint32_t error_code;
    uint32_t error_request;
    int error_status;
} fake_t;

static uint32_t closing_sequence;

static int fake_owns(void *context, uint32_t message_type)
{
    fake_t *fake = context;
    return fake->owned_type != 0 && fake->owned_type == message_type;
}

static int fake_on_ready(void *context, uint64_t negotiated, interop_host_feature_sender_t *sender)
{
    fake_t *fake = context;
    (void)sender;
    ++fake->ready_calls;
    fake->negotiated = negotiated;
    return fake->ready_status;
}

static int fake_on_frame(void *context, uint32_t message_type, uint32_t request_id,
    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender)
{
    fake_t *fake = context;
    (void)frame;
    (void)length;
    (void)sender;
    ++fake->frame_calls;
    fake->last_frame_type = message_type;
    fake->last_frame_id = request_id;
    return fake->frame_status;
}

static int fake_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count)
{
    fake_t *fake = context;
    uint32_t index;

    if (fake->poll_status != 0) return fake->poll_status;
    if (fake->poll_report != 0) {
        *count = fake->poll_report;
        return 0;
    }
    if (capacity < SessionHandlerMaxDescriptors) return 1;
    for (index = 0; index < fake->poll_count; ++index) {
        fds[index].fd = fake->pipe_read[index];
        fds[index].events = POLLIN;
        fds[index].revents = 0;
    }
    *count = fake->poll_count;
    return 0;
}

static int fake_on_fds(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
    interop_host_feature_sender_t *sender)
{
    fake_t *fake = context;
    uint8_t buffer[8];
    size_t index;
    (void)now_ms;
    (void)sender;
    ++fake->fds_calls;
    for (index = 0; index < count; ++index) {
        if ((fds[index].revents & POLLIN) != 0) {
            fake->fds_ready_mask |= UINT32_C(1) << index;
            assert(read(fds[index].fd, buffer, sizeof(buffer)) > 0);
        }
    }
    return fake->fds_status;
}

static int fake_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    fake_t *fake = context;
    uint8_t frame[256];
    size_t length;
    uint32_t request_id = 0;

    (void)now_ms;
    ++fake->tick_calls;
    if (fake->write_on_first_tick && fake->tick_calls == 1) {
        assert(write(fake->pipe_write[0], "x", 1) == 1);
    }
    if (fake->send_on_tick == 1 && fake->sent == 0) {
        length = build_window_close(frame, sizeof(frame), 0x42, 1, 0);
        fake->last_send = interop_host_feature_send(sender, frame, length);
        if (fake->last_send == HostFeatureSuccess) ++fake->sent;
    } else if (fake->send_on_tick == 2 && fake->sent == 0) {
        length = build_launch_request(frame, sizeof(frame), 0);
        fake->last_send = interop_host_feature_send_request(sender, frame, length, &request_id);
        if (fake->last_send == HostFeatureSuccess) {
            ++fake->sent;
            fake->request_id = request_id;
        }
    }
    return fake->tick_status;
}

static uint64_t fake_next_deadline(void *context, uint64_t now_ms)
{
    fake_t *fake = context;
    (void)now_ms;
    return fake->deadline;
}

static void fake_on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender)
{
    fake_t *fake = context;
    uint8_t frame[256];
    size_t length = build_window_close(frame, sizeof(frame), 0x66, 3, 0);
    ++fake->closing_calls;
    fake->closing_order = ++closing_sequence;
    fake->closing_reason = reason;
    fake->closing_send = interop_host_feature_send(sender, frame, length);
}

/* ---- Substituted runtime entry: the fixture's control inputs wrapped with the app handler table. ---- */

static session_handler_t app_table[SessionAppMaxHandlers];
static size_t app_count;

static interop_host_control_result_t app_runner(const interop_host_control_config_t *control,
    interop_host_control_owners_t *owners, interop_host_control_detail_t *detail)
{
    session_app_config_t app;
    memset(&app, 0, sizeof(app));
    app.control = *control;
    app.handlers = app_count == 0 ? NULL : app_table;
    app.handler_count = app_count;
    return session_app_run_detailed(&app, owners, detail);
}

static int fake_on_error(void *context, uint32_t error_code, uint32_t request_id,
                         interop_host_feature_sender_t *sender)
{
    fake_t *fake = context;
    (void)sender;
    ++fake->error_calls;
    fake->error_code = error_code;
    fake->error_request = request_id;
    return fake->error_status;
}

static void make_fake_handler(session_handler_t *handler, fake_t *fake, const char *name, uint64_t caps)
{
    memset(handler, 0, sizeof(*handler));
    handler->name = name;
    handler->context = fake;
    handler->capabilities = caps;
    handler->owns_type = fake_owns;
    handler->on_ready = fake_on_ready;
    handler->on_frame = fake_on_frame;
    handler->poll_fds = fake_poll_fds;
    handler->on_fds = fake_on_fds;
    handler->on_tick = fake_on_tick;
    handler->next_deadline_ms = fake_next_deadline;
    handler->on_closing = fake_on_closing;
    handler->on_error = fake->skip_error ? NULL : fake_on_error;
}

static void fake_defaults(fake_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->deadline = UINT64_MAX;
    fake->pipe_read[0] = -1;
}

static void open_pipes(fake_t *fake, uint32_t count)
{
    uint32_t index;
    for (index = 0; index < count; ++index) {
        int pair[2];
        assert(pipe2(pair, O_NONBLOCK | O_CLOEXEC) == 0);
        fake->pipe_read[index] = pair[0];
        fake->pipe_write[index] = pair[1];
    }
    fake->poll_count = count;
}

static void close_pipes(fake_t *fake)
{
    uint32_t index;
    for (index = 0; index < fake->poll_count; ++index) {
        assert(close(fake->pipe_read[index]) == 0);
        assert(close(fake->pipe_write[index]) == 0);
    }
    fake->poll_count = 0;
}

static void reset_guest(uint64_t hello_caps, uint64_t ack_caps)
{
    peer_hello_capabilities = hello_caps;
    peer_expected_ack_capabilities = ack_caps;
    peer_script_enabled = true;
    peer_script_announces = 0;
    peer_script_echo_launch = false;
    peer_script_close_after = false;
    peer_script_lease_revoked = false;
    peer_script_suppress_pong = false;
    peer_received_window_close = 0;
    peer_received_launch_requests = 0;
    peer_received_other_features = 0;
    peer_last_launch_request_id = 0;
}

static void reset_guest_after(void)
{
    peer_script_enabled = false;
    peer_script_announces = 0;
    peer_script_echo_launch = false;
    peer_script_close_after = false;
    peer_script_lease_revoked = false;
    peer_hello_capabilities = InteropCapWindows | InteropCapInput;
    peer_expected_ack_capabilities = 0;
}

static interop_host_control_result_t run_app(uint32_t count, int run_ms, uint32_t cancel_ms)
{
    interop_host_control_result_t result;
    app_count = count;
    fixture_control_runner = app_runner;
    result = run_control_fixture(false, false, false, cancel_ms, false, false, false, 0,
        false, 0, false, false, run_ms, NULL);
    fixture_control_runner = fixture_run_default;
    app_count = 0;
    return result;
}

/* ---- Validation: every rejected table returns InvalidArgument before any socket is touched. ---- */

static void expect_invalid(const session_app_config_t *config)
{
    interop_host_control_owners_t owners;
    memset(&owners, 0, sizeof(owners));
    assert(session_app_run(config, &owners) == HostControlInvalidArgument);
    assert(owners.session_owner == NULL && owners.mapping_owner == NULL);
}

static void test_validation(void)
{
    session_app_config_t config;
    session_handler_t table[2];
    interop_host_session_config_t admission;
    interop_host_control_owners_t owners;
    fake_t one;
    fake_t two;
    interop_host_control_result_t result;

    memset(&admission, 0, sizeof(admission));
    memset(&config, 0, sizeof(config));
    config.control.admission = &admission;
    config.control.mapping_fd = 5;
    config.control.stop_fd = 7;
    config.control.run_for_ms = 100;
    config.control.backing_kind = InteropMappingLinuxBackingRegularPrivateFile;
    admission.cancel_fd = 7;
    admission.daemon_fd = 8;
    fake_defaults(&one);
    fake_defaults(&two);
    make_fake_handler(&table[0], &one, "alpha", InteropCapWindows);
    make_fake_handler(&table[1], &two, "beta", InteropCapLaunch);

    config.handlers = NULL;
    config.handler_count = 1;
    expect_invalid(&config);
    config.handlers = table;
    config.handler_count = SessionAppMaxHandlers + 1;
    expect_invalid(&config);
    config.handler_count = 2;
    table[1].name = NULL;
    expect_invalid(&config);
    table[1].name = "alpha";
    expect_invalid(&config); /* duplicate name */
    table[1].name = "beta";
    table[1].owns_type = NULL;
    expect_invalid(&config);
    table[1].owns_type = fake_owns;
    table[1].capabilities = UINT64_C(1) << 63;
    expect_invalid(&config);
    table[1].capabilities = InteropCapLaunch;
    table[1].poll_fds = fake_poll_fds;
    table[1].on_fds = NULL;
    expect_invalid(&config);
    table[1].on_fds = fake_on_fds;
    config.control.feature_ops = (const interop_host_feature_ops_t *)(void *)&table[0];
    expect_invalid(&config);
    config.control.feature_ops = NULL;
    admission.local_capabilities = InteropCapWindows;
    expect_invalid(&config);
    admission.local_capabilities = 0;
    config.handler_count = 0;
    expect_invalid(&config); /* empty count with a table pointer is rejected */
    config.handlers = NULL;
    config.handler_count = 0;
    config.control.admission = NULL;
    expect_invalid(&config);
    config.control.admission = &admission;
    memset(&owners, 0, sizeof(owners));
    result = session_app_run(&config, &owners);
    assert(result == HostControlInvalidArgument);
    assert(session_app_run(NULL, &owners) == HostControlInvalidArgument);
}

/* ---- Routing, union offer, descriptors, closing and requests across handlers. ---- */

static void test_routing_and_union(void)
{
    fake_t windows;
    fake_t launch;
    session_handler_t table[2];
    interop_host_control_result_t result;

    fake_defaults(&windows);
    fake_defaults(&launch);
    windows.owned_type = InteropMsgWindowAnnounce;
    launch.owned_type = InteropMsgLaunchResult;
    launch.send_on_tick = 2;
    make_fake_handler(&table[0], &windows, "windows", InteropCapWindows);
    make_fake_handler(&table[1], &launch, "launch", InteropCapLaunch);
    app_table[0] = table[0];
    app_table[1] = table[1];

    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    peer_script_announces = 2;
    peer_script_echo_launch = true;
    result = run_app(2, 600, 0);
    assert(result == HostControlSuccess);
    assert(windows.ready_calls == 1 && launch.ready_calls == 1);
    assert(windows.negotiated == (InteropCapWindows | InteropCapLaunch));
    assert(windows.frame_calls == 2 && windows.last_frame_type == InteropMsgWindowAnnounce);
    assert(launch.frame_calls == 1 && launch.last_frame_type == InteropMsgLaunchResult);
    assert(launch.last_frame_id == launch.request_id && launch.request_id != 0);
    assert(peer_received_launch_requests == 1 && peer_last_launch_request_id == launch.request_id);
    assert(windows.closing_calls == 1 && launch.closing_calls == 1);
    assert(launch.closing_order < windows.closing_order);
    assert(windows.closing_send == HostFeatureDraining && launch.closing_send == HostFeatureDraining);
    assert(windows.closing_reason == HostControlSuccess);
    reset_guest_after();
}

static void test_request_ids_and_sends_reach_guest(void)
{
    fake_t first;
    fake_t second;
    session_handler_t table[2];
    interop_host_control_result_t result;

    fake_defaults(&first);
    fake_defaults(&second);
    first.send_on_tick = 1;
    second.send_on_tick = 2;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    make_fake_handler(&table[1], &second, "second", InteropCapLaunch);
    app_table[0] = table[0];
    app_table[1] = table[1];

    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    result = run_app(2, 500, 0);
    assert(result == HostControlSuccess);
    assert(first.sent == 1 && first.last_send == HostFeatureSuccess);
    assert(peer_received_window_close == 1);
    assert(second.sent == 1 && second.request_id != 0);
    assert(peer_received_launch_requests == 1 && peer_last_launch_request_id == second.request_id);
    reset_guest_after();
}

static void test_descriptor_slices(void)
{
    fake_t left;
    fake_t right;
    session_handler_t table[2];
    interop_host_control_result_t result;

    fake_defaults(&left);
    fake_defaults(&right);
    open_pipes(&left, 2);
    open_pipes(&right, 2);
    left.write_on_first_tick = true;
    make_fake_handler(&table[0], &left, "left", InteropCapWindows);
    make_fake_handler(&table[1], &right, "right", InteropCapLaunch);
    app_table[0] = table[0];
    app_table[1] = table[1];

    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    result = run_app(2, 400, 0);
    assert(result == HostControlSuccess);
    assert(left.fds_calls >= 1 && (left.fds_ready_mask & 1) != 0);
    assert(right.fds_calls == 0);
    assert(left.tick_calls >= 1 && right.tick_calls >= 1);
    close_pipes(&left);
    close_pipes(&right);
    reset_guest_after();
}

static void test_descriptor_budget_exceeded(void)
{
    fake_t left;
    fake_t right;
    fake_t third;
    session_handler_t table[3];
    interop_host_control_result_t result;

    fake_defaults(&left);
    fake_defaults(&right);
    fake_defaults(&third);
    open_pipes(&left, 4);
    open_pipes(&right, 4);
    open_pipes(&third, 4);
    make_fake_handler(&table[0], &left, "left", InteropCapWindows);
    make_fake_handler(&table[1], &right, "right", InteropCapLaunch);
    make_fake_handler(&table[2], &third, "third", InteropCapCatalog);
    app_table[0] = table[0];
    app_table[1] = table[1];
    app_table[2] = table[2];

    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    result = run_app(3, 2000, 0);
    assert(result == HostControlFeatureFailed);
    assert(left.closing_calls == 1 && right.closing_calls == 1 && third.closing_calls == 1);
    close_pipes(&left);
    close_pipes(&right);
    close_pipes(&third);
    reset_guest_after();
}

/* ---- Failure in each callback and the ownership, poll and graceful stop outcomes. ---- */

static void run_single(fake_t *fake, session_handler_t *handler, uint32_t announces,
    const char *name, interop_host_control_result_t expected)
{
    interop_host_control_result_t result;
    make_fake_handler(handler, fake, name, InteropCapWindows);
    app_table[0] = *handler;
    reset_guest(InteropCapWindows, InteropCapWindows);
    peer_script_announces = announces;
    result = run_app(1, 2000, 0);
    assert(result == expected);
    assert(fake->closing_calls == 1);
    assert(fake->closing_reason == (uint32_t)expected);
    reset_guest_after();
}

static void test_callback_failures(void)
{
    fake_t fake;
    session_handler_t handler;

    fake_defaults(&fake);
    fake.ready_status = 2;
    run_single(&fake, &handler, 0, "ready-fail", HostControlFeatureFailed);

    fake_defaults(&fake);
    fake.owned_type = InteropMsgWindowAnnounce;
    fake.frame_status = 2;
    run_single(&fake, &handler, 1, "frame-fail", HostControlFeatureFailed);

    fake_defaults(&fake);
    fake.tick_status = 2;
    run_single(&fake, &handler, 0, "tick-fail", HostControlFeatureFailed);

    fake_defaults(&fake);
    open_pipes(&fake, 1);
    fake.write_on_first_tick = true;
    fake.fds_status = 2;
    run_single(&fake, &handler, 0, "fds-fail", HostControlFeatureFailed);
    close_pipes(&fake);

    fake_defaults(&fake);
    fake.poll_status = 2;
    run_single(&fake, &handler, 0, "poll-fail", HostControlFeatureFailed);
}

static void test_ownership_conflicts(void)
{
    fake_t first;
    fake_t second;
    session_handler_t table[2];
    interop_host_control_result_t result;

    /* Two owners of one type: the frame has no single owner, so the run fails. */
    fake_defaults(&first);
    fake_defaults(&second);
    first.owned_type = InteropMsgWindowAnnounce;
    second.owned_type = InteropMsgWindowAnnounce;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    make_fake_handler(&table[1], &second, "second", InteropCapWindows);
    app_table[0] = table[0];
    app_table[1] = table[1];
    reset_guest(InteropCapWindows, InteropCapWindows);
    peer_script_announces = 1;
    result = run_app(2, 2000, 0);
    assert(result == HostControlFeatureFailed);
    assert(first.frame_calls == 0 && second.frame_calls == 0);
    assert(first.closing_calls == 1 && second.closing_calls == 1);
    assert(second.closing_order < first.closing_order);
    reset_guest_after();

    /* No owner of an admitted type: the run fails rather than dropping the frame. */
    fake_defaults(&first);
    first.owned_type = InteropMsgLaunchResult;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    app_table[0] = table[0];
    reset_guest(InteropCapWindows, InteropCapWindows);
    peer_script_announces = 1;
    result = run_app(1, 2000, 0);
    assert(result == HostControlFeatureFailed);
    assert(first.closing_calls == 1);
    reset_guest_after();
}

static void test_peer_errors(void)
{
    fake_t first;
    fake_t second;
    session_handler_t table[2];
    interop_host_control_result_t result;

    /* A scoped remote Error reaches each handler that defines on_error, in registration order. */
    fake_defaults(&first);
    fake_defaults(&second);
    second.skip_error = true;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    make_fake_handler(&table[1], &second, "second", InteropCapLaunch);
    app_table[0] = table[0];
    app_table[1] = table[1];
    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    arm_peer_send_error = true;
    result = run_app(2, 300, 0);
    assert(result == HostControlSuccess);
    assert(first.error_calls == 1 && first.error_code == 1 && first.error_request == 2);
    assert(second.error_calls == 0 && first.closing_calls == 1 && second.closing_calls == 1);
    reset_guest_after();

    /* A stop from on_error ends the run as Cancelled. */
    fake_defaults(&first);
    first.error_status = SessionHandlerRequestStop;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    app_table[0] = table[0];
    reset_guest(InteropCapWindows, InteropCapWindows);
    arm_peer_send_error = true;
    result = run_app(1, 2000, 0);
    assert(result == HostControlCancelled);
    assert(first.error_calls == 1 && first.closing_calls == 1);
    reset_guest_after();

    /* Any other nonzero result from on_error fails the run. */
    fake_defaults(&first);
    first.error_status = 7;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    app_table[0] = table[0];
    reset_guest(InteropCapWindows, InteropCapWindows);
    arm_peer_send_error = true;
    result = run_app(1, 2000, 0);
    assert(result == HostControlFeatureFailed);
    assert(first.error_calls == 1 && first.closing_calls == 1);
    reset_guest_after();
}

static void test_graceful_stop_and_closing(void)
{
    fake_t first;
    fake_t second;
    session_handler_t table[2];
    interop_host_control_result_t result;

    /* A stop from one handler ends the run as Cancelled and the others still close. */
    fake_defaults(&first);
    fake_defaults(&second);
    first.tick_status = SessionHandlerRequestStop;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    make_fake_handler(&table[1], &second, "second", InteropCapLaunch);
    app_table[0] = table[0];
    app_table[1] = table[1];
    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    result = run_app(2, 2000, 0);
    assert(result == HostControlCancelled);
    assert(first.closing_calls == 1 && second.closing_calls == 1);
    assert(second.closing_order < first.closing_order);
    reset_guest_after();

    /* A stop from on_ready leaves later handlers unreadied, but every handler still closes once. */
    fake_defaults(&first);
    fake_defaults(&second);
    first.ready_status = SessionHandlerRequestStop;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    make_fake_handler(&table[1], &second, "second", InteropCapLaunch);
    app_table[0] = table[0];
    app_table[1] = table[1];
    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    result = run_app(2, 2000, 0);
    assert(result == HostControlCancelled);
    assert(first.ready_calls == 1 && second.ready_calls == 0);
    assert(first.closing_calls == 1 && second.closing_calls == 1);
    reset_guest_after();

    /* A handler without on_closing is skipped; a never-readied handler still closes its peers. */
    fake_defaults(&first);
    fake_defaults(&second);
    second.ready_status = 2;
    make_fake_handler(&table[0], &first, "first", InteropCapWindows);
    make_fake_handler(&table[1], &second, "second", InteropCapLaunch);
    table[0].on_closing = NULL;
    app_table[0] = table[0];
    app_table[1] = table[1];
    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    result = run_app(2, 2000, 0);
    assert(result == HostControlFeatureFailed);
    assert(first.ready_calls == 1 && first.closing_calls == 0);
    assert(second.closing_calls == 1);
    reset_guest_after();
}

static void test_deadlines_and_capability_zero(void)
{
    fake_t fake;
    session_handler_t table[1];
    interop_host_control_result_t result;

    fake_defaults(&fake);
    fake.deadline = UINT64_C(1) << 62;
    make_fake_handler(&table[0], &fake, "deadline", InteropCapWindows);
    app_table[0] = table[0];
    reset_guest(InteropCapWindows, InteropCapWindows);
    result = run_app(1, 300, 0);
    assert(result == HostControlSuccess);
    assert(fake.tick_calls >= 1 && fake.closing_calls == 1);
    reset_guest_after();

    /* Zero handlers: the plain capability-zero runtime, with no feature ops installed. */
    reset_guest(InteropCapWindows | InteropCapInput, 0);
    result = run_app(0, 300, 0);
    assert(result == HostControlSuccess);
    reset_guest_after();
}

/* Handlers that provide only the mandatory owns_type: every optional callback is absent. */
/* The builtin registry: validation, the offered table, forwarding before Ready, and idempotent destroy. */
static void test_builtin_registry(void)
{
    session_builtin_storage_t storage;
    session_builtin_config_t config;
    session_handler_t handlers[SessionAppMaxHandlers];
    size_t count = 99;
    /* The sender is opaque here; before Ready no forwarder touches it, so NULL is passed. */
    interop_host_feature_sender_t *sender = NULL;
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t fd_count = 7;

    memset(&config, 0, sizeof(config));
    memset(&storage, 0, sizeof(storage));
    config.runtime_dir = "/run/user/1000";
    config.device_name = "alpha";
    config.storage = &storage;
    assert(session_handlers_builtin_make(NULL, handlers, &count) == 1);
    assert(session_handlers_builtin_make(&config, NULL, &count) == 1);
    assert(session_handlers_builtin_make(&config, handlers, NULL) == 1);
    config.storage = NULL;
    assert(session_handlers_builtin_make(&config, handlers, &count) == 1);
    config.storage = &storage;
    config.runtime_dir = NULL;
    assert(session_handlers_builtin_make(&config, handlers, &count) == 1);
    config.runtime_dir = "relative";
    assert(session_handlers_builtin_make(&config, handlers, &count) == 1);
    config.runtime_dir = "/run/user/1000";
    config.device_name = NULL;
    assert(session_handlers_builtin_make(&config, handlers, &count) == 1);
    config.device_name = "../escape";
    assert(session_handlers_builtin_make(&config, handlers, &count) == 1);
    assert(count == 99);

    config.device_name = "alpha";
    assert(session_handlers_builtin_make(&config, handlers, &count) == 0);
    assert(count == 1 && strcmp(handlers[0].name, "launch") == 0);
    assert(handlers[0].capabilities == InteropCapLaunch && handlers[0].context == &storage);
    assert(handlers[0].owns_type(&storage, InteropMsgLocalLaunch) == 0);
    assert(handlers[0].owns_type(&storage, InteropMsgLaunchResult) == 0);
    /* Before Ready nothing is initialised: forwarding is inert and closing is a no-op. */
    fd_count = 7;
    assert(handlers[0].poll_fds(&storage, fds, SessionHandlerMaxDescriptors, &fd_count) == 0 && fd_count == 0);
    assert(handlers[0].on_tick(&storage, 1, sender) == 0);
    assert(handlers[0].on_fds(&storage, fds, 0, 1, sender) == 0);
    assert(handlers[0].next_deadline_ms(&storage, 1) == UINT64_MAX);
    assert(handlers[0].on_frame(&storage, InteropMsgLaunchResult, 1, NULL, 0, sender) == SessionBuiltinFailure);
    handlers[0].on_closing(&storage, HostControlSuccess, sender);
    assert(!storage.launch_ready && !storage.launch_initialized);
    /* Destroy is a no-op before initialisation and after it, and never touches the table. */
    session_handlers_builtin_destroy(NULL);
    session_handlers_builtin_destroy(&storage);
    session_handlers_builtin_destroy(&storage);
}

static void test_minimal_handlers(void)
{
    fake_t owner;
    fake_t bare;
    fake_t reporter;
    session_handler_t table[3];
    interop_host_control_result_t result;

    fake_defaults(&owner);
    fake_defaults(&bare);
    fake_defaults(&reporter);
    owner.owned_type = InteropMsgWindowAnnounce;
    make_fake_handler(&table[0], &owner, "owner", InteropCapWindows);
    table[0].on_ready = NULL;
    table[0].on_tick = NULL;
    table[0].next_deadline_ms = NULL;
    table[0].poll_fds = NULL;
    table[0].on_fds = NULL;
    table[0].on_closing = NULL;
    make_fake_handler(&table[1], &bare, "bare", InteropCapLaunch);
    table[1].on_ready = NULL;
    table[1].on_frame = NULL;
    table[1].on_tick = NULL;
    table[1].next_deadline_ms = NULL;
    table[1].poll_fds = NULL;
    table[1].on_fds = NULL;
    table[1].on_closing = NULL;
    app_table[0] = table[0];
    app_table[1] = table[1];
    reset_guest(InteropCapWindows | InteropCapLaunch, InteropCapWindows | InteropCapLaunch);
    peer_script_announces = 1;
    result = run_app(2, 400, 0);
    assert(result == HostControlSuccess);
    assert(owner.frame_calls == 1 && bare.frame_calls == 0);
    reset_guest_after();

    /* An owner without on_frame cannot take its frame, so the run fails. */
    make_fake_handler(&table[0], &owner, "owner", InteropCapWindows);
    table[0].on_frame = NULL;
    app_table[0] = table[0];
    reset_guest(InteropCapWindows, InteropCapWindows);
    peer_script_announces = 1;
    result = run_app(1, 2000, 0);
    assert(result == HostControlFeatureFailed);
    reset_guest_after();

    /* A handler reporting more than four descriptors fails the poll cycle. */
    fake_defaults(&reporter);
    reporter.poll_report = SessionHandlerMaxDescriptors + 1;
    make_fake_handler(&table[0], &reporter, "reporter", InteropCapWindows);
    app_table[0] = table[0];
    reset_guest(InteropCapWindows, InteropCapWindows);
    result = run_app(1, 2000, 0);
    assert(result == HostControlFeatureFailed);
    assert(reporter.closing_calls == 1);
    reset_guest_after();
}

static void test_stress(void)
{
    fake_t sender;
    fake_t receiver;
    session_handler_t table[2];
    interop_host_control_result_t result;

    fake_defaults(&sender);
    fake_defaults(&receiver);
    sender.send_on_tick = 1;
    receiver.owned_type = InteropMsgWindowAnnounce;
    make_fake_handler(&table[0], &sender, "sender", InteropCapWindows);
    make_fake_handler(&table[1], &receiver, "receiver", InteropCapWindows);
    app_table[0] = table[0];
    app_table[1] = table[1];
    reset_guest(InteropCapWindows, InteropCapWindows);
    peer_script_announces = 1000;
    result = run_app(2, 3000, 0);
    assert(result == HostControlSuccess);
    assert(receiver.frame_calls == 1000);
    assert(sender.closing_calls == 1 && receiver.closing_calls == 1);
    reset_guest_after();
}

static void test_app_cancellation(void)
{
    fake_t fake;
    session_handler_t table[1];
    interop_host_control_result_t result;

    fake_defaults(&fake);
    make_fake_handler(&table[0], &fake, "cancel", InteropCapWindows);
    app_table[0] = table[0];
    reset_guest(InteropCapWindows, InteropCapWindows);
    result = run_app(1, 0, 200);
    assert(result == HostControlCancelled);
    assert(fake.closing_calls == 1 && fake.closing_reason == HostControlCancelled);
    reset_guest_after();
}

/* ---- One integration test: the real application runtime under the real supervisor seams. ---- */

static uint32_t integration_attempt;
static fake_t integration_fake;

static interop_host_control_result_t integration_run(void *context, const session_app_config_t *app,
    interop_host_control_owners_t *owners, interop_host_control_detail_t *detail)
{
    interop_host_control_result_t result;
    (void)context;
    (void)app;
    (void)owners;
    ++integration_attempt;
    reset_guest(InteropCapWindows, InteropCapWindows);
    /* The guest drops once after HelloAck, then returns on the next attempt. */
    peer_script_close_after = integration_attempt == 1;
    fixture_detail_out = detail;
    result = run_app(1, integration_attempt == 1 ? 2000 : 400, 0);
    fixture_detail_out = NULL;
    reset_guest_after();
    return result;
}

static void test_supervised_reconnect(void)
{
    session_handler_t table[1];
    session_supervisor_config_t config;
    session_supervisor_report_t report;
    interop_host_control_owners_t owners;
    int stop_pair[2];

    fake_defaults(&integration_fake);
    make_fake_handler(&table[0], &integration_fake, "integration", InteropCapWindows);
    app_table[0] = table[0];
    assert(pipe2(stop_pair, O_NONBLOCK | O_CLOEXEC) == 0);
    memset(&config, 0, sizeof(config));
    memset(&owners, 0, sizeof(owners));
    config.app.control.stop_fd = stop_pair[0];
    config.seams = session_supervisor_system_seams();
    config.seams.run = integration_run;
    config.max_retries = 0;
    assert(session_supervisor_run(&config, &owners, &report) == HostControlSuccess);
    assert(integration_attempt == 2);
    assert(report.attempts == 2 && report.retries == 1);
    assert(report.ready_sessions >= 1);
    assert(report.backoff_total_ms == SessionSupervisorInitialBackoffMs);
    assert(owners.session_owner == NULL && owners.mapping_owner == NULL);
    assert(close(stop_pair[0]) == 0 && close(stop_pair[1]) == 0);
}

int main(void)
{
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    test_builtin_registry();
    test_validation();
    test_routing_and_union();
    test_request_ids_and_sends_reach_guest();
    test_descriptor_slices();
    test_descriptor_budget_exceeded();
    test_callback_failures();
    test_ownership_conflicts();
    test_graceful_stop_and_closing();
    test_peer_errors();
    test_deadlines_and_capability_zero();
    test_stress();
    test_minimal_handlers();
    test_app_cancellation();
    test_supervised_reconnect();
    session_supervisor_unit_tests();
    puts("session_app tests passed");
    return 0;
}
