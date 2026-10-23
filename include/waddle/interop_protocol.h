#ifndef WaddleInteropProtocolH
#define WaddleInteropProtocolH
/**
 * @file interop_protocol.h
 * @brief Seamless-interop control protocol v2 (frozen contract, see
 * impl/seamless-interop/IMPL_DESC.md section 3.2).
 *
 * A frame is a 16-byte little-endian header followed by an ordered sequence of
 * TLV fields: u16 tag | u16 length | value | zero padding to 4 bytes. Tags with
 * bit 0x8000 are critical. Validation, field access, frame writing and chunked
 * transfer reassembly are implemented in src/interop/interop_codec.zig; every
 * function is allocation-free, never retains caller memory, and leaves caller
 * outputs unchanged on failure. All functions are thread-safe for distinct
 * arguments; a writer or transfer object must not be shared between threads
 * without external locking.
 */
#include <stddef.h>
#include <stdint.h>

/** @brief Guest VSOCK listen port for interop v1. */
#define InteropControlPort 5002u
/** @brief Frame magic, bytes 'W' 'I' 'O' 'P'. */
#define InteropMagic 0x504F4957u
/** @brief Protocol version carried in every header. */
#define InteropVersion 2u
/** @brief Shared-memory version required by the current session endpoints. */
#define InteropLayoutVersion 3u
/** @brief Fixed header size in bytes. */
#define InteropHeaderBytes 16u
/** @brief Maximum payload bytes; the largest frame is 65536 bytes. */
#define InteropMaxPayloadBytes 65520u
/** @brief Largest complete frame in bytes. */
#define InteropMaxFrameBytes (InteropHeaderBytes + InteropMaxPayloadBytes)
/** @brief Maximum TLV fields per frame. */
#define InteropMaxFields 64u
/** @brief Maximum generic string field (paths, arguments) in bytes. */
#define InteropMaxStringBytes 4096u
/** @brief Maximum window title in bytes. */
#define InteropMaxTitleBytes 1024u
/** @brief Maximum application key in bytes. */
#define InteropMaxAppKeyBytes 200u
/** @brief Maximum concurrently exported windows. */
#define InteropMaxWindows 64u
/** @brief Maximum concurrent audio rings. */
#define InteropMaxAudioStreams 8u
/** @brief Maximum concurrent chunked transfers per direction. */
#define InteropMaxTransfers 16u
/** @brief Maximum chunk payload in bytes (60 KiB). */
#define InteropChunkBytes 61440u
/** @brief Largest transfer the protocol can describe (1 GiB). */
#define InteropMaxTransferBytes 1073741824u
/** @brief Hello/HelloAck deadline in milliseconds. */
#define InteropHelloTimeoutMs 5000u
/** @brief Host ping interval in milliseconds. */
#define InteropPingIntervalMs 2000u
/** @brief Missing-traffic disconnect deadline in milliseconds. */
#define InteropPingTimeoutMs 6000u
/** @brief WM_NCHITTEST probe timeout in milliseconds. */
#define InteropHitTestTimeoutMs 50u
/** @brief Per-direction bounded send queue depth in frames. */
#define InteropPeerQueueFrames 512u
/** @brief Bit marking a critical TLV tag on the wire. */
#define InteropCriticalTag 0x8000u
/** @brief SHA-256 digest size in bytes. */
#define InteropHashBytes 32u
/** @brief Valid bits of interop_window_state_t. */
#define InteropStateMask 0x7Fu
/** @brief Valid bits of interop_app_flag_t. */
#define InteropAppFlagMask 0x1Fu
/** @brief Valid bits of interop_capability_t. */
#define InteropCapabilityMask 0x1FFFFull

/** @brief Frame header in host byte order after decoding. */
typedef struct interop_header_t {
    uint32_t magic;      /**< InteropMagic. */
    uint16_t version;    /**< InteropVersion. */
    uint16_t type;       /**< interop_message_type_t. */
    uint32_t length;     /**< Payload bytes, 0..InteropMaxPayloadBytes, multiple of 4. */
    uint32_t request_id; /**< 0 unsolicited; responses echo the request id. */
} interop_header_t;

/** @brief Who produced a frame; selects which message types are legal. */
typedef enum interop_direction_t {
    InteropFromGuest = 1, /**< Guest session agent to host session client. */
    InteropFromHost = 2,  /**< Host session client to guest session agent. */
    InteropFromLocal = 3  /**< Either local session.sock peer; never VSOCK. */
} interop_direction_t;

