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
 * PYTHON_HEAP_PROFILE_SAMPLE_BYTES=N (~1 sample per N bytes), PYTHON_HEAP_PROFILE_PRINT=1.
 * PYTHON_HEAP_PROFILE_DEBUG=1: print native C stacks when no Python traceback.
 *
 * bytes_since_last_sample, allocs_since_last_sample: weights for statistical upscaling.
 * Live entries are in a global doubly-linked list (global_next/global_prev).
 * A copy of each sample is added to the allocation list at sample time.
 * Allocation tracebacks are interned via _Py_traceback_intern (dedup by string/frame/traceback).
 */
#define HEAP_PROFILE_TRACEBACK_MAX 8
#define HEAP_PROFILE_NATIVE_BT_MAX 12

/* Large allocation: always reserve one extra pointer before the object.
 * Prefix is NULL if unused, or a pointer to heap_profile_entry. */
#define HEAP_PROFILE_LARGE_PREFIX sizeof(void *)

/* Reason for missing traceback (when traceback_id is NULL). Helps debugging. */
enum heap_profile_traceback_reason {
    HEAP_PROFILE_TB_OK = 0,           /* have traceback */
    HEAP_PROFILE_TB_NO_TABLE,         /* interning table not created */
    HEAP_PROFILE_TB_NO_TSTATE,        /* no PyThreadState (e.g. C-only context) */
    HEAP_PROFILE_TB_NO_FRAMES,        /* no Python frames on stack */
    HEAP_PROFILE_TB_INTERN_FAILED,    /* _Py_traceback_intern returned NULL */
};

struct heap_profile_entry {
    pymem_block *ptr;
    size_t size;
    /* Statistical upscaling: this sample represents these since last sample. */
    uint64_t bytes_since_last_sample;   /* weight in bytes */
    uint64_t allocs_since_last_sample;  /* weight in allocation count */
    Py_traceback_id_t traceback_id;  /* interned; NULL if none */
    unsigned char traceback_reason;   /* enum above; meaningful when traceback_id==NULL */
    /* Native C backtrace when no Python frames (traceback_id==NULL). */
    void *native_bt[HEAP_PROFILE_NATIVE_BT_MAX];
    int native_bt_count;              /* 0 = no native backtrace captured */
    struct heap_profile_entry *next;       /* per-pool list (pool->metadata) */
    struct heap_profile_entry *global_next;
    struct heap_profile_entry *global_prev;
};

/* API for obmalloc.c */

/* Initialize heap profile sampling from env vars. Idempotent. */
void init_heap_profile_sampling(void);

/* Record a sampled allocation. Returns new entry (caller links it) or NULL.
 * ptr: block pointer for pool allocs, NULL for large allocs. */
struct heap_profile_entry *heap_profile_record_sample(size_t size, pymem_block *ptr);

/* Allocate a large block with profiling metadata. Only when profiling is enabled.
 * zero_initialize: true for calloc semantics, false for malloc.
 * Returns the user pointer, or NULL on failure. */
void *heap_profile_alloc_large_block(size_t nbytes, bool zero_initialize);

/* Free a large block (handles prefix and metadata). */
void heap_profile_free_large_block(void *p);

/* Remove profiled block from pool's list and optionally print. */
void heap_profile_remove_and_print(poolp pool, pymem_block *p);

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
