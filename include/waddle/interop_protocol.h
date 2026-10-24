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
 * function is allocation-free. Readers, keys and transfer updates preserve
 * outputs on failure. Writers retain a borrowed frame buffer, set a sticky
 * failed flag on error, and publish a frame only after successful finish.
 * Callers own all buffers and state; distinct outputs are thread-safe. Shared
 * writer/transfer state requires external synchronization. Outputs and state
 * must not alias input or each other unless explicitly allowed below.
 */
#include <stddef.h>
#include <stdint.h>

/** @brief Guest VSOCK listen port for the current interop session protocol. */
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
    InteropMsgHello = 0x0001, /**< Msg Hello; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgHelloAck = 0x0002, /**< Msg Hello Ack; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgGoodbye = 0x0003, /**< Msg Goodbye; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgPing = 0x0004, /**< Msg Ping; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgPong = 0x0005, /**< Msg Pong; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgError = 0x0006, /**< Msg Error; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowAnnounce = 0x0100, /**< Msg Window Announce; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowUpdate = 0x0101, /**< Msg Window Update; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowRetire = 0x0102, /**< Msg Window Retire; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowConfigure = 0x0103, /**< Msg Window Configure; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowClose = 0x0104, /**< Msg Window Close; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowMoveRequest = 0x0105, /**< Msg Window Move Request; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowResizeRequest = 0x0106, /**< Msg Window Resize Request; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowStateRequest = 0x0107, /**< Msg Window State Request; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgFrameReady = 0x0108, /**< Msg Frame Ready; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgFrameRelease = 0x0109, /**< Msg Frame Release; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgWindowIconChanged = 0x010A, /**< Msg Window Icon Changed; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgPopupRepositioned = 0x010B, /**< Msg Popup Repositioned; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgPopupDismissed = 0x010C, /**< Msg Popup Dismissed; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputFocus = 0x0200, /**< Msg Input Focus; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputKey = 0x0201, /**< Msg Input Key; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputPointerMotion = 0x0202, /**< Msg Input Pointer Motion; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputPointerRelative = 0x0203, /**< Msg Input Pointer Relative; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputPointerButton = 0x0204, /**< Msg Input Pointer Button; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputPointerAxis = 0x0205, /**< Msg Input Pointer Axis; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputTouch = 0x0206, /**< Msg Input Touch; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputTouchFrame = 0x0207, /**< Msg Input Touch Frame; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputTouchCancel = 0x0208, /**< Msg Input Touch Cancel; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputTablet = 0x0209, /**< Msg Input Tablet; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputTabletProximity = 0x020A, /**< Msg Input Tablet Proximity; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputGesturePinch = 0x020B, /**< Msg Input Gesture Pinch; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputTextCommit = 0x020C, /**< Msg Input Text Commit; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputTextPreedit = 0x020D, /**< Msg Input Text Preedit; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputRelease = 0x020E, /**< Msg Input Release; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputLockState = 0x020F, /**< Msg Input Lock State; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgInputRepeatInfo = 0x0210, /**< Msg Input Repeat Info; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgKeyboardLayout = 0x0211, /**< Msg Keyboard Layout; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCaretRect = 0x0212, /**< Msg Caret Rect; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgLeaseRevoked = 0x0213, /**< Msg Lease Revoked; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgLeaseAck = 0x0214, /**< Msg Lease Ack; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgLeaseReady = 0x0215, /**< Msg Lease Ready; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCursorShape = 0x0300, /**< Msg Cursor Shape; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCursorConstraint = 0x0301, /**< Msg Cursor Constraint; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgClipboardOffer = 0x0400, /**< Msg Clipboard Offer; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgClipboardRequest = 0x0401, /**< Msg Clipboard Request; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgClipboardClear = 0x0402, /**< Msg Clipboard Clear; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndEnter = 0x0500, /**< Msg Dnd Enter; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndMotion = 0x0501, /**< Msg Dnd Motion; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndLeave = 0x0502, /**< Msg Dnd Leave; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndDrop = 0x0503, /**< Msg Dnd Drop; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndResult = 0x0504, /**< Msg Dnd Result; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndSourceStart = 0x0505, /**< Msg Dnd Source Start; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndSourceRequest = 0x0506, /**< Msg Dnd Source Request; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDndSourceFinished = 0x0507, /**< Msg Dnd Source Finished; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCatalogBegin = 0x0600, /**< Msg Catalog Begin; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCatalogAdd = 0x0601, /**< Msg Catalog Add; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCatalogUpdate = 0x0602, /**< Msg Catalog Update; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCatalogRemove = 0x0603, /**< Msg Catalog Remove; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgCatalogEnd = 0x0604, /**< Msg Catalog End; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgIconRequest = 0x0605, /**< Msg Icon Request; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgLaunchRequest = 0x0700, /**< Msg Launch Request; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgLaunchResult = 0x0701, /**< Msg Launch Result; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgElevationNotice = 0x0702, /**< Msg Elevation Notice; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgUacPrompt = 0x0703, /**< Msg Uac Prompt; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgUacDecision = 0x0704, /**< Msg Uac Decision; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDesktopSwitch = 0x0705, /**< Msg Desktop Switch; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgAudioStreamOpen = 0x0800, /**< Msg Audio Stream Open; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgAudioStreamClose = 0x0801, /**< Msg Audio Stream Close; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgPrinterSync = 0x0900, /**< Msg Printer Sync; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgPrinterResult = 0x0901, /**< Msg Printer Result; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDisplayConfig = 0x0A00, /**< Msg Display Config; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgDisplayApplied = 0x0A01, /**< Msg Display Applied; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgTransferBegin = 0x0F00, /**< Msg Transfer Begin; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgTransferChunk = 0x0F01, /**< Msg Transfer Chunk; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgTransferEnd = 0x0F02, /**< Msg Transfer End; see the enum contract above and IMPL_DESC section 3.2. */
    InteropMsgTransferCancel = 0x0F03, /**< Msg Transfer Cancel; see the enum contract above and IMPL_DESC section 3.2. */
    /** Local session.sock request from `waddle open`; LaunchRequest fields. The
     * reply on that socket is a LaunchResult validated as InteropFromGuest
     * because waddle-session relays the guest's result unchanged. */
    InteropMsgLocalLaunch = 0x7F00,
    InteropMsgLocalStop = 0x7F01, /**< Local session stop request; nonzero request id. */
    InteropMsgLocalStopResult = 0x7F02 /**< Local stop result; echoes request id. */
} interop_message_type_t;