/** @brief Message types (IMPL_DESC 3.2.5). */
typedef enum interop_message_type_t {
    InteropMsgHello = 0x0001,
    InteropMsgHelloAck = 0x0002,
    InteropMsgGoodbye = 0x0003,
    InteropMsgPing = 0x0004,
    InteropMsgPong = 0x0005,
    InteropMsgError = 0x0006,
    InteropMsgWindowAnnounce = 0x0100,
    InteropMsgWindowUpdate = 0x0101,
    InteropMsgWindowRetire = 0x0102,
    InteropMsgWindowConfigure = 0x0103,
    InteropMsgWindowClose = 0x0104,
    InteropMsgWindowMoveRequest = 0x0105,
    InteropMsgWindowResizeRequest = 0x0106,
    InteropMsgWindowStateRequest = 0x0107,
    InteropMsgFrameReady = 0x0108,
    InteropMsgFrameRelease = 0x0109,
    InteropMsgWindowIconChanged = 0x010A,
    InteropMsgPopupRepositioned = 0x010B,
    InteropMsgPopupDismissed = 0x010C,
    InteropMsgInputFocus = 0x0200,
    InteropMsgInputKey = 0x0201,
    InteropMsgInputPointerMotion = 0x0202,
    InteropMsgInputPointerRelative = 0x0203,
    InteropMsgInputPointerButton = 0x0204,
    InteropMsgInputPointerAxis = 0x0205,
    InteropMsgInputTouch = 0x0206,
    InteropMsgInputTouchFrame = 0x0207,
    InteropMsgInputTouchCancel = 0x0208,
    InteropMsgInputTablet = 0x0209,
    InteropMsgInputTabletProximity = 0x020A,
    InteropMsgInputGesturePinch = 0x020B,
    InteropMsgInputTextCommit = 0x020C,
    InteropMsgInputTextPreedit = 0x020D,
    InteropMsgInputRelease = 0x020E,
    InteropMsgInputLockState = 0x020F,
    InteropMsgInputRepeatInfo = 0x0210,
    InteropMsgKeyboardLayout = 0x0211,
    InteropMsgCaretRect = 0x0212,
    InteropMsgLeaseRevoked = 0x0213,
    InteropMsgLeaseAck = 0x0214,
    InteropMsgLeaseReady = 0x0215,
    InteropMsgCursorShape = 0x0300,
    InteropMsgCursorConstraint = 0x0301,
    InteropMsgClipboardOffer = 0x0400,
    InteropMsgClipboardRequest = 0x0401,
    InteropMsgClipboardClear = 0x0402,
    InteropMsgDndEnter = 0x0500,
    InteropMsgDndMotion = 0x0501,
    InteropMsgDndLeave = 0x0502,
    InteropMsgDndDrop = 0x0503,
    InteropMsgDndResult = 0x0504,
    InteropMsgDndSourceStart = 0x0505,
    InteropMsgDndSourceRequest = 0x0506,
    InteropMsgDndSourceFinished = 0x0507,
    InteropMsgCatalogBegin = 0x0600,
    InteropMsgCatalogAdd = 0x0601,
    InteropMsgCatalogUpdate = 0x0602,
    InteropMsgCatalogRemove = 0x0603,
    InteropMsgCatalogEnd = 0x0604,
    InteropMsgIconRequest = 0x0605,
    InteropMsgLaunchRequest = 0x0700,
    InteropMsgLaunchResult = 0x0701,
    InteropMsgElevationNotice = 0x0702,
    InteropMsgUacPrompt = 0x0703,
    InteropMsgUacDecision = 0x0704,
    InteropMsgDesktopSwitch = 0x0705,
    InteropMsgAudioStreamOpen = 0x0800,
    InteropMsgAudioStreamClose = 0x0801,
    InteropMsgPrinterSync = 0x0900,
    InteropMsgPrinterResult = 0x0901,
    InteropMsgDisplayConfig = 0x0A00,
    InteropMsgDisplayApplied = 0x0A01,
    InteropMsgTransferBegin = 0x0F00,
    InteropMsgTransferChunk = 0x0F01,
    InteropMsgTransferEnd = 0x0F02,
    InteropMsgTransferCancel = 0x0F03,
    /** Local session.sock request from `waddle open`; LaunchRequest fields. The
     * reply on that socket is a LaunchResult validated as InteropFromGuest
     * because waddle-session relays the guest's result unchanged. */
    InteropMsgLocalLaunch = 0x7F00,
    InteropMsgLocalStop = 0x7F01, /**< Local session stop request; nonzero request id. */
    InteropMsgLocalStopResult = 0x7F02 /**< Local stop result; echoes request id. */
} interop_message_type_t;

