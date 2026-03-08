/* Heap profiler: byte-weighted Poisson sampling of PyObject allocations.
 * See Include/internal/pycore_heap_profile.h for API and env vars. */

#include <stdbool.h>
#include <stdlib.h>               /* getenv(), strtoul() */
#include <stdio.h>                /* fprintf(), fileno(), stderr */
#include <string.h>               /* memcpy(), snprintf() */
#include <math.h>                 /* log() for Poisson sampling */

#include "Python.h"
#include "pycore_heap_profile.h"
#include "pycore_obmalloc.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"       /* _PyThreadState_GET */
#include "pycore_traceback.h"     /* _Py_GetTracebackFrames, PyTracebackFrameInfo */

/* All heap profiler state in one place. */
static struct heap_profiler_state {
    struct heap_profile_entry *list_head;       /* live allocations */
    struct heap_profile_entry *accumulated_head; /* freed samples since last reset */
    Py_traceback_interning_table_t *interning_table;
    int print_enabled;
    int initialized;
    /* Byte-weighted Poisson sampling */
    uint64_t sample_interval_bytes;
    uint64_t allocated_bytes;
    uint64_t next_sample_target;
    uint64_t rand_state;
    uint64_t allocs_since_last;  /* running count, reset on each sample */
} heap_profiler = {0};

static void
heap_profile_global_remove(struct heap_profile_entry *ent)
{
    if (ent->global_prev != NULL) {
        ent->global_prev->global_next = ent->global_next;
    } else {
        heap_profiler.list_head = ent->global_next;
    }
    if (ent->global_next != NULL) {
        ent->global_next->global_prev = ent->global_prev;
    }
}

static Py_traceback_id_t
heap_profile_pseudoframe(const char *reason)
{
    PyTracebackFrameInfo pseudo = {0};
    snprintf(pseudo.filename, sizeof(pseudo.filename), "<heap_profile>");
    snprintf(pseudo.name, sizeof(pseudo.name), "%s", reason);
    pseudo.lineno = 0;
    return _Py_traceback_intern(&pseudo, 1, heap_profiler.interning_table);
}

static Py_traceback_id_t
heap_profile_collect_traceback(void)
{
    if (heap_profiler.interning_table == NULL) {
        return NULL;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL) {
        return heap_profile_pseudoframe("no thread state (C-only context?)");
    }
    PyTracebackFrameInfo frames[HEAP_PROFILE_TRACEBACK_MAX];
    int count = _Py_GetTracebackFrames(tstate, frames, HEAP_PROFILE_TRACEBACK_MAX);
    if (count > 0) {
        Py_traceback_id_t id = _Py_traceback_intern(frames, count,
                                                    heap_profiler.interning_table);
        if (id != NULL) {
            return id;
        }
        return heap_profile_pseudoframe("traceback interning failed");
    }
    return heap_profile_pseudoframe("no Python frames on stack");
}

static void
heap_profile_dump_traceback(struct heap_profile_entry *ent)
{
    if (ent->traceback_id != NULL && heap_profiler.interning_table != NULL) {
        _Py_traceback_dump_id(ent->traceback_id, heap_profiler.interning_table,
                              fileno(stderr));
    } else {
        fprintf(stderr, "  no Python traceback\n");
    }
}

static void
heap_profile_free_traceback(struct heap_profile_entry *ent)
{
    if (ent->traceback_id != NULL && heap_profiler.interning_table != NULL) {
        _Py_traceback_release(ent->traceback_id, heap_profiler.interning_table);
        ent->traceback_id = NULL;
    }
}

/* Draw next sample target from exponential distribution with given mean.
 * Inverse transform sampling: -mean * ln(U) for U uniform (0,1]. */
static uint64_t
heap_profile_next_target(uint64_t mean)
{
    /* xorshift64* (Marsaglia). Fast, minimal state, period 2^64-1.
     * Good enough for heap sampling: https://en.wikipedia.org/wiki/Xorshift */
    uint64_t x = heap_profiler.rand_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    heap_profiler.rand_state = x;
    x *= 0x2545F4914F6CDD1DULL;  /* xorshift64* output */
    /* U in (0, 1] to avoid ln(0) */
    double u = (double)(x % (1ULL << 24) + 1) / (double)(1ULL << 24);
    double t = -mean * log(u);
    return (uint64_t)t;
}

/* Returns true if we should sample this allocation. Updates state.
 * When sampling, sets *out_bytes_since_last and *out_allocs_since_last (for upscaling).
 */