/** @brief Field tags without the critical bit (IMPL_DESC 3.2.3). */
typedef enum interop_tag_t {
    InteropTagWindow = 0x0001, /**< window: u64; critical. HWND value */
    InteropTagGeneration = 0x0002, /**< generation: u64; critical. Per-agent-session monotonically increasing window incarnation, never 0 */
    InteropTagParent = 0x0003, /**< parent: u64; critical. Parent HWND, 0 = none */
    InteropTagParentGeneration = 0x0004, /**< parent_generation: u64; critical. */
    InteropTagRole = 0x0005, /**< role: u8; critical. interop_window_role_t */
    InteropTagRect = 0x0006, /**< rect: rect; critical. Window bounds in guest desktop pixels */
    InteropTagTitle = 0x0007, /**< title: str; optional. ≤ InteropMaxTitleBytes */
    InteropTagAppKey = 0x0008, /**< app_key: str; critical. Section 3.8.2 */
    InteropTagProcessId = 0x0009, /**< process_id: u32; optional. */
    InteropTagState = 0x000A, /**< state: u32; critical. interop_window_state_t bitmask */
    InteropTagDpi = 0x000B, /**< dpi: u32; critical. 96..960 */
    InteropTagSlot = 0x000C, /**< slot: u32; critical. IVSHMEM slot index */
    InteropTagFrameSequence = 0x000D, /**< frame_sequence: u64; critical. */
    InteropTagDamage = 0x000E, /**< damage: bytes; optional. Array of up to 16 rects; empty = full */
    InteropTagEdge = 0x000F, /**< edge: u8; critical. interop_resize_edge_t */
    InteropTagSerial = 0x0010, /**< serial: u32; critical. Host protocol input counter or its guest echo; never a raw Wayland seat serial */
    InteropTagPoint = 0x0011, /**< point: point; critical. Surface-local guest pixels */
    InteropTagButton = 0x0012, /**< button: u16; critical. interop_button_t */
    InteropTagPressed = 0x0013, /**< pressed: bool; critical. */
    InteropTagKey = 0x0014, /**< key: u16; critical. Linux evdev code 1..767 */
    InteropTagAxisV120 = 0x0015, /**< axis_v120: i32; critical. Vertical, value120 units, ±15360 */
    InteropTagAxisH120 = 0x0016, /**< axis_h120: i32; critical. Horizontal, value120 units, ±15360 */
    InteropTagDelta = 0x0017, /**< delta: bytes; critical. i32 dx_fixed, i32 dy_fixed (24.8 fixed) */
    InteropTagTouchId = 0x0018, /**< touch_id: u32; critical. 0..9 */
    InteropTagPressure = 0x0019, /**< pressure: u16; critical. 0..1024 */
    InteropTagTilt = 0x001A, /**< tilt: bytes; critical. i16 tilt_x, i16 tilt_y degrees ×100, ±9000 */
    InteropTagRotation = 0x001B, /**< rotation: u16; optional. degrees ×100, 0..35999 */
    InteropTagTool = 0x001C, /**< tool: u8; critical. interop_tablet_tool_t */
    InteropTagText = 0x001D, /**< text: str; critical. Commit/preedit text ≤ 4096 */
    InteropTagCursorBegin = 0x001E, /**< cursor_begin: i32; optional. Preedit cursor byte offset, -1 hidden */
    InteropTagCursorEnd = 0x001F, /**< cursor_end: i32; optional. */
    InteropTagLayout = 0x0020, /**< layout: str; critical. Windows KLID, 8 hex digits */
    InteropTagLocks = 0x0021, /**< locks: u8; critical. bit0 caps, bit1 num, bit2 scroll */
    InteropTagCursorShape = 0x0022, /**< cursor_shape: u16; critical. interop_cursor_shape_t */
    InteropTagHotspot = 0x0023, /**< hotspot: point; critical. Cursor hotspot */
    InteropTagImageSize = 0x0024, /**< image_size: bytes; critical. u32 width, u32 height ≤ 256×256 for cursors, ≤ 512×512 icons */
    InteropTagContentHash = 0x0025, /**< content_hash: bytes; critical. 32-byte SHA-256 */
    InteropTagTransfer = 0x0026, /**< transfer: u32; critical. Transfer id */
    InteropTagTotalBytes = 0x0027, /**< total_bytes: u64; critical. */
    InteropTagOffset = 0x0028, /**< offset: u64; critical. */
    InteropTagData = 0x0029, /**< data: bytes; critical. Chunk payload ≤ InteropChunkBytes */
    InteropTagMime = 0x002A, /**< mime: str; critical. MIME type, ≤ 255 bytes, [!-~] only */
    InteropTagMimeList = 0x002B, /**< mime_list: bytes; critical. Concatenated u8 len + mime entries, ≤ 32 entries */
    InteropTagClipboardEpoch = 0x002C, /**< clipboard_epoch: u64; critical. Selection generation */
    InteropTagDropEffect = 0x002D, /**< drop_effect: u8; critical. 1 copy, 2 move, 4 link (bitmask for offers) */
    InteropTagDisplayName = 0x002E, /**< display_name: str; critical. App display name ≤ 255 */
    InteropTagTargetPath = 0x002F, /**< target_path: str; optional. Guest path of the target executable */
    InteropTagArguments = 0x0030, /**< arguments: str; optional. Windows command-line tail */
    InteropTagWorkingDir = 0x0031, /**< working_dir: str; optional. Guest path */
    InteropTagAumid = 0x0032, /**< aumid: str; optional. AppUserModelID */
    InteropTagAppFlags = 0x0033, /**< app_flags: u32; critical. interop_app_flag_t */
    InteropTagIconSizes = 0x0034, /**< icon_sizes: bytes; optional. Array of u16 available sizes */
    InteropTagIconSize = 0x0035, /**< icon_size: u16; critical. */
    InteropTagLaunchKind = 0x0036, /**< launch_kind: u8; critical. interop_launch_kind_t */
    InteropTagActivationToken = 0x0037, /**< activation_token: str; optional. XDG activation token ≤ 255, opaque to guest */
    InteropTagStatus = 0x0038, /**< status: u32; critical. interop_status_t */
    InteropTagMessage = 0x0039, /**< message: str; optional. Human-readable diagnostic ≤ 1024 */
    InteropTagCapabilities = 0x003A, /**< capabilities: u64; critical. interop_capability_t bitmask */
    InteropTagAgentVersion = 0x003B, /**< agent_version: str; critical. Semantic version of the sender */
    InteropTagLayoutVersion = 0x003C, /**< layout_version: u32; critical. IVSHMEM layout version */
    InteropTagSessionEpoch = 0x003D, /**< session_epoch: u64; critical. Fresh nonzero epoch per authorized mapping initialization; transport accept never authorizes reuse */
    InteropTagOutput = 0x003E, /**< output: bytes; critical. u32 output_id, u32 width_px, u32 height_px, u32 scale_120 (scale ×120, 120..480) */
    InteropTagStream = 0x003F, /**< stream: u32; critical. Audio stream / ring index */
    InteropTagPrinterName = 0x0040, /**< printer_name: str; critical. CUPS queue name ≤ 127 */
    InteropTagPrinterUri = 0x0041, /**< printer_uri: str; critical. ≤ 255 */
    InteropTagPublisher = 0x0042, /**< publisher: str; optional. UAC verified publisher ≤ 255 */
    InteropTagDecision = 0x0043, /**< decision: u8; critical. 0 deny, 1 allow */
    InteropTagUsbId = 0x0044, /**< usb_id: bytes; critical. u16 vendor, u16 product, u8 bus, u8 address */
    InteropTagLeaseEpoch = 0x0045, /**< lease_epoch: u64; critical. Input lease epoch (Section 3.6.2) */
    InteropTagErrorCode = 0x0046, /**< error_code: u32; critical. interop_error_t */
    InteropTagRefusedType = 0x0047, /**< refused_type: u16; optional. Message type that caused an Error */
    InteropTagUriList = 0x0048, /**< uri_list: str; critical. text/uri-list body (host paths) or CRLF list of guest paths */
    InteropTagIntervalMs = 0x0049, /**< interval_ms: u32; critical. */
    InteropTagRate = 0x004A, /**< rate: u32; critical. Key repeat rate per second, 0 = no repeat */
    InteropTagMinSize = 0x004B /**< min_size: bytes; optional. u32 width, u32 height, each 0..16384 guest physical pixels; zero clears that minimum */
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
    InteropStateMinimized = 1u << 0, /**< State Minimized; see the enum contract above and IMPL_DESC section 3.2. */
    InteropStateMaximized = 1u << 1, /**< State Maximized; see the enum contract above and IMPL_DESC section 3.2. */
    InteropStateFullscreen = 1u << 2, /**< State Fullscreen; see the enum contract above and IMPL_DESC section 3.2. */
    InteropStateActivated = 1u << 3,   /**< Host to guest only. */
    InteropStateResizable = 1u << 4,   /**< Guest to host: WS_THICKFRAME. */
    InteropStateMaximizable = 1u << 5, /**< Guest to host: WS_MAXIMIZEBOX. */
    InteropStateMinimizable = 1u << 6  /**< Guest to host: WS_MINIMIZEBOX. */
} interop_window_state_t;