/** @brief Field tags without the critical bit (IMPL_DESC 3.2.3). */
typedef enum interop_tag_t {
    InteropTagWindow = 0x0001,
    InteropTagGeneration = 0x0002,
    InteropTagParent = 0x0003,
    InteropTagParentGeneration = 0x0004,
    InteropTagRole = 0x0005,
    InteropTagRect = 0x0006,
    InteropTagTitle = 0x0007,
    InteropTagAppKey = 0x0008,
    InteropTagProcessId = 0x0009,
    InteropTagState = 0x000A,
    InteropTagDpi = 0x000B,
    InteropTagSlot = 0x000C,
    InteropTagFrameSequence = 0x000D,
    InteropTagDamage = 0x000E,
    InteropTagEdge = 0x000F,
    InteropTagSerial = 0x0010,
    InteropTagPoint = 0x0011,
    InteropTagButton = 0x0012,
    InteropTagPressed = 0x0013,
    InteropTagKey = 0x0014,
    InteropTagAxisV120 = 0x0015,
    InteropTagAxisH120 = 0x0016,
    InteropTagDelta = 0x0017,
    InteropTagTouchId = 0x0018,
    InteropTagPressure = 0x0019,
    InteropTagTilt = 0x001A,
    InteropTagRotation = 0x001B,
    InteropTagTool = 0x001C,
    InteropTagText = 0x001D,
    InteropTagCursorBegin = 0x001E,
    InteropTagCursorEnd = 0x001F,
    InteropTagLayout = 0x0020,
    InteropTagLocks = 0x0021,
    InteropTagCursorShape = 0x0022,
    InteropTagHotspot = 0x0023,
    InteropTagImageSize = 0x0024,
    InteropTagContentHash = 0x0025,
    InteropTagTransfer = 0x0026,
    InteropTagTotalBytes = 0x0027,
    InteropTagOffset = 0x0028,
    InteropTagData = 0x0029,
    InteropTagMime = 0x002A,
    InteropTagMimeList = 0x002B,
    InteropTagClipboardEpoch = 0x002C,
    InteropTagDropEffect = 0x002D,
    InteropTagDisplayName = 0x002E,
    InteropTagTargetPath = 0x002F,
    InteropTagArguments = 0x0030,
    InteropTagWorkingDir = 0x0031,
    InteropTagAumid = 0x0032,
    InteropTagAppFlags = 0x0033,
    InteropTagIconSizes = 0x0034,
    InteropTagIconSize = 0x0035,
    InteropTagLaunchKind = 0x0036,
    InteropTagActivationToken = 0x0037,
    InteropTagStatus = 0x0038,
    InteropTagMessage = 0x0039,
    InteropTagCapabilities = 0x003A,
    InteropTagAgentVersion = 0x003B,
    InteropTagLayoutVersion = 0x003C,
    InteropTagSessionEpoch = 0x003D,
    InteropTagOutput = 0x003E,
    InteropTagStream = 0x003F,
    InteropTagPrinterName = 0x0040,
    InteropTagPrinterUri = 0x0041,
    InteropTagPublisher = 0x0042,
    InteropTagDecision = 0x0043,
    InteropTagUsbId = 0x0044,
    InteropTagLeaseEpoch = 0x0045,
    InteropTagErrorCode = 0x0046,
    InteropTagRefusedType = 0x0047,
    InteropTagUriList = 0x0048,
    InteropTagIntervalMs = 0x0049,
    InteropTagRate = 0x004A,
    InteropTagMinSize = 0x004B /**< Optional width/height u32 pair, 0..16384; zero clears a minimum. */
} interop_tag_t;

/** @brief Guest window classification for host surface mapping. */
typedef enum interop_window_role_t {
    InteropRoleToplevel = 1,   /**< Ownerless or WS_EX_APPWINDOW: xdg_toplevel. */
    InteropRoleDialog = 2,     /**< Owned activatable window: xdg_toplevel + set_parent. */
    InteropRoleMenu = 3,       /**< Menus and dropdowns: xdg_popup with grab. */
    InteropRoleTooltip = 4,    /**< Tooltips/no-activate popups: xdg_popup without grab. */
    InteropRoleUacConsent = 5, /**< consent.exe dialog: xdg_toplevel, app_id waddle.<dev>.uac. */
    InteropRoleImePreedit = 6  /**< Agent-owned preedit window: xdg_popup without grab. */
} interop_window_role_t;

