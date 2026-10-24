# Frozen VSOCK message facility policy map

Source basis: `impl/seamless-interop/IMPL_DESC.md` §3.2.4–3.2.7 (capability names, message direction/schema, negotiated-facility rule); `include/waddle/interop_protocol.h` (`interop_message_type_t`, `interop_capability_t`, `InteropCapabilityMask`). The frozen text defines the 17 capability bits and says unnegotiated-facility frames are `UnsupportedType`, but it does not normatively map message types to capability bits. This table is therefore a proposed dispatch policy derived from message-family ownership. Parent/lead must resolve the explicitly marked ambiguities before a peer core treats it as frozen behavior.

A row with `A + B` means both bits are required in the committed negotiated intersection. `—` means connection-control traffic has no feature bit. Transfer frames inherit the capability of the operation owning their `transfer` ID; the frame alone does not identify that operation.

| VSOCK type(s), direction from §3.2.5 | Candidate required capability | Basis / notes |
|---|---|---|
| `Hello` G→H, `HelloAck` H→G | — (handshake phase only) | Negotiate capabilities. Do not reject Hello merely because an advertised optional bit is absent from the host offer; Ack carries the intersection. |
| `Goodbye` both, `Ping` H→G, `Pong` G→H, `Error` both | — | Common connection control and fatal/error reporting. Still phase/direction/request-ID constrained by endpoint policy. |
| `WindowAnnounce`, `WindowUpdate`, `WindowRetire` G→H; `WindowConfigure`, `WindowClose` H→G; `WindowMoveRequest`, `WindowResizeRequest`, `WindowStateRequest` G→H; `FrameReady` G→H, `FrameRelease` H→G; `WindowIconChanged` G→H; `PopupRepositioned`, `PopupDismissed` H→G | `Windows` | Window/surface lifecycle and frames. `WindowIconChanged` is treated as part of exported-window lifecycle, not the app catalog. Whether `Windows` is a prerequisite or a redundant platform marker is not explicit. |
| `InputFocus`, `InputKey`, `InputPointerMotion`, `InputPointerButton`, `InputPointerAxis`, `InputTouchFrame`, `InputTouchCancel`, `InputRelease`, `InputLockState`, `InputRepeatInfo` H→G; `CaretRect`, `LeaseRevoked`, `LeaseReady` G→H; `LeaseAck` H→G | `Input` | Base input/lease/focus facility. `InputLockState` and repeat settings are input state even without a per-window field. |
| `InputPointerRelative` H→G | `Input` + `RelativePointer` | Specialized relative motion. |
| `InputTouch` H→G | `Input` + `Touch` | Touch-specific event. |
| `InputTablet`, `InputTabletProximity` H→G | `Input` + `Tablet` | Tablet-specific event. |
| `InputGesturePinch` H→G | `Input` + `Gestures` | Gesture-derived Ctrl+wheel. |
| `InputTextCommit`, `InputTextPreedit` H→G | `Input` + `TextInput` | Text-input facility. |
| `KeyboardLayout` H→G | `KeyboardLayout` | Capability matches the message; whether it also requires base `Input` is unresolved. |
| `CursorShape` G→H | `Cursor` | Shape/bitmap-hash selection. A custom cursor's `Transfer*` frames inherit Cursor. |
| `CursorConstraint` G→H | `Cursor` for release/unconstrain; `Cursor` + `RelativePointer` when `pressed=true` | The message's true value explicitly means lock plus relative mode. Capability check therefore depends on the decoded value; whether the whole type should instead require both bits is unresolved. |
| `ClipboardOffer`, `ClipboardRequest`, `ClipboardClear` both | `Clipboard` | Clipboard ownership and rendering. Transfer frames started by a clipboard request inherit Clipboard. |
| `DndEnter`, `DndMotion`, `DndLeave`, `DndDrop` H→G; `DndResult`, `DndSourceStart` G→H; `DndSourceRequest`, `DndSourceFinished` H→G | `DragDrop` | Drag/drop protocol. Lazy payload `Transfer*` frames inherit DragDrop even though clipboard-style conversions may be reused. Whether DnD requires Clipboard as well is unresolved; no explicit dependency says so. |
| `CatalogBegin`, `CatalogAdd`, `CatalogUpdate`, `CatalogRemove`, `CatalogEnd` G→H; `IconRequest` H→G | `Catalog` | Catalog and catalog-icon retrieval. Optional per-window identity on IconRequest does not change the primary facility. |
| `LaunchRequest` H→G, `LaunchResult` G→H | `Launch` | Launch operation and result. |
| `ElevationNotice`, `UacPrompt`, `DesktopSwitch` G→H; `UacDecision` H→G | `UacNative` | These messages describe native elevation consent/secure-desktop support. Whether `ElevationNotice` and `DesktopSwitch` should be available when native prompting is disabled is unresolved; capability name suggests native UAC, while the text also defines these as `auto`/safety-net behaviors. |
| `AudioStreamOpen`, `AudioStreamClose` G→H | `Audio` | Audio stream/ring lifecycle. |
| `PrinterSync` H→G, `PrinterResult` G→H | `Printers` | Printer synchronization and result. |
| `DisplayConfig` H→G, `DisplayApplied` G→H | `Display` | Output configuration and apply result. |
| `TransferBegin`, `TransferChunk`, `TransferEnd`, `TransferCancel` both | Owning operation's capability, not a standalone bit | `transfer` ID is only unique per direction and does not encode Clipboard/DragDrop/Catalog/Cursor/UAC purpose. Dispatch must resolve the active transfer record before capability authorization; unknown transfer remains a transfer-state/protocol error, not inferred authorization. Explicit context needed before accepting payload. |
| `LocalLaunch`, `LocalStop`, `LocalStopResult` | Not VSOCK; local socket only | Do not admit these on VSOCK regardless of negotiated capabilities. |