/** @brief Resize edges, numerically equal to xdg_toplevel_resize_edge. */
typedef enum interop_resize_edge_t {
    InteropEdgeTop = 1, /**< Edge Top; see the enum contract above and IMPL_DESC section 3.2. */
    InteropEdgeBottom = 2, /**< Edge Bottom; see the enum contract above and IMPL_DESC section 3.2. */
    InteropEdgeLeft = 4, /**< Edge Left; see the enum contract above and IMPL_DESC section 3.2. */
    InteropEdgeTopLeft = 5, /**< Edge Top Left; see the enum contract above and IMPL_DESC section 3.2. */
    InteropEdgeBottomLeft = 6, /**< Edge Bottom Left; see the enum contract above and IMPL_DESC section 3.2. */
    InteropEdgeRight = 8, /**< Edge Right; see the enum contract above and IMPL_DESC section 3.2. */
    InteropEdgeTopRight = 9, /**< Edge Top Right; see the enum contract above and IMPL_DESC section 3.2. */
    InteropEdgeBottomRight = 10 /**< Edge Bottom Right; see the enum contract above and IMPL_DESC section 3.2. */
} interop_resize_edge_t;

/** @brief Pointer buttons (also tablet barrel buttons 1 and 2). */
typedef enum interop_button_t {
    InteropButtonLeft = 1, /**< Button Left; see the enum contract above and IMPL_DESC section 3.2. */
    InteropButtonRight = 2, /**< Button Right; see the enum contract above and IMPL_DESC section 3.2. */
    InteropButtonMiddle = 3, /**< Button Middle; see the enum contract above and IMPL_DESC section 3.2. */
    InteropButtonBack = 4,   /**< BTN_SIDE to XBUTTON1. */
    InteropButtonForward = 5 /**< BTN_EXTRA to XBUTTON2. */
} interop_button_t;