/** @brief Window state bitmask. */
typedef enum interop_window_state_t {
    InteropStateMinimized = 1u << 0,
    InteropStateMaximized = 1u << 1,
    InteropStateFullscreen = 1u << 2,
    InteropStateActivated = 1u << 3,   /**< Host to guest only. */
    InteropStateResizable = 1u << 4,   /**< Guest to host: WS_THICKFRAME. */
    InteropStateMaximizable = 1u << 5, /**< Guest to host: WS_MAXIMIZEBOX. */
    InteropStateMinimizable = 1u << 6  /**< Guest to host: WS_MINIMIZEBOX. */
} interop_window_state_t;

/** @brief Resize edges, numerically equal to xdg_toplevel_resize_edge. */
typedef enum interop_resize_edge_t {
    InteropEdgeTop = 1,
    InteropEdgeBottom = 2,
    InteropEdgeLeft = 4,
    InteropEdgeTopLeft = 5,
    InteropEdgeBottomLeft = 6,
    InteropEdgeRight = 8,
    InteropEdgeTopRight = 9,
    InteropEdgeBottomRight = 10
} interop_resize_edge_t;

/** @brief Pointer buttons (also tablet barrel buttons 1 and 2). */
typedef enum interop_button_t {
    InteropButtonLeft = 1,
    InteropButtonRight = 2,
    InteropButtonMiddle = 3,
    InteropButtonBack = 4,   /**< BTN_SIDE to XBUTTON1. */
    InteropButtonForward = 5 /**< BTN_EXTRA to XBUTTON2. */
} interop_button_t;

/** @brief Tablet tool kinds. */
typedef enum interop_tablet_tool_t {
    InteropToolPen = 1,
    InteropToolEraser = 2
} interop_tablet_tool_t;

/** @brief Launch request kinds. */
typedef enum interop_launch_kind_t {
    InteropLaunchApp = 1,       /**< Catalog app_key. */
    InteropLaunchPath = 2,      /**< ShellExecuteExW "open" on target_path. */
    InteropLaunchInstaller = 3, /**< msiexec.exe /i on target_path. */
    InteropLaunchShortcut = 4,  /**< ShellExecuteExW on a .lnk. */
    InteropLaunchConsole = 5    /**< Console image in a new guest console window. */
} interop_launch_kind_t;

/** @brief Catalog entry flags. */
typedef enum interop_app_flag_t {
    InteropAppSystem = 1u << 0,
    InteropAppSeed = 1u << 1,
    InteropAppDesktopShortcut = 1u << 2,
    InteropAppPackaged = 1u << 3,
    InteropAppUninstallable = 1u << 4
} interop_app_flag_t;

/** @brief Negotiated optional facilities (64-bit mask). */
typedef enum interop_capability_t {
    InteropCapWindows = 1ull << 0,
    InteropCapInput = 1ull << 1,
    InteropCapCursor = 1ull << 2,
    InteropCapRelativePointer = 1ull << 3,
    InteropCapTouch = 1ull << 4,
    InteropCapTablet = 1ull << 5,
    InteropCapGestures = 1ull << 6,
    InteropCapTextInput = 1ull << 7,
    InteropCapClipboard = 1ull << 8,
    InteropCapDragDrop = 1ull << 9,
    InteropCapCatalog = 1ull << 10,
    InteropCapLaunch = 1ull << 11,
    InteropCapAudio = 1ull << 12,
    InteropCapPrinters = 1ull << 13,
    InteropCapUacNative = 1ull << 14,
    InteropCapDisplay = 1ull << 15,
    InteropCapKeyboardLayout = 1ull << 16
} interop_capability_t;

/** @brief Result status codes. */
typedef enum interop_status_t {
    InteropOk = 0,
    InteropNotFound = 1,
    InteropAccessDenied = 2,
    InteropCancelled = 3,
    InteropTimedOut = 4,
    InteropBusy = 5,
    InteropTooLarge = 6,
    InteropUnsupported = 7,
    InteropOutsideShare = 8,
    InteropGuestError = 9
} interop_status_t;

/** @brief Error codes; also the nonzero return values of the codec. */
typedef enum interop_error_t {
    InteropErrorNone = 0,
    InteropErrorProtocolViolation = 1,
    InteropErrorMalformedFrame = 2,
    InteropErrorUnsupportedField = 3,
    InteropErrorUnsupportedType = 4,
    InteropErrorStaleWindow = 5,
    InteropErrorQuotaExceeded = 6,
    InteropErrorVersionMismatch = 7,
    InteropErrorUnexpected = 8
} interop_error_t;

