# Historical AV compatibility fixture

These are Waddle's own production files from local commit
1585dac8a4b628febb2980daf3cb405e939c7943 (input-era old guest/host), not a third-party
dependency. Tests compile them separately; production never links them. Keeping
the source here lets shallow CI checkouts test real historical rejection behavior
without depending on rewritten publication commit hashes or network access.

Original source SHA-256:
- src/av/av_codec.zig: 055984d705138de53db003c135e5714b7e6615822b26a4f386c607d22897333e
- src/av/av_peer.c: 228a44ba0f79e75594e8668cd8ea1db2ddda529b58f7c45cc554d0eb65b70eba

The codec is its original production prefix before the first test. Unrelated
number/PCI parsers and unit tests are omitted. Only exported encode/decode names
and the header include path are changed. Its type bound, validation and decode
remain unchanged. The peer C file is exact historical source; its header include
path is local. The historical protocol header is unmodified. Symbol renaming in
legacy_fixture.c permits linking old and new peers in one socket fixture.

The fixture sends old Create sequence 0 and nonzero, plus zero Geometry/Destroy,
through fragmented sockets to new admission. It sends new CreateV2 through the
actual old pump/decoder, expects protocol failure then executes the old host's
normal socket close and verifies new guest EOF. It does not claim a pre-window
handshake, timeout or negotiated fallback.

Original Git blob identities and complete-source SHA-256:
- src/av/av_codec.zig: blob `0ec04935a6f4f7956e91811abb46d6bf40e77e68`, SHA-256 `055984d705138de53db003c135e5714b7e6615822b26a4f386c607d22897333e`.
- src/av/av_peer.c: blob `0941c74123d8abbb68c5ac0f47ce9e3616992d68`, SHA-256 `228a44ba0f79e75594e8668cd8ea1db2ddda529b58f7c45cc554d0eb65b70eba`.
- src/av/av_peer.h: blob `09be89d65d2d5f75eaefe3d879241004f49a79e1`, SHA-256 `e8c62ed4c65cd90b466e0c6e79cd5c843a5feeea0561b0df8a1ffef264429589`.
- include/waddle/av_protocol.h: blob `e0f8fa718af9b1fd264fbed477b4789c98fec683`, SHA-256 `f09815f682bdaa1a5406e2999f3fcd2136e5f5feb47073cbf273dada84c5cb5c`.
