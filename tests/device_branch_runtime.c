/** @file device_branch_runtime.c Test-only source branch edge recorder. */
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
/** @brief Fixed bound for instrumented codec branches; no allocation ownership. */
enum { MaxBranches = 16384, MaxEdges = 32 };
static _Atomic unsigned char hits[MaxBranches][MaxEdges];
/** @brief Record an instrumented source conditional edge in a concurrent test.
 * @param[in] branch Bounded metadata index. @param[in] edge Bounded branch outcome.
 * @note No pointer/ownership; invalid instrumentation aborts instead of truncating.
 */
void waddle_branch_hit(uint32_t branch, uint32_t edge) {
    if (branch >= MaxBranches || edge >= MaxEdges) {
        fprintf(stderr, "Coverage recorder bounds: branch=%u/%u edge=%u/%u\n",
                branch, MaxBranches, edge, MaxEdges);
        abort();
    }
    atomic_store_explicit(&hits[branch][edge], 1, memory_order_relaxed);
}
/** @brief Persist hit indices at test exit; owns/closes output FILE, no heap memory.
 * @note Test-only destructor; WADDLE_BRANCH_OUT is a private controlled output dir.
 */
__attribute__((destructor)) static void emit_edges(void) {
    const char *root = getenv("WADDLE_BRANCH_OUT");
    if (!root) return;
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%ld.edges", root, (long)getpid());
    if (written < 0 || written >= (int)sizeof(path)) {
        fputs("Coverage recorder output path exceeds its bounded storage\n", stderr);
        abort();
    }
    FILE *output = fopen(path, "w");
    if (!output) { perror(path); abort(); }
    for (uint32_t branch = 0; branch < MaxBranches; branch++)
        for (uint32_t edge = 0; edge < MaxEdges; edge++)
            if (atomic_load_explicit(&hits[branch][edge], memory_order_relaxed)) fprintf(output, "%u %u\n", branch, edge);
    if (fclose(output)) { perror(path); abort(); }
}