/** @brief Cursor shapes: wp_cursor_shape_device_v1 values 1..34 plus these. */
typedef enum interop_cursor_shape_t {
    InteropCursorHidden = 0,
    InteropCursorDefault = 1,
    InteropCursorLast = 34,
    InteropCursorCustom = 0xFFFF
} interop_cursor_shape_t;

/** @brief Payload writer; the frame buffer stays caller-owned.
 * @note Fields are written after a reserved 16-byte header. After any failed
 * put the writer is poisoned (failed != 0) and finish fails. Not thread-safe.
 */
typedef struct interop_writer_t {
    uint8_t *frame;    /**< Borrowed frame buffer, NULL until init. */
    size_t capacity;   /**< Usable bytes in frame. */
    size_t length;     /**< Bytes written including the reserved header. */
    uint16_t last_tag; /**< Last base tag written, 0 before the first field. */
    uint16_t fields;   /**< Fields written. */
    int failed;        /**< Nonzero after any rejected operation. */
} interop_writer_t;

/** @brief Receiver-side transfer reassembly state, caller-owned storage.
 * @note hash_state holds an incremental SHA-256 context and is opaque.
 */
typedef struct interop_transfer_t {
    uint32_t id;           /**< Transfer id, 0 when idle. */
    uint32_t state;        /**< 0 idle, 1 receiving, 2 complete. */
    uint64_t total_bytes;  /**< Announced size. */
    uint64_t received;     /**< Contiguous bytes received. */
    _Alignas(8) uint8_t hash_state[128]; /**< Opaque SHA-256 context. */
} interop_transfer_t;

/** @brief Decode and validate a frame header.
 * @param[in] bytes Borrowed untrusted bytes, at least @p length long; may be NULL only if length is 0.
 * @param[in] length Available bytes (>= InteropHeaderBytes).
 * @param[out] header Caller-owned output, unchanged on failure.
 * @return InteropErrorNone, or ProtocolViolation (bad magic/version/length) /
 * MalformedFrame (short input). Pure and thread-safe.
 */
int interop_header_decode(const uint8_t *bytes, size_t length, interop_header_t *header);

/** @brief Validate a payload for a message type and direction.
 * @param[in] type Message type from a decoded header.
 * @param[in] direction Producer of the frame.
 * @param[in] payload Borrowed untrusted payload bytes, may be NULL when length is 0.
 * @param[in] length Payload bytes (header.length).
 * @return InteropErrorNone or the interop_error_t describing the first defect:
 * UnsupportedType (unknown type or wrong direction), UnsupportedField (unknown
 * critical tag), MalformedFrame (structure, order, padding, criticality,
 * value, or required-field defects). Pure and thread-safe.
 */
int interop_payload_validate(uint16_t type, interop_direction_t direction,
                             const uint8_t *payload, size_t length);

/** @brief Find the n-th occurrence of a field in a validated payload.
 * @param[in] payload Borrowed payload previously accepted by interop_payload_validate.
 * @param[in] length Payload bytes.
 * @param[in] tag Base tag (without the critical bit).
 * @param[in] index Zero-based occurrence (only Output may repeat).
 * @param[out] value Borrowed pointer into @p payload, valid while payload lives; unchanged when absent.
 * @param[out] value_length Value bytes; unchanged when absent.
 * @return 0 found, 1 absent, -1 malformed payload or NULL outputs.
 */
int interop_field_get(const uint8_t *payload, size_t length, uint16_t tag, uint32_t index,
                      const uint8_t **value, uint16_t *value_length);

/** @brief Read an unsigned field of 1, 2, 4 or 8 bytes, widened to 64 bits.
 * @param[out] value Unchanged when absent or on error.
 * @return 0 found, 1 absent, -1 malformed or size mismatch with @p width.
 */
int interop_field_uint(const uint8_t *payload, size_t length, uint16_t tag, uint32_t width,
                       uint64_t *value);

/** @brief Read a 4-byte signed field. @return 0 found, 1 absent, -1 error. */
int interop_field_i32(const uint8_t *payload, size_t length, uint16_t tag, int32_t *value);

