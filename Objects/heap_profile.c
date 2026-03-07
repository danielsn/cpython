/* Heap profiler: byte-weighted Poisson sampling of PyObject allocations.
 * See Include/internal/pycore_heap_profile.h for API and env vars. */

#include <stdbool.h>
#include <stdlib.h>               /* getenv(), strtoul() */
#include <stdio.h>                /* fprintf(), fileno(), stderr */
#include <string.h>               /* memcpy() */
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
    int print_debug;   /* PYTHON_HEAP_PROFILE_DEBUG: native stacks, etc. */
    int initialized;
    /* Byte-weighted Poisson sampling */
    uint64_t sample_interval_bytes;
    uint64_t allocated_bytes;
    uint64_t next_sample_target;
    uint64_t rand_state;
    uint64_t allocs_since_last;  /* running count, reset on each sample */
} heap_profiler = {0};

static void
heap_profile_global_insert(struct heap_profile_entry *ent)
{
    ent->global_next = heap_profiler.list_head;
    ent->global_prev = NULL;
    if (heap_profiler.list_head != NULL) {
        heap_profiler.list_head->global_prev = ent;
    }
    heap_profiler.list_head = ent;
}

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

static void
heap_profile_accumulated_insert(struct heap_profile_entry *ent)
{
    ent->global_next = heap_profiler.accumulated_head;
    ent->global_prev = NULL;
    if (heap_profiler.accumulated_head != NULL) {
        heap_profiler.accumulated_head->global_prev = ent;
    }
    heap_profiler.accumulated_head = ent;
}

static void
heap_profile_collect_traceback(struct heap_profile_entry *ent)
{
    ent->traceback_id = NULL;
    ent->traceback_reason = HEAP_PROFILE_TB_NO_TABLE;
    ent->native_bt_count = 0;
    if (heap_profiler.interning_table == NULL) {
        return;
    }
    ent->traceback_reason = HEAP_PROFILE_TB_NO_TSTATE;
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL) {
        ent->native_bt_count = _Py_GetBacktrace(ent->native_bt,
                                                HEAP_PROFILE_NATIVE_BT_MAX);
        return;
    }
    PyTracebackFrameInfo frames[HEAP_PROFILE_TRACEBACK_MAX];
    int count = _Py_GetTracebackFrames(tstate, frames, HEAP_PROFILE_TRACEBACK_MAX);
    ent->traceback_reason = HEAP_PROFILE_TB_NO_FRAMES;
    if (count > 0) {
        ent->traceback_id = _Py_traceback_intern(frames, count,
                                                 heap_profiler.interning_table);
        if (ent->traceback_id != NULL) {
            ent->traceback_reason = HEAP_PROFILE_TB_OK;
        } else {
            ent->traceback_reason = HEAP_PROFILE_TB_INTERN_FAILED;
        }
    } else {
        /* No Python frames - capture native C backtrace for debugging */
        ent->native_bt_count = _Py_GetBacktrace(ent->native_bt,
                                                 HEAP_PROFILE_NATIVE_BT_MAX);
    }
}

static const char *heap_profile_traceback_reason_str[] = {
    [HEAP_PROFILE_TB_OK] = "ok",
    [HEAP_PROFILE_TB_NO_TABLE] = "no interning table",
    [HEAP_PROFILE_TB_NO_TSTATE] = "no thread state (C-only context?)",
    [HEAP_PROFILE_TB_NO_FRAMES] = "no Python frames on stack",
    [HEAP_PROFILE_TB_INTERN_FAILED] = "traceback interning failed",
};

static void
heap_profile_dump_traceback(struct heap_profile_entry *ent)
{
    if (ent->traceback_id != NULL && heap_profiler.interning_table != NULL) {
        _Py_traceback_dump_id(ent->traceback_id, heap_profiler.interning_table,
                              fileno(stderr));
    } else {
        unsigned char r = ent->traceback_reason;
        const char *reason = (r <= HEAP_PROFILE_TB_INTERN_FAILED)
            ? heap_profile_traceback_reason_str[r] : "unknown";
        fprintf(stderr, "  no Python traceback (%s)\n", reason);
        if (heap_profiler.print_debug && ent->native_bt_count > 0) {
            _Py_DumpBacktraceFromArray(fileno(stderr), ent->native_bt,
                                        ent->native_bt_count);
        }
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
 * When sampling, sets *out_bytes_since_last to bytes accumulated (for upscaling).
 */
static int
heap_profile_should_sample(size_t size, uint64_t *out_bytes_since_last)
{
    if (heap_profiler.sample_interval_bytes == 0) {
        return 0;
    }
    heap_profiler.allocated_bytes += size;
    if (heap_profiler.allocated_bytes >= heap_profiler.next_sample_target) {
        if (out_bytes_since_last != NULL) {
            *out_bytes_since_last = heap_profiler.allocated_bytes;
        }
        heap_profiler.allocated_bytes = 0;
        heap_profiler.next_sample_target = heap_profile_next_target(
            heap_profiler.sample_interval_bytes);
        return 1;
    }
    return 0;
}

struct heap_profile_entry *
heap_profile_record_sample(size_t size, pymem_block *ptr)
{
    if (heap_profiler.sample_interval_bytes == 0) {
        return NULL;
    }
    heap_profiler.allocs_since_last++;
    uint64_t bytes_since_last;
    if (!heap_profile_should_sample(size, &bytes_since_last)) {
        return NULL;
    }
    struct heap_profile_entry *ent = PyMem_RawMalloc(sizeof(*ent));
    if (ent == NULL) {
        return NULL;
    }
    ent->ptr = ptr;
    ent->size = size;
    ent->bytes_since_last_sample = bytes_since_last;
    ent->allocs_since_last_sample = heap_profiler.allocs_since_last;
    heap_profiler.allocs_since_last = 0;
    heap_profile_collect_traceback(ent);
    heap_profile_global_insert(ent);

    /* Copy to allocation list (all samples since last reset). */
    struct heap_profile_entry *copy = PyMem_RawMalloc(sizeof(*copy));
    if (copy != NULL) {
        memcpy(copy, ent, sizeof(*copy));
        copy->ptr = NULL;
        if (copy->traceback_id != NULL && heap_profiler.interning_table != NULL) {
            _Py_traceback_retain(copy->traceback_id, heap_profiler.interning_table);
        }
        heap_profile_accumulated_insert(copy);
    }
    return ent;
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
    env = getenv("PYTHON_HEAP_PROFILE_DEBUG");
    heap_profiler.print_debug = (env != NULL && env[0] != '\0');
    if (heap_profiler.sample_interval_bytes > 0
        && heap_profiler.interning_table == NULL) {
        Py_traceback_interning_allocator_t raw_alloc = {
            PyMem_RawMalloc,
            PyMem_RawFree,
        };
        heap_profiler.interning_table = _Py_traceback_interning_table_new(&raw_alloc);
    }
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
    return heap_profiler.sample_interval_bytes > 0 && heap_profiler.initialized;
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
