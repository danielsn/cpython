#ifndef Py_INTERNAL_HEAP_PROFILE_H
#define Py_INTERNAL_HEAP_PROFILE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stdio.h>

#include "pycore_obmalloc.h"
#include "pycore_traceback.h"

/* Heap profile: byte-weighted Poisson sampling via linked list in pool->metadata.
 * PYTHON_HEAP_PROFILE_SAMPLE_BYTES=N (~1 sample per N bytes).
 *
 * bytes_since_last_sample, allocs_since_last_sample: weights for statistical upscaling.
 * Live entries are in a global doubly-linked list (global_next/global_prev).
 * A copy of each sample is added to the allocation list at sample time.
 * Allocation tracebacks are interned via _Py_traceback_intern (dedup by string/frame/traceback).
 */
#define HEAP_PROFILE_TRACEBACK_MAX 8

/* Large allocation: always reserve one extra pointer before the object.
 * Prefix is NULL if unused, or a pointer to heap_profile_entry. */
#define HEAP_PROFILE_LARGE_PREFIX sizeof(void *)

struct heap_profile_entry {
    pymem_block *ptr;
    size_t size;
    /* Statistical upscaling: this sample represents these since last sample. */
    uint64_t bytes_since_last_sample;   /* weight in bytes */
    uint64_t allocs_since_last_sample;  /* weight in allocation count */
    Py_traceback_id_t traceback_id;  /* interned; NULL if none */
    struct heap_profile_entry *next;       /* per-pool list (pool->metadata) */
    struct heap_profile_entry *global_next;
    struct heap_profile_entry *global_prev;
};

/* API for obmalloc.c */

/* Initialize heap profile sampling from env vars. Idempotent. */
void init_heap_profile_sampling(void);

/* Record a sampled allocation. Inserts into the list at *metadata_head.
 * ptr: block pointer for pool allocs, NULL for large allocs. */
void heap_profile_record_sample(size_t size, pymem_block *ptr,
                                struct heap_profile_entry **metadata_head);

/* Allocate a large block with profiling metadata. Only when profiling is enabled.
 * zero_initialize: true for calloc semantics, false for malloc.
 * Returns the user pointer, or NULL on failure. */
void *heap_profile_alloc_large_block(size_t nbytes, bool zero_initialize);

/* Free a large block (handles prefix and metadata). */
void heap_profile_free_large_block(void *p);

/* Realloc a large block: equivalent to free + new alloc with fresh sampling. */
void *heap_profile_realloc_large_block(void *p, size_t nbytes);

/* Remove profiled block from pool's list and release. */
void heap_profile_remove_from_pool(poolp pool, pymem_block *p);

/* Iteration API for C extensions. Export for _testinternalcapi shared extension. */

/* Return 1 if heap profiling is enabled and sampling, 0 otherwise. */
PyAPI_FUNC(int) heap_profile_is_enabled(void);

/* Get first/next entry in the global list. Returns NULL at end or when disabled.
 * Safe to call; entries may be removed by other threads during iteration. */
PyAPI_FUNC(struct heap_profile_entry *) heap_profile_get_first(void);
PyAPI_FUNC(struct heap_profile_entry *) heap_profile_get_next(struct heap_profile_entry *ent);

/* Allocation list: copy of every sample since last reset (added at sample time).
 * heap_profile_get_first_accumulated/get_next iterate over them.
 * heap_profile_reset_accumulated() clears the list and frees entries. */
PyAPI_FUNC(struct heap_profile_entry *) heap_profile_get_first_accumulated(void);
PyAPI_FUNC(void) heap_profile_reset_accumulated(void);

/* Get the interning table (for resolving traceback_id via _Py_traceback_fill_frames).
 * Returns NULL if profiling disabled or table not created. */
PyAPI_FUNC(Py_traceback_interning_table_t *) heap_profile_get_interning_table(void);

/* Export heap profile to pprof format. Returns 0 on success, -1 on error. */
PyAPI_FUNC(int) heap_profile_export_pprof(FILE *fp);

/* Export heap profile to OTel format. Returns 0 on success, -1 on error. Stub: returns -1. */
PyAPI_FUNC(int) heap_profile_export_otel(FILE *fp);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_HEAP_PROFILE_H */
