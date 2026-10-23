/** @file device_branch_runtime.c Test-only source branch edge recorder. */
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef WaddleCoverageMetadataHeader
#include WaddleCoverageMetadataHeader
#else
/** @brief Legacy caller capacity; no generated metadata or ownership. */
#define WaddleCoverageSiteCount 16384u
/** @brief Legacy exported recorder ABI; concurrent callers are permitted. */
#define WaddleCoverageHitSymbol waddle_branch_hit
#endif
/** @brief Exact generated site bound, unchanged edge and total storage limits. */
enum { MaxBranches = WaddleCoverageSiteCount, MaxEdges = 32, MaxRecorderBytes = 524288 };
/** @brief One static owner; immutable arities follow atomic per-site edge masks. */
typedef struct coverage_storage_t {
    _Atomic uint32_t hits[MaxBranches];
#ifdef WaddleCoverageMetadataHeader
    const uint8_t arities[MaxBranches];
#endif
} coverage_storage_t;
_Static_assert(MaxBranches > 0, "coverage metadata must contain sites");
_Static_assert(sizeof(coverage_storage_t) <= MaxRecorderBytes,
               "coverage recorder exceeds unchanged 512KiB storage budget");
static coverage_storage_t storage = {
#ifdef WaddleCoverageMetadataHeader
    .arities = WaddleCoverageArities,
#else
    .hits = {0},
#endif
};
#ifdef WaddleCoverageMetadataHeader
/** @brief Validate all immutable metadata before tests; no allocation/retention. */
__attribute__((constructor)) static void validate_metadata(void) {
    for (uint32_t site = 0; site < MaxBranches; site++) {
        if (storage.arities[site] == 0 || storage.arities[site] > MaxEdges) {
            fprintf(stderr, "Coverage recorder metadata: site=%u arity=%u\n",
                    site, storage.arities[site]);
            abort();
        }
    }
    fprintf(stderr, "Coverage recorder storage: %zu/%u bytes; %u sites\n",
            sizeof(storage), MaxRecorderBytes, MaxBranches);
}
#endif
/** @brief Record an exact instrumented edge, atomically unioning concurrent hits.
 * @param[in] branch Exact metadata index. @param[in] edge Valid per-site outcome.
 * @note No pointer/ownership; invalid metadata/index/edge aborts before indexing.
 */
void WaddleCoverageHitSymbol(uint32_t branch, uint32_t edge) {
    if (branch >= MaxBranches || edge >= MaxEdges) {
        fprintf(stderr, "Coverage recorder bounds: branch=%u/%u edge=%u/%u\n",
                branch, MaxBranches, edge, MaxEdges);
        abort();
    }
#ifdef WaddleCoverageMetadataHeader
    if (edge >= storage.arities[branch]) {
        fprintf(stderr, "Coverage recorder arity: branch=%u edge=%u/%u\n",
                branch, edge, storage.arities[branch]);
        abort();
    }
#endif
    atomic_fetch_or_explicit(&storage.hits[branch], UINT32_C(1) << edge, memory_order_relaxed);
}
/** @brief Persist every observed edge at exit; owns/closes FILE, no heap memory.
 * @note Hit-producing threads must be joined/quiescent before process exit.
 * Test-only destructor; WADDLE_BRANCH_OUT is a private controlled directory.
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
    for (uint32_t branch = 0; branch < MaxBranches; branch++) {
        uint32_t mask = atomic_load_explicit(&storage.hits[branch], memory_order_relaxed);
        for (uint32_t edge = 0; edge < MaxEdges; edge++)
            if (mask & (UINT32_C(1) << edge)) fprintf(output, "%u %u\n", branch, edge);
    }
    if (fclose(output)) { perror(path); abort(); }
}
