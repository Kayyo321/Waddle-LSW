#ifndef WaddleInteropSessionControlWindowsApiDoubleH
#define WaddleInteropSessionControlWindowsApiDoubleH
/** @file session_control_windows_api_double.h
 * @brief Narrow, test-only WinSock/Win32/mapping seams for the session owner.
 */
#include "windows_socket_owner_api_double.h"
#include "interop_native_atomic.h"

typedef struct interop_guest_test_wsa_data_t { uint32_t version; } WSADATA;
typedef struct interop_guest_test_pollfd_t { SOCKET fd; short events; short revents; } WSAPOLLFD;
#define MAKEWORD(low, high) ((uint16_t)(((uint8_t)(low)) | ((uint16_t)(uint8_t)(high) << 8)))
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80u
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#ifndef ERROR_INVALID_DATA
#define ERROR_INVALID_DATA 13u
#endif

/** @brief One deterministic failure point selected by the portable fixture. */
typedef enum interop_guest_test_failure_t {
    InteropGuestTestNoFailure = 0,
    InteropGuestTestFailStartup = 1,
    InteropGuestTestFailOpenFamily = 2,
    InteropGuestTestFailQueryFamily = 3,
    InteropGuestTestBadFamily = 4,
    InteropGuestTestFailCloseHandle = 5,
    InteropGuestTestFailCreateListener = 6,
    InteropGuestTestFailListenerInherit = 7,
    InteropGuestTestFailListenerNonblock = 8,
    InteropGuestTestFailBind = 9,
    InteropGuestTestFailListen = 10,
    InteropGuestTestFailListenerPoll = 11,
    InteropGuestTestFailAccept = 12,
    InteropGuestTestFailAcceptedInherit = 13,
    InteropGuestTestFailAcceptedNonblock = 14,
    InteropGuestTestFailPeerQuery = 15,
    InteropGuestTestBadPeer = 16,
    InteropGuestTestFailSocketCreate = 17,
    InteropGuestTestFailMappingCreate = 18,
    InteropGuestTestFailMappingOpen = 19,
    InteropGuestTestFailRandom = 20,
    InteropGuestTestFailMappingResult = 21,
    InteropGuestTestFailSocketPoll = 22,
    InteropGuestTestFailSessionPoll = 23,
    InteropGuestTestFailSocketDrive = 24,
    InteropGuestTestReadyCapabilities = 25,
    InteropGuestTestHeaderInvalid = 26,
    InteropGuestTestFeatureEvent = 27,
    InteropGuestTestDrainingEvent = 28,
    InteropGuestTestHeartbeatMismatch = 29,
    InteropGuestTestMapClose = 30,
    InteropGuestTestSocketClosePending = 31,
    InteropGuestTestSocketCloseIndeterminate = 32,
    InteropGuestTestStop = 33,
    InteropGuestTestFailSocketClose = 34,
    InteropGuestTestFailWsaCleanup = 35,
    InteropGuestTestBadPollEvent = 36,
    InteropGuestTestNoIncomingPeer = 37,
    InteropGuestTestFailMappingInitialize = 38,
    InteropGuestTestFailStopWait = 39,
    InteropGuestTestClockBackwards = 40,
    InteropGuestTestClockError = 41,
    InteropGuestTestFailSocketClosePending = 42,
    InteropGuestTestFailMappingDestroy = 43,
    InteropGuestTestBadPeerLength = 44,
    InteropGuestTestBadPeerFamily = 45,
    InteropGuestTestBadPeerReserved = 46,
    InteropGuestTestErrorEvent = 47,
    InteropGuestTestNonreadyPhase = 48,
    InteropGuestTestImmediateWork = 49,
    InteropGuestTestFailGuestHeartbeatLoad = 50,
    InteropGuestTestFailHostHeartbeatLoad = 51,
    InteropGuestTestFailGuestHeartbeatStore = 52,
    InteropGuestTestFailSessionAllocation = 53,
    InteropGuestTestClockNearWrap = 54,
    InteropGuestTestNoReadyTimeout = 55,
    InteropGuestTestEofBeforeReady = 56,
    InteropGuestTestStopDuringRun = 57,
    InteropGuestTestFailStopWaitDuringRun = 58,
    InteropGuestTestRunTimeoutAfterReady = 59,
    InteropGuestTestListenerNoEvent = 60,
    InteropGuestTestShortPollTimeout = 61,
    InteropGuestTestZeroEpoch = 62,
    InteropGuestTestMaxHeartbeat = 63,
    InteropGuestTestSocketDestroyPeerFailure = 64,
    InteropGuestTestSocketWorkClosePending = 65,
    InteropGuestTestSocketWorkCloseIndeterminate = 66,
    InteropGuestTestStopAfterSessionPoll = 67,
    InteropGuestTestFailStopWaitAfterSessionPoll = 68,
    InteropGuestTestBadFamilySize = 69,
    InteropGuestTestZeroFamily = 70,
    InteropGuestTestPeerQueryAndCloseFailure = 71,
    InteropGuestTestRetrySocketClosePending = 72,
    InteropGuestTestSocketWorkPeerFailure = 73,
    InteropGuestTestFailHeartbeatLoadDuringRun = 74,
    InteropGuestTestBadFamilyCloseFailure = 75
} interop_guest_test_failure_t;

struct interop_mapping_windows_t;
struct interop_mapping_windows_view_t;
struct interop_windows_socket_t;
struct interop_windows_socket_config_t;
struct interop_windows_socket_poll_t;
struct interop_windows_socket_work_t;
struct interop_windows_socket_quarantine_t;
struct interop_peer_event_t;