static int
heap_profile_should_sample(size_t size, uint64_t *out_bytes_since_last,
                          uint64_t *out_allocs_since_last)
{
    if (heap_profiler.sample_interval_bytes == 0) {
        return 0;
    }
    if (heap_profiler.allocs_since_last < UINT64_MAX) {
        heap_profiler.allocs_since_last++;
    }
    heap_profiler.allocated_bytes += size;
    if (heap_profiler.allocated_bytes >= heap_profiler.next_sample_target) {
        if (out_bytes_since_last != NULL) {
            *out_bytes_since_last = heap_profiler.allocated_bytes;
        }
        if (out_allocs_since_last != NULL) {
            *out_allocs_since_last = heap_profiler.allocs_since_last;
        }
        heap_profiler.allocated_bytes = 0;
        heap_profiler.allocs_since_last = 0;
        heap_profiler.next_sample_target = heap_profile_next_target(
            heap_profiler.sample_interval_bytes);
        return 1;
    }
    return 0;
}

void
heap_profile_record_sample(size_t size, pymem_block *ptr,
                           struct heap_profile_entry **metadata_head)
{
    assert(heap_profiler.initialized && heap_profiler.sample_interval_bytes != 0);
    uint64_t bytes_since_last;
    uint64_t allocs_since_last;
    if (!heap_profile_should_sample(size, &bytes_since_last, &allocs_since_last)) {
        return;
    }

    struct heap_profile_entry *heap_sample = PyMem_RawMalloc(sizeof(*heap_sample));
    if (heap_sample == NULL) {
        return;
    }

    Py_traceback_id_t traceback_id = heap_profile_collect_traceback();

    *heap_sample = (struct heap_profile_entry){
        .ptr = ptr,
        .size = size,
        .bytes_since_last_sample = bytes_since_last,
        .allocs_since_last_sample = allocs_since_last,
        .traceback_id = traceback_id,
        .next = *metadata_head,
        .global_next = heap_profiler.list_head,
        .global_prev = NULL,
    };
    *metadata_head = heap_sample;
    if (heap_profiler.list_head) heap_profiler.list_head->global_prev = heap_sample;
    heap_profiler.list_head = heap_sample;


    struct heap_profile_entry *alloc_sample = PyMem_RawMalloc(sizeof(*alloc_sample));
    if (alloc_sample != NULL) {
        _Py_traceback_retain(traceback_id, heap_profiler.interning_table);

        *alloc_sample = (struct heap_profile_entry){
            .ptr = ptr,
            .size = size,
            .bytes_since_last_sample = bytes_since_last,
            .allocs_since_last_sample = allocs_since_last,
            .traceback_id = traceback_id,
            .next = NULL,
            .global_next = heap_profiler.accumulated_head,
            .global_prev = NULL,
        };
        if (heap_profiler.accumulated_head) heap_profiler.accumulated_head->global_prev = alloc_sample;
        heap_profiler.accumulated_head = alloc_sample;
    }
}

static void
heap_profile_print_entry(struct heap_profile_entry *ent, void *ptr)
{
    if (!heap_profiler.print_enabled) {
        return;
    }
    fprintf(stderr, "heap profile free: %p size=%zu weight_bytes=%llu weight_allocs=%llu\n",
            ptr, ent->size,
            (unsigned long long)ent->bytes_since_last_sample,
            (unsigned long long)ent->allocs_since_last_sample);
    heap_profile_dump_traceback(ent);
}

void
init_heap_profile_sampling(void)
{
    if (heap_profiler.initialized) {
        return;
    }
    heap_profiler.initialized = 1;
    heap_profiler.sample_interval_bytes = 0;  /* disabled by default */
    heap_profiler.rand_state = 0x853c49e6748fea9bULL;  /* xorshift64 seed */

    const char *env = getenv("PYTHON_HEAP_PROFILE_SAMPLE_BYTES");
    if (env != NULL && env[0] != '\0') {
        unsigned long val = strtoul(env, NULL, 10);
        if (val > 0 && val <= UINT64_MAX) {
            heap_profiler.sample_interval_bytes = (uint64_t)val;
        }
    }
    if (heap_profiler.sample_interval_bytes > 0) {
        heap_profiler.allocated_bytes = 0;
        heap_profiler.allocs_since_last = 0;
        heap_profiler.next_sample_target = heap_profile_next_target(
            heap_profiler.sample_interval_bytes);
    }
    env = getenv("PYTHON_HEAP_PROFILE_PRINT");
    heap_profiler.print_enabled = (env != NULL && env[0] != '\0');
    if (heap_profiler.sample_interval_bytes > 0
        && heap_profiler.interning_table == NULL) {
        Py_traceback_interning_allocator_t raw_alloc = {
            PyMem_RawMalloc,
            PyMem_RawFree,
        };
        heap_profiler.interning_table = _Py_traceback_interning_table_new(&raw_alloc);
    }
}

