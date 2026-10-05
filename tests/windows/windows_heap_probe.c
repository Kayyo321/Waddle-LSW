/** @file windows_heap_probe.c @brief Negative controls for heap instrumentation. */
#define GuestHeapListener
#include "windows_heap.h"
#include <string.h>
/** @brief Intentionally damage or retain a test allocation to prove detection.
 * @param[in] argc Must equal two. @param[in] argv Borrowed nonnull CRT strings.
 * @return 2 invalid mode; 1 missed defect; detector terminates with 86.
 * @note Main thread only. Intentional leaks/corruption are isolated child processes.
 */
int main(int argc, char **argv) {
    if (argc != 2) { return 2; }
    heap_start();
    volatile unsigned char *bytes = malloc(16);
    if (bytes == NULL) { return 2; }
    if (strcmp(argv[1], "leak") == 0) {
        heap_check();
    } else if (strcmp(argv[1], "overrun") == 0) {
        bytes[16] = 0;
        heap_check();
    } else if (strcmp(argv[1], "freed-write") == 0) {
        free((void *)bytes);
        bytes[0] = 0;
        heap_check();
    } else { free((void *)bytes); return 2; }
    return 1;
}