int interop_guest_test_wsa_startup(uint16_t version, WSADATA *data);
uint32_t interop_guest_test_last_wsa_error_calls(void);
int interop_guest_test_wsa_cleanup(void);
HANDLE interop_guest_test_open_family(void);
int interop_guest_test_query_family(HANDLE handle, DWORD *family, DWORD *returned);
int interop_guest_test_close_handle(HANDLE handle);
SOCKET interop_guest_test_create_socket(int family);
int interop_guest_test_set_noninherit(SOCKET socket_value);
int interop_guest_test_set_nonblocking(SOCKET socket_value);
int interop_guest_test_bind(SOCKET socket_value, const void *address, int length);
int interop_guest_test_listen(SOCKET socket_value);
int interop_guest_test_poll(WSAPOLLFD *descriptor, unsigned long count, int timeout_ms);
SOCKET interop_guest_test_accept(SOCKET socket_value);
int interop_guest_test_peer_address(SOCKET socket_value, void *address, int *length);
int interop_guest_test_close_socket(SOCKET socket_value);
DWORD interop_guest_test_wait_stop(HANDLE event, DWORD timeout_ms);
uint64_t interop_guest_test_now(void);
DWORD interop_guest_test_thread_id(void);
int interop_guest_test_random(void *output, size_t length);
int interop_guest_test_last_wsa_error(void);
DWORD interop_guest_test_last_win32_error(void);
void interop_guest_test_sleep(uint32_t milliseconds);

int interop_guest_test_mapping_create(struct interop_mapping_windows_t **owner_out);
int interop_guest_test_mapping_open(struct interop_mapping_windows_t *owner,
                                    uint32_t device_index,
                                    struct interop_mapping_windows_view_t *view_out);
int interop_guest_test_mapping_close(struct interop_mapping_windows_t *owner, uint32_t detached_proof);
int interop_guest_test_mapping_destroy(struct interop_mapping_windows_t **owner_inout);
int interop_guest_test_mapping_initialize(uintptr_t mapping_base, uint64_t mapping_length,
    uint64_t session_epoch, uint32_t exclusive_owner_proof);
void *interop_guest_test_calloc(size_t count, size_t size);
interop_atomic_status_t interop_guest_test_heartbeat_load(const interop_shm_header_t *header,
    uintptr_t mapping_base, uint64_t mapping_length, interop_heartbeat_kind_t kind, uint64_t *value);
interop_atomic_status_t interop_guest_test_heartbeat_store_next(interop_shm_header_t *header,
    uintptr_t mapping_base, uint64_t mapping_length, interop_heartbeat_kind_t kind, uint64_t next_value);
int interop_guest_test_socket_create(SOCKET socket_value,
    const struct interop_windows_socket_config_t *config,
    struct interop_windows_socket_t **owner_out);
int interop_guest_test_socket_destroy(struct interop_windows_socket_t **owner_slot);
int interop_guest_test_socket_retry_close(struct interop_windows_socket_t *owner, uint64_t token);
int interop_guest_test_socket_poll_events(struct interop_windows_socket_t *owner,
    uint64_t token, uint64_t now_ms, struct interop_windows_socket_poll_t *out_poll);
int interop_guest_test_socket_drive(struct interop_windows_socket_t *owner, uint64_t token,
    uint64_t poll_cookie, uint64_t now_ms, uint32_t revents, size_t rx_byte_budget,
    size_t tx_byte_budget, uint32_t syscall_budget, uint8_t *frame_out,
    size_t frame_capacity, struct interop_windows_socket_work_t *out_work);
int interop_guest_test_socket_mapping_result(struct interop_windows_socket_t *owner,
    uint64_t token, uint64_t now_ms, uint64_t epoch, uint32_t layout_version,
    uint32_t succeeded, struct interop_peer_event_t *out_event);
int interop_guest_test_socket_begin_drain(struct interop_windows_socket_t *owner,
    uint64_t token, uint64_t now_ms, struct interop_peer_event_t *out_event);
int interop_guest_test_socket_quarantine_handoff(struct interop_windows_socket_t **owner_slot,
    uint64_t token, struct interop_windows_socket_quarantine_t *quarantine_out);
void interop_guest_test_reset(void);
void interop_guest_test_set_failure(interop_guest_test_failure_t failure);
void interop_guest_test_set_fast_timeout(uint32_t enabled);
void interop_guest_test_fail_clock_read(uint32_t read_number);
void interop_guest_test_override_clock_read(uint32_t read_number, uint64_t value);
/** @brief Probe pure private pointer-range and timeout helpers for branch tests. */
int interop_guest_test_session_helpers(const void *pointer, size_t length, size_t alignment,
    const void *left, size_t left_length, const void *right, size_t right_length,
    uint64_t now, uint64_t deadline, uint32_t *range_out, uint32_t *overlap_out,
    uint32_t *timeout_out);
/** @brief Probe the null-owner branch of the private thread helper. */
int interop_guest_test_is_owner_thread_null(void);
void interop_guest_test_set_thread_id(DWORD thread_id);
uint32_t interop_guest_test_wsa_calls(void);
uint32_t interop_guest_test_map_close_calls(void);
uint32_t interop_guest_test_socket_drive_calls(void);
uint32_t interop_guest_test_close_socket_calls(void);
uint64_t interop_guest_test_final_guest_heartbeat(void);

#endif
