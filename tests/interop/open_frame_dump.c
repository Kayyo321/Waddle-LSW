/** @file open_frame_dump.c @brief Encode LocalLaunch and reply frames with the Zig codec for the open e2e test.
 *
 * Usage:
 *   open_frame_dump launch RID KIND TARGET|- ARGS|- TOKEN|-   (LocalLaunch, from local)
 *   open_frame_dump result RID STATUS MESSAGE|-                (LaunchResult, from guest)
 *   open_frame_dump error RID CODE MESSAGE|-                   (Error, from guest)
 * Prints the complete frame as lowercase hex on stdout and exits 0; exits 2 on usage errors and
 * 1 when the codec rejects the frame. Test-only; links the production codec object.
 */
#include "waddle/interop_protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_absent(const char *text) { return strcmp(text, "-") == 0; }

static int print_frame(interop_writer_t *writer, uint16_t type, uint32_t request_id, interop_direction_t direction) {
    size_t length = 0;
    if (interop_writer_finish(writer, type, request_id, direction, &length) != 0) return 1;
    for (size_t index = 0; index < length; index++) printf("%02x", writer->frame[index]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    static uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    if (argc < 3) return 2;
    uint32_t request_id = (uint32_t)strtoul(argv[2], NULL, 10);
    interop_writer_init(&writer, frame, sizeof(frame));
    if (strcmp(argv[1], "launch") == 0 && argc == 7) {
        uint64_t kind = strtoul(argv[3], NULL, 10);
        uint16_t key_tag = kind == InteropLaunchApp ? InteropTagAppKey : InteropTagTargetPath;
        if (interop_put_bytes(&writer, key_tag, argv[4], (uint16_t)strlen(argv[4])) != 0) return 1;
        if (!is_absent(argv[5]) && interop_put_bytes(&writer, InteropTagArguments, argv[5], (uint16_t)strlen(argv[5])) != 0) return 1;
        if (interop_put_uint(&writer, InteropTagLaunchKind, kind) != 0) return 1;
        if (!is_absent(argv[6]) && interop_put_bytes(&writer, InteropTagActivationToken, argv[6], (uint16_t)strlen(argv[6])) != 0) return 1;
        return print_frame(&writer, InteropMsgLocalLaunch, request_id, InteropFromLocal);
    }
    if (strcmp(argv[1], "result") == 0 && argc == 5) {
        if (interop_put_uint(&writer, InteropTagProcessId, 4242) != 0) return 1;
        if (interop_put_uint(&writer, InteropTagStatus, strtoul(argv[3], NULL, 10)) != 0) return 1;
        if (!is_absent(argv[4]) && interop_put_bytes(&writer, InteropTagMessage, argv[4], (uint16_t)strlen(argv[4])) != 0) return 1;
        return print_frame(&writer, InteropMsgLaunchResult, request_id, InteropFromGuest);
    }
    if (strcmp(argv[1], "error") == 0 && argc == 5) {
        if (!is_absent(argv[4]) && interop_put_bytes(&writer, InteropTagMessage, argv[4], (uint16_t)strlen(argv[4])) != 0) return 1;
        if (interop_put_uint(&writer, InteropTagErrorCode, strtoul(argv[3], NULL, 10)) != 0) return 1;
        return print_frame(&writer, InteropMsgError, request_id, InteropFromGuest);
    }
    return 2;
}