void *
heap_profile_alloc_large_block(size_t nbytes, bool zero_initialize)
{
    void *block = zero_initialize
        ? PyMem_RawCalloc(1, nbytes + HEAP_PROFILE_LARGE_PREFIX)
        : PyMem_RawMalloc(nbytes + HEAP_PROFILE_LARGE_PREFIX);
    if (block == NULL) {
        return NULL;
    }

    struct heap_profile_entry *metadata = NULL;
    heap_profile_record_sample(nbytes, NULL, &metadata);
    *(void **)block = metadata;
    return (char *)block + HEAP_PROFILE_LARGE_PREFIX;
}

void
heap_profile_free_large_block(void *p)
{
    void *block = (char *)p - HEAP_PROFILE_LARGE_PREFIX;
    void *metadata = *(void **)block;
    if (metadata != NULL) {
        struct heap_profile_entry *ent = metadata;
        heap_profile_global_remove(ent);
        heap_profile_print_entry(ent, p);
        heap_profile_free_traceback(ent);
        PyMem_RawFree(metadata);
    }
    PyMem_RawFree(block);
}

/* Realloc a large block. Semantics: free the old allocation (remove from profile,
 * print if enabled) then make a new allocation with a fresh sampling decision.
 * We realloc the underlying memory first; only if that succeeds do we free the
 * old metadata, so that on failure the caller's block remains valid and tracked. */
void *
heap_profile_realloc_large_block(void *p, size_t nbytes)
{
    void *block = (char *)p - HEAP_PROFILE_LARGE_PREFIX;
    block = PyMem_RawRealloc(block, nbytes + HEAP_PROFILE_LARGE_PREFIX);
    if (block == NULL) {
        return NULL;
    }
    /* Remove old sample from profile (if any). Read metadata from the current
     * block after realloc, since the block may have moved. */
    void *metadata = *(void **)block;
    if (metadata != NULL) {
        struct heap_profile_entry *ent = metadata;
        heap_profile_global_remove(ent);
        heap_profile_print_entry(ent, p);
        heap_profile_free_traceback(ent);
        PyMem_RawFree(metadata);
    }
    /* Fresh sampling decision for the new size. */
    struct heap_profile_entry *new_metadata = NULL;
    heap_profile_record_sample(nbytes, NULL, &new_metadata);
    *(void **)block = new_metadata;
    return (char *)block + HEAP_PROFILE_LARGE_PREFIX;
}

void
heap_profile_remove_and_print(poolp pool, pymem_block *p)
{
    struct heap_profile_entry **pnext = &pool->metadata;
    while (*pnext != NULL) {
        struct heap_profile_entry *ent = *pnext;
        if (ent->ptr == p) {
            *pnext = ent->next;
            heap_profile_global_remove(ent);
            heap_profile_print_entry(ent, (void *)p);
            heap_profile_free_traceback(ent);
            PyMem_RawFree(ent);
            return;
        }
        pnext = &ent->next;
    }
}

/* Iteration API. Export for shared extensions (e.g. _testinternalcapi). */
#if defined(__GNUC__) || (defined(__clang__) && __has_attribute(visibility))
#  define HEAP_PROFILE_EXPORT __attribute__((visibility("default")))
#else
#  define HEAP_PROFILE_EXPORT
#endif

HEAP_PROFILE_EXPORT int
heap_profile_is_enabled(void)
{
    init_heap_profile_sampling();
    return heap_profiler.sample_interval_bytes > 0;
}

HEAP_PROFILE_EXPORT struct heap_profile_entry *
heap_profile_get_first(void)
{
    if (!heap_profile_is_enabled()) {
        return NULL;
    }
    return heap_profiler.list_head;
}

HEAP_PROFILE_EXPORT struct heap_profile_entry *
heap_profile_get_next(struct heap_profile_entry *ent)
{
    if (ent == NULL) {
        return NULL;
    }
    return ent->global_next;
}

HEAP_PROFILE_EXPORT struct heap_profile_entry *
heap_profile_get_first_accumulated(void)
{
    if (!heap_profile_is_enabled()) {
        return NULL;
    }
    return heap_profiler.accumulated_head;
}

HEAP_PROFILE_EXPORT void
heap_profile_reset_accumulated(void)
{
    struct heap_profile_entry *ent = heap_profiler.accumulated_head;
    while (ent != NULL) {
        struct heap_profile_entry *next = ent->global_next;
        heap_profile_free_traceback(ent);
        PyMem_RawFree(ent);
        ent = next;
    }
    heap_profiler.accumulated_head = NULL;
}

HEAP_PROFILE_EXPORT Py_traceback_interning_table_t *
heap_profile_get_interning_table(void)
{
    if (!heap_profile_is_enabled()) {
        return NULL;
    }
    return heap_profiler.interning_table;
}