/** @brief Tablet tool kinds. */
typedef enum interop_tablet_tool_t {
    InteropToolPen = 1, /**< Tool Pen; see the enum contract above and IMPL_DESC section 3.2. */
    InteropToolEraser = 2 /**< Tool Eraser; see the enum contract above and IMPL_DESC section 3.2. */
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
    InteropAppSystem = 1u << 0, /**< App System; see the enum contract above and IMPL_DESC section 3.2. */
    InteropAppSeed = 1u << 1, /**< App Seed; see the enum contract above and IMPL_DESC section 3.2. */
    InteropAppDesktopShortcut = 1u << 2, /**< App Desktop Shortcut; see the enum contract above and IMPL_DESC section 3.2. */
    InteropAppPackaged = 1u << 3, /**< App Packaged; see the enum contract above and IMPL_DESC section 3.2. */
    InteropAppUninstallable = 1u << 4 /**< App Uninstallable; see the enum contract above and IMPL_DESC section 3.2. */
} interop_app_flag_t;

/** @brief Negotiated optional facilities (64-bit mask). */
typedef enum interop_capability_t {
    InteropCapWindows = 1ull << 0, /**< Cap Windows; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapInput = 1ull << 1, /**< Cap Input; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapCursor = 1ull << 2, /**< Cap Cursor; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapRelativePointer = 1ull << 3, /**< Cap Relative Pointer; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapTouch = 1ull << 4, /**< Cap Touch; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapTablet = 1ull << 5, /**< Cap Tablet; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapGestures = 1ull << 6, /**< Cap Gestures; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapTextInput = 1ull << 7, /**< Cap Text Input; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapClipboard = 1ull << 8, /**< Cap Clipboard; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapDragDrop = 1ull << 9, /**< Cap Drag Drop; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapCatalog = 1ull << 10, /**< Cap Catalog; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapLaunch = 1ull << 11, /**< Cap Launch; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapAudio = 1ull << 12, /**< Cap Audio; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapPrinters = 1ull << 13, /**< Cap Printers; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapUacNative = 1ull << 14, /**< Cap Uac Native; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapDisplay = 1ull << 15, /**< Cap Display; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCapKeyboardLayout = 1ull << 16 /**< Cap Keyboard Layout; see the enum contract above and IMPL_DESC section 3.2. */
} interop_capability_t;

