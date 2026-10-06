/** @file gpu_queue.c @brief Real Venus queue fixture C entry point. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
/** @brief Run bounded Zig fixture with symmetric receiver ownership.
 * @param[in] hardware Nonzero rejects CPU devices; no buffer ownership.
 * @return Zero success, one failure; sole session thread, no retained storage.
 */
extern int venus_gpu_fixture_run(int hardware);
int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--require-hardware")))
        return 2;
    alarm(30); /* Whole fixture guard, including any blocking SDK teardown. */
    int status = venus_gpu_fixture_run(argc == 2);
    if (!status)
        puts("Real Venus GPU timestamp workload and fence ordering passed");
    return status;
}
