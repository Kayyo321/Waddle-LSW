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
/** @brief Exact generated site and global edge bounds; legacy ABI stays 32-wide. */
#ifdef WaddleCoverageMetadataHeader
enum { MaxBranches = WaddleCoverageSiteCount, MaxEdges = 255, MaxRecorderBytes = 524288 };
#else
enum { MaxBranches = WaddleCoverageSiteCount, MaxEdges = 32, MaxRecorderBytes = 524288 };
#endif
/** @brief Sparse location of additional edge words for one wide site. */
typedef struct coverage_wide_site_t {
    uint32_t site_index;
    uint32_t extra_word_offset;
} coverage_wide_site_t;
/** @brief One static owner; wide-site metadata exists only for wide arities. */
typedef struct coverage_storage_t {
    _Atomic uint32_t hits[MaxBranches];
#ifdef WaddleCoverageMetadataHeader
    const uint8_t arities[MaxBranches];
#if WaddleCoverageWideSiteCount > 0
    _Atomic uint32_t extra_hits[WaddleCoverageExtraMaskWordCount == 0 ? 1 : WaddleCoverageExtraMaskWordCount];
    const coverage_wide_site_t wide_sites[WaddleCoverageWideSiteCount];
#endif
#endif
} coverage_storage_t;
_Static_assert(MaxBranches > 0, "coverage metadata must contain sites");
_Static_assert(sizeof(coverage_storage_t) <= MaxRecorderBytes,
               "coverage recorder exceeds unchanged 512KiB storage budget");
static coverage_storage_t storage = {
#ifdef WaddleCoverageMetadataHeader
    .arities = WaddleCoverageArities,
#if WaddleCoverageWideSiteCount > 0
    .extra_hits = {0},
    .wide_sites = WaddleCoverageWideSites,
#endif
#else
    .hits = {0},
#endif
};
#ifdef WaddleCoverageMetadataHeader
/** @brief Validate all immutable metadata before tests; no allocation/retention. */
__attribute__((constructor)) static void validate_metadata(void) {
    for (uint32_t site = 0; site < MaxBranches; site++) {
        if (storage.arities[site] == 0) {
            fprintf(stderr, "Coverage recorder metadata: site=%u arity=%u\n",
                    site, storage.arities[site]);
            abort();
        }
    }
#if WaddleCoverageWideSiteCount > 0
    uint32_t wide_index = 0;
    uint32_t extra_word_offset = 0;
    for (uint32_t site = 0; site < MaxBranches; site++) {
        if (storage.arities[site] > 32) {
            if (wide_index >= WaddleCoverageWideSiteCount ||
                storage.wide_sites[wide_index].site_index != site ||
                storage.wide_sites[wide_index].extra_word_offset != extra_word_offset) {
                fprintf(stderr, "Coverage recorder wide metadata: site=%u entry=%u offset=%u\n",
                        site, wide_index, extra_word_offset);
                abort();
            }
            extra_word_offset += ((uint32_t)storage.arities[site] + 31) / 32 - 1;
            wide_index++;
        } else if (wide_index < WaddleCoverageWideSiteCount &&
                   storage.wide_sites[wide_index].site_index <= site) {
            fprintf(stderr, "Coverage recorder unexpected wide entry: site=%u entry=%u\n",
                    site, wide_index);
            abort();
        }
    }
    if (wide_index != WaddleCoverageWideSiteCount ||
        extra_word_offset != WaddleCoverageExtraMaskWordCount) {
        fprintf(stderr, "Coverage recorder wide metadata totals: entries=%u/%u words=%u/%u\n",
                wide_index, WaddleCoverageWideSiteCount, extra_word_offset,
                WaddleCoverageExtraMaskWordCount);
        abort();
    }
#else
    for (uint32_t site = 0; site < MaxBranches; site++) {
        if (storage.arities[site] > 32) {
            fprintf(stderr, "Coverage recorder wide metadata missing sparse entries: site=%u\n", site);
            abort();
        }
    }
    if (WaddleCoverageExtraMaskWordCount != 0) {
        fputs("Coverage recorder wide metadata missing sparse entries\n", stderr);
        abort();
    }
#endif
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
    if (edge >= 32) {
#if WaddleCoverageWideSiteCount > 0
        uint32_t low = 0;
        uint32_t high = WaddleCoverageWideSiteCount;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            uint32_t site = storage.wide_sites[middle].site_index;
            if (site < branch) low = middle + 1;
            else high = middle;
        }
        if (low >= WaddleCoverageWideSiteCount || storage.wide_sites[low].site_index != branch) {
            fprintf(stderr, "Coverage recorder missing wide site: branch=%u edge=%u\n", branch, edge);
            abort();
        }
        uint32_t extra_word = storage.wide_sites[low].extra_word_offset + edge / 32 - 1;
#if WaddleCoverageExtraMaskWordCount > 0
        if (extra_word >= WaddleCoverageExtraMaskWordCount) {
            fprintf(stderr, "Coverage recorder wide word bounds: branch=%u edge=%u word=%u/%u\n",
                    branch, edge, extra_word, WaddleCoverageExtraMaskWordCount);
            abort();
        }
#else
        (void)extra_word;
        fprintf(stderr, "Coverage recorder missing wide words: branch=%u edge=%u\n", branch, edge);
        abort();
#endif
        atomic_fetch_or_explicit(&storage.extra_hits[extra_word], UINT32_C(1) << (edge % 32),
                                 memory_order_relaxed);
        return;
#else
        fprintf(stderr, "Coverage recorder missing wide metadata: branch=%u edge=%u\n", branch, edge);
        abort();
#endif
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
#ifdef WaddleCoverageMetadataHeader
        uint32_t base_edges = storage.arities[branch] < 32 ? storage.arities[branch] : 32;
        for (uint32_t edge = 0; edge < base_edges; edge++)
            if (mask & (UINT32_C(1) << edge)) fprintf(output, "%u %u\n", branch, edge);
        if (storage.arities[branch] > 32) {
#if WaddleCoverageWideSiteCount > 0
            uint32_t low = 0;
            uint32_t high = WaddleCoverageWideSiteCount;
            while (low < high) {
                uint32_t middle = low + (high - low) / 2;
                uint32_t site = storage.wide_sites[middle].site_index;
                if (site < branch) low = middle + 1;
                else high = middle;
            }
            uint32_t extra_word_offset = storage.wide_sites[low].extra_word_offset;
            for (uint32_t edge = 32; edge < storage.arities[branch]; edge++) {
                uint32_t extra_word = extra_word_offset + edge / 32 - 1;
                uint32_t extra_mask = atomic_load_explicit(&storage.extra_hits[extra_word],
                                                           memory_order_relaxed);
                if (extra_mask & (UINT32_C(1) << (edge % 32)))
                    fprintf(output, "%u %u\n", branch, edge);
            }
#else
            fprintf(stderr, "Coverage recorder missing wide metadata: branch=%u\n", branch);
            abort();
#endif
        }
#else
        for (uint32_t edge = 0; edge < MaxEdges; edge++)
            if (mask & (UINT32_C(1) << edge)) fprintf(output, "%u %u\n", branch, edge);
#endif
    }
    if (fclose(output)) { perror(path); abort(); }
}