/** @brief Result status codes. */
typedef enum interop_status_t {
    InteropOk = 0, /**< Ok; see the enum contract above and IMPL_DESC section 3.2. */
    InteropNotFound = 1, /**< Not Found; see the enum contract above and IMPL_DESC section 3.2. */
    InteropAccessDenied = 2, /**< Access Denied; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCancelled = 3, /**< Cancelled; see the enum contract above and IMPL_DESC section 3.2. */
    InteropTimedOut = 4, /**< Timed Out; see the enum contract above and IMPL_DESC section 3.2. */
    InteropBusy = 5, /**< Busy; see the enum contract above and IMPL_DESC section 3.2. */
    InteropTooLarge = 6, /**< Too Large; see the enum contract above and IMPL_DESC section 3.2. */
    InteropUnsupported = 7, /**< Unsupported; see the enum contract above and IMPL_DESC section 3.2. */
    InteropOutsideShare = 8, /**< Outside Share; see the enum contract above and IMPL_DESC section 3.2. */
    InteropGuestError = 9 /**< Guest Error; see the enum contract above and IMPL_DESC section 3.2. */
} interop_status_t;

/** @brief Error codes; also the nonzero return values of the codec. */
typedef enum interop_error_t {
    InteropErrorNone = 0, /**< Error None; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorProtocolViolation = 1, /**< Error Protocol Violation; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorMalformedFrame = 2, /**< Error Malformed Frame; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorUnsupportedField = 3, /**< Error Unsupported Field; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorUnsupportedType = 4, /**< Error Unsupported Type; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorStaleWindow = 5, /**< Error Stale Window; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorQuotaExceeded = 6, /**< Error Quota Exceeded; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorVersionMismatch = 7, /**< Error Version Mismatch; see the enum contract above and IMPL_DESC section 3.2. */
    InteropErrorUnexpected = 8 /**< Error Unexpected; see the enum contract above and IMPL_DESC section 3.2. */
} interop_error_t;