## Request/response ID ownership candidates

The frozen generic header rule (§3.2.1) says responses echo the request ID; unsolicited frames use zero. The endpoint must use one allocator per sending direction/connection, reserve until response or cancellation is conclusively handled, and correlate message type/payload/connection separately (§8.17). The message table does not define request-ID policy for every semantic pair, so below distinguishes explicit pair semantics from candidates needing a normative decision.

| Request / response candidate | Evidence and ownership note |
|---|---|
| `Hello` → `HelloAck` | Explicit endpoint rule in §8.14: nonzero guest Hello ID, Ack echoes it exactly. Host's HelloAck is a reply, not a host reservation. |
| `Ping` → `Pong` | Explicit §8.15 behavior: host reserves/records nonzero Ping ID and guest echoes it; timeout/disconnect retires the outstanding ID. |
| `LaunchRequest` → `LaunchResult` | §3.9.1 says `--wait` waits for LaunchResult; likely request/reply ID pair under the generic header rule. Exact ID requirements and cancellation on caller timeout are not stated. |
| `PrinterSync` → `PrinterResult` | §3.17 says sync “Results → PrinterResult”; request/reply correlation is likely, but request-ID requirement is not explicit. Printer queue names can have concurrent updates, so ID vs queue-name correlation must be chosen. |
| `DisplayConfig` → `DisplayApplied` | §3.19 says guest replies DisplayApplied. Likely request/reply ID pair; whether coalesced configs share/replace outstanding IDs is unspecified. |
| `DndDrop` → `DndResult` | §3.2.5/schema and §3.10.3 establish operation/result, but transfer ID is separate and header ID echo behavior is unspecified. Do not assume the transfer ID replaces request_id. |
| `LocalStop` → `LocalStopResult` | Explicit §3.12.5 local-only request: nonzero request_id and result echoes it. Must use a local-socket direction allocator; never VSOCK. |
| `ClipboardRequest`, `IconRequest`, `DndDrop`, `DndSourceRequest`, `UacPrompt` → transfer sequence / decision | §3.2.6 explicitly allocates a distinct transfer ID (UacPrompt uses it as prompt ID). Transfer IDs are not header request IDs. Whether the first TransferBegin/other response also echoes a request_id is not specified; transfer correlation can use the transfer ID and operation context. |

## Unresolved decisions for the composed peer core

1. Freeze the exact message→capability map: especially whether `Windows` is required on all window messages; whether specialized input bits require `Input`; and whether capability dependencies are conjunctions or negotiated dependency closure.
2. Specify conditional checks: `CursorConstraint(pressed=true)` needs relative mode; decide its exact required bits. Decide whether KeyboardLayout additionally needs Input.
3. Specify UAC capability semantics for `ElevationNotice` and `DesktopSwitch` in `auto` / safety-net configurations.
4. Decide whether DnD is independent of Clipboard and formally authorize contextual capability inheritance for generic `Transfer*` frames (including unknown/cancelled transfer behavior).
5. Define request_id allocation/echo/zero rules and lifecycle for LaunchResult, PrinterResult, DisplayApplied, DndResult, and every TransferBegin/Cancel path. Keep explicit transfer-ID and input-serial namespaces distinct.
