/** @file device_branch_runtime.c Test-only source branch edge recorder. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
/** @brief Fixed bound for instrumented codec branches; no allocation ownership. */
enum { MaxBranches = 4096 };
static unsigned char hits[MaxBranches][32];
/** @brief Record an instrumented source conditional edge in a single-thread test.
 * @param[in] branch Bounded metadata index. @param[in] edge Bounded branch outcome.
 * @note No pointer/ownership; invalid instrumentation aborts instead of truncating.
 */
void waddle_branch_hit(uint32_t branch, uint32_t edge) {
    if (branch >= MaxBranches || edge >= 32) abort();
    hits[branch][edge] = 1;
}
/** @brief Persist hit indices at test exit; owns/closes output FILE, no heap memory.
 * @note Test-only destructor; WADDLE_BRANCH_OUT is a private controlled output dir.
 */
__attribute__((destructor)) static void emit_edges(void) {
    const char *root = getenv("WADDLE_BRANCH_OUT");
    if (!root) return;
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%ld.edges", root, (long)getpid()) >= (int)sizeof(path)) abort();
    FILE *output = fopen(path, "w");
    if (!output) abort();
    for (uint32_t branch = 0; branch < MaxBranches; branch++)
        for (uint32_t edge = 0; edge < 32; edge++)
            if (hits[branch][edge]) fprintf(output, "%u %u\n", branch, edge);
    if (fclose(output)) abort();
}