/** @brief Start a frame in a caller-owned buffer.
 * @param[out] writer Caller-owned writer, fully reset.
 * @param[in] frame Borrowed output buffer retained by the writer until finish.
 * @param[in] capacity Buffer bytes; values above InteropMaxFrameBytes are clamped.
 * @note A capacity below InteropHeaderBytes or a NULL frame poisons the writer.
 */
void interop_writer_init(interop_writer_t *writer, uint8_t *frame, size_t capacity);

/** @brief Append a field with raw value bytes. The registry criticality bit is
 * applied automatically and the value is validated.
 * @return InteropErrorNone or MalformedFrame (unknown tag, order, size, value,
 * capacity, field count); the writer is poisoned on failure.
 */
int interop_put_bytes(interop_writer_t *writer, uint16_t tag, const void *value, uint16_t length);
/** @brief Append an unsigned field of the registry width (1, 2, 4 or 8 bytes). */
int interop_put_uint(interop_writer_t *writer, uint16_t tag, uint64_t value);
/** @brief Append a signed 32-bit field. */
int interop_put_i32(interop_writer_t *writer, uint16_t tag, int32_t value);
/** @brief Append a rect field. */
int interop_put_rect(interop_writer_t *writer, uint16_t tag, int32_t x, int32_t y,
                     uint32_t width, uint32_t height);
/** @brief Append a point field. */
int interop_put_point(interop_writer_t *writer, uint16_t tag, int32_t x, int32_t y);

/** @brief Write the header and validate the whole frame.
 * @param[in,out] writer Writer from interop_writer_init.
 * @param[in] type Message type.
 * @param[in] request_id Request id (0 for unsolicited frames).
 * @param[in] direction Producer, checked against the message table.
 * @param[out] frame_length Total frame bytes; unchanged on failure.
 * @return InteropErrorNone or the validation error; header bytes are written only on success.
 */
int interop_writer_finish(interop_writer_t *writer, uint16_t type, uint32_t request_id,
                          interop_direction_t direction, size_t *frame_length);

/** @brief Begin receiving a transfer.
 * @param[out] transfer Caller-owned state, reset on success only.
 * @param[in] id Nonzero transfer id. @param[in] total_bytes Announced size.
 * @param[in] cap Receiver limit for this transfer kind (<= InteropMaxTransferBytes).
 * @return InteropErrorNone, MalformedFrame (zero id) or QuotaExceeded (over cap).
 */
int interop_transfer_begin(interop_transfer_t *transfer, uint32_t id, uint64_t total_bytes,
                           uint64_t cap);
/** @brief Account one chunk; the caller copies @p data to its own storage.
 * @return InteropErrorNone or ProtocolViolation (not receiving, gap, overlap,
 * overflow, empty or oversize chunk). The state is unchanged on failure.
 */
int interop_transfer_chunk(interop_transfer_t *transfer, uint64_t offset, const uint8_t *data,
                           size_t length);
/** @brief Complete a transfer after all bytes arrived.
 * @param[in] hash InteropHashBytes SHA-256 digest from TransferEnd.
 * @return InteropErrorNone or ProtocolViolation (incomplete or digest mismatch).
 */
int interop_transfer_end(interop_transfer_t *transfer, const uint8_t *hash);
/** @brief Reset a transfer to idle; safe on any state. */
void interop_transfer_cancel(interop_transfer_t *transfer);
/** @brief SHA-256 of a buffer for senders.
 * @param[in] data Borrowed bytes, may be NULL when length is 0.
 * @param[out] hash InteropHashBytes output.
 */
void interop_sha256(const uint8_t *data, size_t length, uint8_t *hash);

/** @brief Make a bounded protocol-v2 application key from a guest AUMID.
 * @param[in] aumid Borrowed UTF-8 bytes, non-NULL for a valid call; no embedded
 * NUL. All input is consumed before output is written; overlapping storage is safe.
 * @param[in] length AUMID bytes, 1..InteropMaxStringBytes.
 * @param[out] key Non-NULL caller-owned buffer of at least capacity bytes;
 * output is not NUL-terminated and is unchanged on failure.
 * @param[in] capacity Output capacity; at most InteropMaxAppKeyBytes are written.
 * @param[out] key_length Non-NULL caller-owned length; unchanged on failure.
 * @return InteropErrorNone, MalformedFrame for invalid input, or QuotaExceeded
 * for insufficient capacity. No allocation or retained pointers. Thread-safe
 * for distinct outputs; key_length must not overlap either byte buffer.
 */
int interop_app_key_from_aumid(const uint8_t *aumid, size_t length,
                              uint8_t *key, size_t capacity, size_t *key_length);

#endif