/** @brief Cursor shapes: wp_cursor_shape_device_v1 values 1..34 plus these. */
typedef enum interop_cursor_shape_t {
    InteropCursorHidden = 0, /**< Cursor Hidden; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCursorDefault = 1, /**< Cursor Default; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCursorLast = 34, /**< Cursor Last; see the enum contract above and IMPL_DESC section 3.2. */
    InteropCursorCustom = 0xFFFF /**< Cursor Custom; see the enum contract above and IMPL_DESC section 3.2. */
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

/** @brief Decode a protocol-v2 header into host byte order.
 * @param[in] bytes Borrowed untrusted bytes, readable for length bytes; NULL is rejected.
 * @param[in] length Available bytes, at least InteropHeaderBytes; following payload is not
 * consumed.
 * @param[out] header Non-NULL caller-owned output; unchanged on every error.
 * @return InteropErrorNone; MalformedFrame for short/NULL input; ProtocolViolation for bad
 * magic, non-v2 version, oversize or unaligned payload length.
 * @note No allocation or retained pointers. Thread-safe for distinct outputs.
 */
int interop_header_decode(const uint8_t *bytes, size_t length, interop_header_t *header);

/** @brief Validate a complete payload for a message type and producer.
 * @param[in] type Message type from a successfully decoded header.
 * @param[in] direction VSOCK producer or InteropFromLocal for either session.sock peer.
 * @param[in] payload Borrowed untrusted bytes; may be NULL when length is zero.
 * @param[in] length Complete payload size from the header; must be a multiple of four.
 * @return InteropErrorNone; UnsupportedType for unknown/wrong-direction messages;
 * UnsupportedField for unknown critical tags; MalformedFrame for structure, required-field,
 * ordering, size, value or cross-field defects.
 * @note No allocation or retained pointers. Thread-safe for distinct outputs. Does not enforce
 * runtime capabilities, request IDs, leases, or connection state.
 */
int interop_payload_validate(uint16_t type, interop_direction_t direction,
                             const uint8_t *payload, size_t length);

/** @brief Borrow the indexed occurrence of a field in a validated payload.
 * @param[in] payload Borrowed previously validated bytes; may be NULL only when length is zero.
 * @param[in] length Payload bytes, at most InteropMaxPayloadBytes.
 * @param[in] tag Base field tag without the critical bit.
 * @param[in] index Zero-based occurrence; only Output may repeat.
 * @param[out] value Non-NULL caller-owned pointer slot; result borrows payload until that
 * payload is modified/freed. Unchanged on error/absence.
 * @param[out] value_length Non-NULL caller-owned size slot; unchanged on error/absence.
 * @return 0 found, 1 absent, -1 for malformed bounds/padding, oversize payload or NULL output
 * slots.
 * @note No allocation or retained pointers. Thread-safe for distinct outputs.
 */
int interop_field_get(const uint8_t *payload, size_t length, uint16_t tag, uint32_t index,
                      const uint8_t **value, uint16_t *value_length);

/** @brief Read a validated unsigned field, widened to uint64_t.
 * @param[in] payload Borrowed previously validated bytes; may be NULL only when length is zero.
 * @param[in] length Payload bytes, at most InteropMaxPayloadBytes.
 * @param[in] tag Base field tag without the critical bit.
 * @param[in] width Expected width in bytes: 1, 2, 4 or 8.
 * @param[out] value Non-NULL caller-owned scalar, unchanged on error/absence.
 * @return 0 found, 1 absent, -1 for malformed access, invalid width or width mismatch.
 * @note No allocation or retained pointers. Thread-safe for distinct outputs.
 */
int interop_field_uint(const uint8_t *payload, size_t length, uint16_t tag, uint32_t width,
                       uint64_t *value);

/** @brief Read a validated signed 32-bit little-endian field.
 * @param[in] payload Borrowed previously validated bytes; may be NULL only when length is zero.
 * @param[in] length Payload bytes, at most InteropMaxPayloadBytes.
 * @param[in] tag Base field tag without the critical bit.
 * @param[out] value Non-NULL caller-owned scalar, unchanged on error/absence.
 * @return 0 found, 1 absent, -1 for malformed access or a field not four bytes long.
 * @note No allocation or retained pointers. Thread-safe for distinct outputs.
 */
int interop_field_i32(const uint8_t *payload, size_t length, uint16_t tag, int32_t *value);

/** @brief Reset a writer to borrow caller-owned frame storage.
 * @param[out] writer Non-NULL caller-owned state, fully reset; may not overlap frame storage.
 * @param[in,out] frame Caller-owned writable bytes retained by writer until its reset/disposal;
 * NULL creates a failed writer. No frame bytes are changed by init.
 * @param[in] capacity Readable/writable frame capacity, clamped to InteropMaxFrameBytes.
 * @return No value. NULL frame or capacity below InteropHeaderBytes sets the failed flag.
 * @note No allocation. The caller owns state/storage; serialize calls sharing the writer or its
 * frame.
 */
void interop_writer_init(interop_writer_t *writer, uint8_t *frame, size_t capacity);

/** @brief Append a validated raw field; add its registry critical bit.
 * @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains borrowed
 * until reset or state disposal.
 * @param[in] tag Known base tag, in ascending order; Output repetition is validated by finish.
 * @param[in] value Borrowed input for length bytes; NULL allowed only when length is zero. Must
 * not overlap frame storage.
 * @param[in] length Input bytes, within that tag registry entry limits.
 * @return InteropErrorNone or MalformedFrame for a failed writer, unknown tag, bad
 * order/value/size, field count or capacity. Failure preserves prior frame bytes and poisons the
 * writer.
 * @note No allocation. The caller owns state/storage; serialize calls sharing the writer or its
 * frame.
 */
int interop_put_bytes(interop_writer_t *writer, uint16_t tag, const void *value, uint16_t length);
/** @brief Append an unsigned field of its registry width.
 * @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains borrowed
 * until reset or state disposal.
 * @param[in] tag Known base tag with a fixed 1/2/4/8-byte integer encoding.
 * @param[in] value Integer to encode; narrowing overflow is rejected.
 * @return InteropErrorNone or MalformedFrame for registry, width, range, writer or capacity
 * errors; failure poisons the writer.
 * @note No allocation. The caller owns state/storage; serialize calls sharing the writer or its
 * frame.
 */
int interop_put_uint(interop_writer_t *writer, uint16_t tag, uint64_t value);
/** @brief Append a signed 32-bit little-endian field.
 * @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains borrowed
 * until reset or state disposal.
 * @param[in] tag Known base tag accepting exactly four bytes.
 * @param[in] value Signed value, also checked against tag-specific limits.
 * @return InteropErrorNone or MalformedFrame; failure poisons the writer.
 * @note No allocation. The caller owns state/storage; serialize calls sharing the writer or its
 * frame.
 */
int interop_put_i32(interop_writer_t *writer, uint16_t tag, int32_t value);
/** @brief Append a 16-byte rect field.
 * @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains borrowed
 * until reset or state disposal.
 * @param[in] tag Known base tag accepting the rect encoding.
 * @param[in] x Signed guest-pixel x origin.
 * @param[in] y Signed guest-pixel y origin.
 * @param[in] width Physical pixel width, 0..16384.
 * @param[in] height Physical pixel height, 0..16384.
 * @return InteropErrorNone or MalformedFrame; failure poisons the writer.
 * @note No allocation. The caller owns state/storage; serialize calls sharing the writer or its
 * frame.
 */
int interop_put_rect(interop_writer_t *writer, uint16_t tag, int32_t x, int32_t y,
                     uint32_t width, uint32_t height);
/** @brief Append an eight-byte signed point field.
 * @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains borrowed
 * until reset or state disposal.
 * @param[in] tag Known base tag accepting an x/y pair.
 * @param[in] x Signed coordinate subject to tag-specific bounds.
 * @param[in] y Signed coordinate subject to tag-specific bounds.
 * @return InteropErrorNone or MalformedFrame; failure poisons the writer.
 * @note No allocation. The caller owns state/storage; serialize calls sharing the writer or its
 * frame.
 */
int interop_put_point(interop_writer_t *writer, uint16_t tag, int32_t x, int32_t y);

/** @brief Validate payload and publish its protocol-v2 header.
 * @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains borrowed
 * until reset or state disposal.
 * @param[in] type Message type for required/allowed field validation.
 * @param[in] request_id Request correlation; zero for unsolicited messages. Runtime enforces
 * outstanding-ID and local stop rules.
 * @param[in] direction Producer checked against the message table.
 * @param[out] frame_length Non-NULL caller-owned total byte count; unchanged on failure.
 * @return InteropErrorNone or payload validation error. Header bytes are written only on
 * success; failure poisons the writer. The caller must not send a frame from a failed writer.
 * @note No allocation. The caller owns state/storage; serialize calls sharing the writer or its
 * frame.
 */
int interop_writer_finish(interop_writer_t *writer, uint16_t type, uint32_t request_id,
                          interop_direction_t direction, size_t *frame_length);

/** @brief Initialize one receiver transfer accounting state.
 * @param[out] transfer Non-NULL caller-owned state, reset on success only; begin replaces
 * previous state. Caller handles table admission/cancel first.
 * @param[in] id Nonzero transfer ID.
 * @param[in] total_bytes Announced payload bytes, including zero for empty data.
 * @param[in] cap Per-kind receiver limit; the global InteropMaxTransferBytes limit applies too.
 * @return InteropErrorNone, MalformedFrame for zero ID, QuotaExceeded for size over either cap;
 * state unchanged on error.
 * @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
 * transfer. Does not allocate or store payload bytes.
 */
int interop_transfer_begin(interop_transfer_t *transfer, uint32_t id, uint64_t total_bytes,
                           uint64_t cap);
/** @brief Account and hash one contiguous borrowed chunk.
 * @param[in,out] transfer Non-NULL caller-owned receiving state created by begin; unchanged on
 * error.
 * @param[in] offset Must equal the number of bytes already received.
 * @param[in] data Borrowed chunk bytes, non-NULL on success; NULL is rejected.
 * @param[in] length Chunk bytes, 1..InteropChunkBytes, fitting the remaining total.
 * @return InteropErrorNone or ProtocolViolation for wrong state, gap/overlap, overflow,
 * empty/oversize chunk or NULL data.
 * @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
 * transfer. Caller separately copies/streams accepted bytes to owned payload storage.
 */
int interop_transfer_chunk(interop_transfer_t *transfer, uint64_t offset, const uint8_t *data,
                           size_t length);
/** @brief Verify the final digest and mark a transfer complete.
 * @param[in,out] transfer Non-NULL caller-owned receiving state; unchanged on error.
 * @param[in] hash Borrowed InteropHashBytes-byte SHA-256 digest; NULL is rejected.
 * @return InteropErrorNone or ProtocolViolation for NULL hash, wrong state, incomplete bytes or
 * digest mismatch.
 * @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
 * transfer.
 */
int interop_transfer_end(interop_transfer_t *transfer, const uint8_t *hash);
/** @brief Reset a caller-owned transfer to idle.
 * @param[out] transfer Non-NULL caller-owned state; all bytes, including opaque hash context,
 * are zeroed. Valid in any initialized state.
 * @return No value and no error. Does not free payload storage, which belongs to the caller.
 * @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
 * transfer.
 */
void interop_transfer_cancel(interop_transfer_t *transfer);
/** @brief Hash borrowed bytes for a sender.
 * @param[in] data Borrowed readable bytes; NULL allowed only when length is zero.
 * @param[in] length Readable input byte count.
 * @param[out] hash Non-NULL caller-owned output of InteropHashBytes bytes; must not overlap input.
 * @return No value or recoverable error; invalid pointer/length arguments violate the precondition.
 * @note No allocation or retained pointers. Thread-safe for distinct outputs.
 */
void interop_sha256(const uint8_t *data, size_t length, uint8_t *hash);

/** @brief Build a bounded protocol-v2 application key from UTF-8.
 * @param[in] aumid Borrowed UTF-8 AUMID without NUL. NULL is rejected. Entire input is consumed
 * before output; overlapping byte buffers are safe.
 * @param[in] length Input bytes, 1..InteropMaxStringBytes; no normalization or case folding.
 * @param[out] key Non-NULL caller-owned writable buffer of capacity bytes; not NUL-terminated.
 * Unchanged on failure.
 * @param[in] capacity Output capacity; at most InteropMaxAppKeyBytes are written.
 * @param[out] key_length Non-NULL caller-owned length, separate from both byte buffers;
 * unchanged on failure.
 * @return InteropErrorNone, MalformedFrame for invalid input, or QuotaExceeded for insufficient
 * output space.
 * @note No allocation or retained pointers. Thread-safe for distinct outputs. Hash suffix is
 * eight lowercase hex digits of SHA-256 over original UTF-8.
 */
int interop_app_key_from_aumid(const uint8_t *aumid, size_t length,
                              uint8_t *key, size_t capacity, size_t *key_length);

#endif
