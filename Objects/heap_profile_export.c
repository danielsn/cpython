/* Heap profile export: pprof and OTel formats via pure C hand-written protobuf.
 *
 * pprof: https://github.com/google/pprof/blob/main/proto/profile.proto
 * OTel: Full Profile with stacktraces, locations, functions, string_table
 *       https://github.com/open-telemetry/opentelemetry-proto-profile
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "Python.h"
#include "pycore_hashtable.h"
#include "pycore_heap_profile.h"
#include "pycore_pymem.h"
#include "pycore_traceback.h"

/* Use larger limit for export to capture deeper stacks */
#define HEAP_EXPORT_TRACEBACK_MAX 128

/* -------------------------------------------------------------------------
 * Dynamic buffer for protobuf encoding
 * ------------------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} dynbuf_t;

#define DYN_INIT_CAP 4096

static int
dynbuf_grow(dynbuf_t *b, size_t need)
{
    size_t new_cap = b->cap;
    if (new_cap == 0) {
        new_cap = DYN_INIT_CAP;
    }
    while (b->len + need > new_cap) {
        if (new_cap > SIZE_MAX / 2) {
            return -1;
        }
        new_cap *= 2;
    }
    unsigned char *p = PyMem_RawRealloc(b->data, new_cap);
    if (p == NULL) {
        return -1;
    }
    b->data = p;
    b->cap = new_cap;
    return 0;
}

static int
dynbuf_append(dynbuf_t *b, const void *ptr, size_t n)
{
    if (b->len + n > b->cap && dynbuf_grow(b, n) < 0) {
        return -1;
    }
    if (n > 0) {
        memcpy(b->data + b->len, ptr, n);
    }
    b->len += n;
    return 0;
}

#define DYN_APPEND(b, ptr, n) do { \
    if (dynbuf_append((b), (ptr), (n)) < 0) goto err; \
} while (0)

#define DYN_APPEND_FUNC(b, fn, arg) do { \
    if (fn((b), (arg)) < 0) goto err; \
} while (0)

/* -------------------------------------------------------------------------
 * Protobuf wire encoding helpers
 * ------------------------------------------------------------------------- */

#define PB_WIRETYPE_VARINT   0
#define PB_WIRETYPE_64BIT    1
#define PB_WIRETYPE_LEN      2

static int
pb_put_varint(dynbuf_t *b, uint64_t val)
{
    unsigned char buf[10];
    size_t n = 0;
    do {
        buf[n++] = (unsigned char)((val & 0x7F) | (val > 0x7F ? 0x80 : 0));
        val >>= 7;
    } while (val != 0);
    return dynbuf_append(b, buf, n);
}

static int
pb_put_tag(dynbuf_t *b, int field_num, int wire_type)
{
    return pb_put_varint(b, (uint64_t)((field_num << 3) | wire_type));
}

static int
pb_put_bytes(dynbuf_t *b, const void *data, size_t len)
{
    if (pb_put_varint(b, (uint64_t)len) < 0) return -1;
    if (len > 0 && dynbuf_append(b, data, len) < 0) return -1;
    return 0;
}

static int
pb_put_int64(dynbuf_t *b, int64_t val)
{
    return pb_put_varint(b, (uint64_t)val);
}

static int
pb_put_uint64(dynbuf_t *b, uint64_t val)
{
    return pb_put_varint(b, val);
}

static int
pb_put_string(dynbuf_t *b, const char *s)
{
    if (s == NULL) s = "";
    size_t len = strlen(s);
    return pb_put_bytes(b, s, len);
}

static int
pb_put_fixed64(dynbuf_t *b, uint64_t val)
{
    unsigned char buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (unsigned char)(val & 0xFF);
        val >>= 8;
    }
    return dynbuf_append(b, buf, 8);
}

/* -------------------------------------------------------------------------
 * String table: pre-built from traceback interning table (1:1 mapping).
 * Fixed entries first, then interned strings via _Py_traceback_build_export_string_table.
 * ------------------------------------------------------------------------- */

typedef struct {
    char **strings;   /* owned, to be freed */
    size_t count;
    size_t cap;
} string_table_t;

static void
string_table_clear(string_table_t *st)
{
    for (size_t i = 0; i < st->count; i++) {
        PyMem_RawFree(st->strings[i]);
    }
    PyMem_RawFree(st->strings);
    st->strings = NULL;
    st->count = st->cap = 0;
}

/* Append raw string. Returns index. */
static int64_t
string_table_add_str(string_table_t *st, const char *s)
{
    if (s == NULL) s = "";
    if (st->count >= st->cap) {
        size_t new_cap = st->cap ? st->cap * 2 : 64;
        char **p = PyMem_RawRealloc(st->strings, new_cap * sizeof(char *));
        if (p == NULL) return -1;
        st->strings = p;
        st->cap = new_cap;
    }
    char *copy = _PyMem_RawStrdup(s);
    if (copy == NULL) return -1;
    st->strings[st->count] = copy;
    return (int64_t)st->count++;
}

/* Callback for _Py_traceback_build_export_string_table */
static int64_t
string_table_add_str_cb(void *ctx, const char *s)
{
    return string_table_add_str((string_table_t *)ctx, s);
}

/* -------------------------------------------------------------------------
 * Frame key for deduplication: (filename, name, lineno)
 * ------------------------------------------------------------------------- */

typedef struct {
    int64_t filename_idx;
    int64_t name_idx;
    int lineno;
    uint64_t function_id;
    uint64_t location_id;
} frame_entry_t;

typedef struct {
    frame_entry_t *entries;
    size_t count;
    size_t cap;
} frame_table_t;

static void
frame_table_clear(frame_table_t *ft)
{
    PyMem_RawFree(ft->entries);
    ft->entries = NULL;
    ft->count = ft->cap = 0;
}

static frame_entry_t *
frame_table_lookup_or_add(frame_table_t *ft, int64_t filename_idx, int64_t name_idx,
                         int lineno, uint64_t *next_func_id, uint64_t *next_loc_id)
{
    for (size_t i = 0; i < ft->count; i++) {
        frame_entry_t *e = &ft->entries[i];
        if (e->filename_idx == filename_idx && e->name_idx == name_idx
            && e->lineno == lineno) {
            return e;
        }
    }
    if (ft->count >= ft->cap) {
        size_t new_cap = ft->cap ? ft->cap * 2 : 64;
        frame_entry_t *p = PyMem_RawRealloc(ft->entries,
                                           new_cap * sizeof(frame_entry_t));
        if (p == NULL) return NULL;
        ft->entries = p;
        ft->cap = new_cap;
    }
    frame_entry_t *e = &ft->entries[ft->count++];
    e->filename_idx = filename_idx;
    e->name_idx = name_idx;
    e->lineno = lineno;
    e->function_id = (*next_func_id)++;
    e->location_id = (*next_loc_id)++;
    return e;
}

/* -------------------------------------------------------------------------
 * pprof submessage encoding
 * ------------------------------------------------------------------------- */

static int
encode_value_type(dynbuf_t *b, int64_t type_idx, int64_t unit_idx)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, type_idx) < 0) return -1;
    if (pb_put_tag(b, 2, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, unit_idx) < 0) return -1;
    return 0;
}

static int
encode_line(dynbuf_t *b, uint64_t function_id, int64_t line)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_uint64(b, function_id) < 0) return -1;
    if (pb_put_tag(b, 2, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, line) < 0) return -1;
    return 0;
}

static int
encode_function(dynbuf_t *b, uint64_t id, int64_t name, int64_t system_name,
                int64_t filename, int64_t start_line)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_uint64(b, id) < 0) return -1;
    if (pb_put_tag(b, 2, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, name) < 0) return -1;
    if (pb_put_tag(b, 3, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, system_name) < 0) return -1;
    if (pb_put_tag(b, 4, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, filename) < 0) return -1;
    if (pb_put_tag(b, 5, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, start_line) < 0) return -1;
    return 0;
}

static int
encode_location(dynbuf_t *b, uint64_t id, uint64_t mapping_id, uint64_t address,
               uint64_t function_id, int64_t line)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_uint64(b, id) < 0) return -1;
    if (pb_put_tag(b, 2, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_uint64(b, mapping_id) < 0) return -1;
    if (pb_put_tag(b, 3, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_uint64(b, address) < 0) return -1;
    if (pb_put_tag(b, 4, PB_WIRETYPE_LEN) < 0) return -1;
    dynbuf_t line_buf = {0};
    if (encode_line(&line_buf, function_id, line) < 0) {
        PyMem_RawFree(line_buf.data);
        return -1;
    }
    if (pb_put_bytes(b, line_buf.data, line_buf.len) < 0) {
        PyMem_RawFree(line_buf.data);
        return -1;
    }
    PyMem_RawFree(line_buf.data);
    return 0;
}

static int
encode_mapping(dynbuf_t *b, uint64_t id, int64_t filename_idx)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_uint64(b, id) < 0) return -1;
    if (pb_put_tag(b, 5, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_int64(b, filename_idx) < 0) return -1;
    return 0;
}

static int
encode_sample(dynbuf_t *b, const uint64_t *loc_ids, size_t nloc,
             const int64_t *values, size_t nval)
{
    if (nloc > 0) {
        if (pb_put_tag(b, 1, PB_WIRETYPE_LEN) < 0) return -1;
        dynbuf_t packed = {0};
        for (size_t i = 0; i < nloc; i++) {
            if (pb_put_varint(&packed, loc_ids[i]) < 0) {
                PyMem_RawFree(packed.data);
                return -1;
            }
        }
        if (pb_put_bytes(b, packed.data, packed.len) < 0) {
            PyMem_RawFree(packed.data);
            return -1;
        }
        PyMem_RawFree(packed.data);
    }
    if (nval > 0) {
        if (pb_put_tag(b, 2, PB_WIRETYPE_LEN) < 0) return -1;
        dynbuf_t packed = {0};
        for (size_t i = 0; i < nval; i++) {
            if (pb_put_varint(&packed, (uint64_t)values[i]) < 0) {
                PyMem_RawFree(packed.data);
                return -1;
            }
        }
        if (pb_put_bytes(b, packed.data, packed.len) < 0) {
            PyMem_RawFree(packed.data);
            return -1;
        }
        PyMem_RawFree(packed.data);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Build pprof Profile into dynbuf. Returns 0 on success, -1 on error.
 * Caller owns profile_buf->data and must PyMem_RawFree it.
 * ------------------------------------------------------------------------- */
static int
build_pprof_profile(dynbuf_t *profile_buf, Py_traceback_interning_table_t *table)
{
    profile_buf->data = NULL;
    profile_buf->len = profile_buf->cap = 0;

    string_table_t st = {0};
    frame_table_t ft = {0};
    dynbuf_t sample_types_buf = {0};
    dynbuf_t samples_buf = {0};
    dynbuf_t mappings_buf = {0};
    dynbuf_t locations_buf = {0};
    dynbuf_t functions_buf = {0};

    uint64_t next_func_id = 1;
    uint64_t next_loc_id = 1;

    /* String table: fixed entries 0-6, "<python>" at 7, interned strings at 8+.
       Index 6 = "???" for NULL string_id. */
    const char *fixed[] = {"", "allocated_space", "bytes", "allocated_objects", "count", "unknown", "???"};
    st.strings = PyMem_RawCalloc(64, sizeof(char *));
    if (st.strings == NULL) goto err;
    st.cap = 64;
    for (size_t i = 0; i < sizeof(fixed)/sizeof(fixed[0]); i++) {
        st.strings[st.count] = _PyMem_RawStrdup(fixed[i]);
        if (st.strings[st.count] == NULL) goto err;
        st.count++;
    }

    int64_t python_filename_idx = string_table_add_str(&st, "<python>");
    if (python_filename_idx < 0) goto err;

    /* Build string table from interning table (1:1 mapping, indices stored in scratch) */
    if (_Py_traceback_build_export_string_table(table, &st, string_table_add_str_cb) < 0) {
        goto err;
    }

    /* First pass: collect all unique frames */
    PyTracebackFrameInfoWithIds frames[HEAP_EXPORT_TRACEBACK_MAX];
    for (struct heap_profile_entry *ent = heap_profile_get_first(); ent != NULL;
         ent = heap_profile_get_next(ent)) {
        if (ent->traceback_id == NULL) continue;
        int n = _Py_traceback_fill_frames_with_string_ids(ent->traceback_id, table,
                                                         frames, HEAP_EXPORT_TRACEBACK_MAX);
        for (int i = 0; i < n; i++) {
            int64_t filename_idx = _Py_traceback_string_id_get_export_index(
                frames[i].filename_id, 6);
            int64_t name_idx = _Py_traceback_string_id_get_export_index(
                frames[i].name_id, 6);
            frame_table_lookup_or_add(&ft, filename_idx, name_idx, frames[i].lineno,
                                     &next_func_id, &next_loc_id);
        }
    }

    /* Encode sample_type: allocated_space/bytes, allocated_objects/count (each as separate repeated element) */
    dynbuf_t vt_buf = {0};
    if (encode_value_type(&vt_buf, 1, 2) < 0) {  /* type=allocated_space, unit=bytes */
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    if (pb_put_tag(&sample_types_buf, 1, PB_WIRETYPE_LEN) < 0) {
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    if (pb_put_bytes(&sample_types_buf, vt_buf.data, vt_buf.len) < 0) {
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    PyMem_RawFree(vt_buf.data);
    vt_buf = (dynbuf_t){0};
    if (encode_value_type(&vt_buf, 3, 4) < 0) {  /* type=allocated_objects, unit=count */
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    if (pb_put_tag(&sample_types_buf, 1, PB_WIRETYPE_LEN) < 0) {
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    if (pb_put_bytes(&sample_types_buf, vt_buf.data, vt_buf.len) < 0) {
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    PyMem_RawFree(vt_buf.data);

    /* Encode samples */
    for (struct heap_profile_entry *ent = heap_profile_get_first(); ent != NULL;
         ent = heap_profile_get_next(ent)) {
        uint64_t loc_ids[HEAP_EXPORT_TRACEBACK_MAX];
        int64_t values[2] = {
            (int64_t)ent->bytes_since_last_sample,
            (int64_t)ent->allocs_since_last_sample
        };
        size_t nloc = 0;

        if (ent->traceback_id != NULL) {
            int n = _Py_traceback_fill_frames_with_string_ids(ent->traceback_id, table,
                                                              frames, HEAP_EXPORT_TRACEBACK_MAX);
            for (int i = n - 1; i >= 0 && nloc < HEAP_EXPORT_TRACEBACK_MAX; i--) {
                int64_t filename_idx = _Py_traceback_string_id_get_export_index(
                    frames[i].filename_id, 6);
                int64_t name_idx = _Py_traceback_string_id_get_export_index(
                    frames[i].name_id, 6);
                frame_entry_t *fe = frame_table_lookup_or_add(&ft, filename_idx, name_idx,
                                                              frames[i].lineno,
                                                              &next_func_id, &next_loc_id);
                if (fe == NULL) goto err;
                loc_ids[nloc++] = fe->location_id;
            }
        }
        if (nloc == 0) {
            /* No traceback: use location_id 1 (unknown) - we'll create it */
            loc_ids[nloc++] = 1;
        }

        if (pb_put_tag(&samples_buf, 2, PB_WIRETYPE_LEN) < 0) goto err;
        dynbuf_t sample_buf = {0};
        if (encode_sample(&sample_buf, loc_ids, nloc, values, 2) < 0) {
            PyMem_RawFree(sample_buf.data);
            goto err;
        }
        if (pb_put_bytes(&samples_buf, sample_buf.data, sample_buf.len) < 0) {
            PyMem_RawFree(sample_buf.data);
            goto err;
        }
        PyMem_RawFree(sample_buf.data);
    }

    /* Ensure we have location_id 1 for samples with no traceback */
    if (next_loc_id == 1) {
        next_loc_id = 2;  /* Reserve 1 for "unknown" */
        next_func_id = 2; /* Reserve 1 for "unknown" function */
    }

    /* Encode mapping (id=1, filename=<python>) - each repeated needs tag+len+data */
    if (pb_put_tag(&mappings_buf, 3, PB_WIRETYPE_LEN) < 0) goto err;
    dynbuf_t map_buf = {0};
    if (encode_mapping(&map_buf, 1, python_filename_idx) < 0) goto err;
    if (pb_put_bytes(&mappings_buf, map_buf.data, map_buf.len) < 0) {
        PyMem_RawFree(map_buf.data);
        goto err;
    }
    PyMem_RawFree(map_buf.data);

    /* Encode locations and functions from frame table */
    for (size_t i = 0; i < ft.count; i++) {
        frame_entry_t *e = &ft.entries[i];
        if (pb_put_tag(&functions_buf, 5, PB_WIRETYPE_LEN) < 0) goto err;
        map_buf = (dynbuf_t){0};
        if (encode_function(&map_buf, e->function_id, e->name_idx, 0,
                            e->filename_idx, (int64_t)e->lineno) < 0) goto err;
        if (pb_put_bytes(&functions_buf, map_buf.data, map_buf.len) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err;
        }
        PyMem_RawFree(map_buf.data);
        if (pb_put_tag(&locations_buf, 4, PB_WIRETYPE_LEN) < 0) goto err;
        map_buf = (dynbuf_t){0};
        if (encode_location(&map_buf, e->location_id, 1, 0,
                           e->function_id, (int64_t)e->lineno) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err;
        }
        if (pb_put_bytes(&locations_buf, map_buf.data, map_buf.len) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err;
        }
        PyMem_RawFree(map_buf.data);
    }

    /* If we have samples with no traceback, create unknown location/function */
    if (ft.count == 0) {
        int64_t unknown_idx = 5;  /* "unknown" in fixed strings */
        if (pb_put_tag(&functions_buf, 5, PB_WIRETYPE_LEN) < 0) goto err;
        map_buf = (dynbuf_t){0};
        if (encode_function(&map_buf, 1, unknown_idx, 0, unknown_idx, 0) < 0) goto err;
        if (pb_put_bytes(&functions_buf, map_buf.data, map_buf.len) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err;
        }
        PyMem_RawFree(map_buf.data);
        if (pb_put_tag(&locations_buf, 4, PB_WIRETYPE_LEN) < 0) goto err;
        map_buf = (dynbuf_t){0};
        if (encode_location(&map_buf, 1, 1, 0, 1, 0) < 0) goto err;
        if (pb_put_bytes(&locations_buf, map_buf.data, map_buf.len) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err;
        }
        PyMem_RawFree(map_buf.data);
    }

    /* Assemble Profile message: sample_type, sample, mapping, location, function, string_table, time_nanos, period_type.
     * Each repeated field is already tag+length+value per element. */
    DYN_APPEND(profile_buf, sample_types_buf.data, sample_types_buf.len);
    DYN_APPEND(profile_buf, samples_buf.data, samples_buf.len);
    DYN_APPEND(profile_buf, mappings_buf.data, mappings_buf.len);
    DYN_APPEND(profile_buf, locations_buf.data, locations_buf.len);
    DYN_APPEND(profile_buf, functions_buf.data, functions_buf.len);
    /* string_table: repeated string, each needs tag(6,LEN)+length+data */
    for (size_t i = 0; i < st.count; i++) {
        if (pb_put_tag(profile_buf, 6, PB_WIRETYPE_LEN) < 0) goto err;
        if (pb_put_string(profile_buf, st.strings[i]) < 0) goto err;
    }
    if (pb_put_tag(profile_buf, 9, PB_WIRETYPE_VARINT) < 0) goto err;
    if (pb_put_int64(profile_buf, (int64_t)time(NULL) * 1000000000LL) < 0) goto err;
    if (pb_put_tag(profile_buf, 11, PB_WIRETYPE_LEN) < 0) goto err;
    vt_buf = (dynbuf_t){0};
    if (encode_value_type(&vt_buf, 1, 2) < 0) {  /* period_type: allocated_space/bytes */
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    if (pb_put_bytes(profile_buf, vt_buf.data, vt_buf.len) < 0) {
        PyMem_RawFree(vt_buf.data);
        goto err;
    }
    PyMem_RawFree(vt_buf.data);

    string_table_clear(&st);
    frame_table_clear(&ft);
    _Py_traceback_clear_export_indices(table);
    PyMem_RawFree(sample_types_buf.data);
    PyMem_RawFree(samples_buf.data);
    PyMem_RawFree(mappings_buf.data);
    PyMem_RawFree(locations_buf.data);
    PyMem_RawFree(functions_buf.data);
    /* profile_buf is the output - caller owns it */
    return 0;

err:
    string_table_clear(&st);
    _Py_traceback_clear_export_indices(table);
    frame_table_clear(&ft);
    PyMem_RawFree(sample_types_buf.data);
    PyMem_RawFree(samples_buf.data);
    PyMem_RawFree(mappings_buf.data);
    PyMem_RawFree(locations_buf.data);
    PyMem_RawFree(functions_buf.data);
    PyMem_RawFree(profile_buf->data);
    profile_buf->data = NULL;
    profile_buf->len = profile_buf->cap = 0;
    return -1;
}

/* -------------------------------------------------------------------------
 * OTel export: full Profile with stacktraces, locations, functions
 * ------------------------------------------------------------------------- */

/* OTel uses uint32 for indices. Pack (function_idx << 32) | location_idx. */
typedef struct {
    uint32_t *locs;      /* OTel location indices, one per frame (oldest first) */
    size_t count;        /* Number of frames in this stacktrace */
    int64_t bytes;       /* Aggregated bytes allocated for this stacktrace */
    int64_t allocs;      /* Aggregated allocation count for this stacktrace */
} otel_stacktrace_entry_t;

/* Key for hash table lookup: (locs, nloc) */
typedef struct {
    const uint32_t *locs;
    size_t nloc;
} otel_st_key_t;

typedef struct {
    otel_stacktrace_entry_t *entries;
    size_t count;
    size_t cap;
    _Py_hashtable_t *index;  /* key -> (void*)(size_t)index */
} otel_stacktrace_table_t;

static Py_uhash_t
otel_st_hash(const void *keyp)
{
    const otel_st_key_t *k = keyp;
    Py_uhash_t h = 2166136261u;
    h ^= (Py_uhash_t)k->nloc;
    h *= 16777619u;
    for (size_t i = 0; i < k->nloc; i++) {
        h ^= (Py_uhash_t)k->locs[i];
        h *= 16777619u;
    }
    return h;
}

static int
otel_st_compare(const void *a, const void *b)
{
    const otel_st_key_t *ka = a, *kb = b;
    if (ka->nloc != kb->nloc) return 0;
    return memcmp(ka->locs, kb->locs, ka->nloc * sizeof(uint32_t)) == 0;
}

static void
otel_st_key_destroy(void *keyp)
{
    PyMem_RawFree(keyp);
}

static void
otel_stacktrace_table_clear(otel_stacktrace_table_t *t)
{
    for (size_t i = 0; i < t->count; i++) {
        PyMem_RawFree(t->entries[i].locs);
    }
    PyMem_RawFree(t->entries);
    t->entries = NULL;
    t->count = t->cap = 0;
    if (t->index != NULL) {
        _Py_hashtable_destroy(t->index);
        t->index = NULL;
    }
}

/* Find or add stacktrace. Returns index. Adds bytes/allocs to existing. */
static int64_t
otel_stacktrace_find_or_add(otel_stacktrace_table_t *t,
                            const uint32_t *locs, size_t nloc,
                            int64_t bytes, int64_t allocs)
{
    if (t->index == NULL) {
        _Py_hashtable_allocator_t alloc = {PyMem_RawMalloc, PyMem_RawFree};
        t->index = _Py_hashtable_new_full(otel_st_hash, otel_st_compare,
                                          otel_st_key_destroy, NULL, &alloc);
        if (t->index == NULL) return -1;
    }

    otel_st_key_t lookup_key = { .locs = locs, .nloc = nloc };
    _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(t->index, &lookup_key);
    if (entry != NULL) {
        size_t idx = (size_t)(uintptr_t)entry->value;
        t->entries[idx].bytes += bytes;
        t->entries[idx].allocs += allocs;
        return (int64_t)idx;
    }

    if (t->count >= t->cap) {
        size_t new_cap = t->cap ? t->cap * 2 : 64;
        otel_stacktrace_entry_t *p = PyMem_RawRealloc(t->entries,
                                                new_cap * sizeof(otel_stacktrace_entry_t));
        if (p == NULL) return -1;
        t->entries = p;
        t->cap = new_cap;
    }
    otel_stacktrace_entry_t *e = &t->entries[t->count];
    e->locs = PyMem_RawMalloc(nloc * sizeof(uint32_t));
    if (e->locs == NULL) return -1;
    memcpy(e->locs, locs, nloc * sizeof(uint32_t));
    e->count = nloc;
    e->bytes = bytes;
    e->allocs = allocs;

    otel_st_key_t *stored_key = PyMem_RawMalloc(sizeof(otel_st_key_t));
    if (stored_key == NULL) {
        PyMem_RawFree(e->locs);
        return -1;
    }
    stored_key->locs = e->locs;
    stored_key->nloc = nloc;
    if (_Py_hashtable_set(t->index, stored_key, (void *)(uintptr_t)t->count) < 0) {
        PyMem_RawFree(stored_key);
        PyMem_RawFree(e->locs);
        return -1;
    }
    return (int64_t)t->count++;
}

/* OTel Mapping: field 4 filename_index (uint32) */
static int
otel_encode_mapping(dynbuf_t *b, uint32_t filename_index)
{
    if (pb_put_tag(b, 4, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, filename_index) < 0) return -1;
    return 0;
}

/* OTel Line: field 1 function_index, 2 line (uint32) */
static int
otel_encode_line(dynbuf_t *b, uint32_t function_index, uint32_t line)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, function_index) < 0) return -1;
    if (pb_put_tag(b, 2, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, line) < 0) return -1;
    return 0;
}

/* OTel Location: field 1 mapping_index, 2 address, 3 line (embedded) */
static int
otel_encode_location(dynbuf_t *b, uint32_t mapping_index, uint32_t function_index,
                     uint32_t line)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, mapping_index) < 0) return -1;
    if (pb_put_tag(b, 2, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, 0) < 0) return -1;  /* address */
    if (pb_put_tag(b, 3, PB_WIRETYPE_LEN) < 0) return -1;
    dynbuf_t line_buf = {0};
    if (otel_encode_line(&line_buf, function_index, line) < 0) {
        PyMem_RawFree(line_buf.data);
        return -1;
    }
    if (pb_put_bytes(b, line_buf.data, line_buf.len) < 0) {
        PyMem_RawFree(line_buf.data);
        return -1;
    }
    PyMem_RawFree(line_buf.data);
    return 0;
}

/* OTel Function: field 1 name_index, 2 system_name_index, 3 filename_index, 4 start_line */
static int
otel_encode_function(dynbuf_t *b, uint32_t name_index, uint32_t filename_index,
                     uint32_t start_line)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, name_index) < 0) return -1;
    if (pb_put_tag(b, 2, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, 0) < 0) return -1;  /* system_name */
    if (pb_put_tag(b, 3, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, filename_index) < 0) return -1;
    if (pb_put_tag(b, 4, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, start_line) < 0) return -1;
    return 0;
}

/* OTel Stacktrace: field 1 location_indices (repeated uint32) */
static int
otel_encode_stacktrace(dynbuf_t *b, const uint32_t *locs, size_t nloc)
{
    if (pb_put_tag(b, 1, PB_WIRETYPE_LEN) < 0) return -1;
    dynbuf_t packed = {0};
    for (size_t i = 0; i < nloc; i++) {
        if (pb_put_varint(&packed, locs[i]) < 0) {
            PyMem_RawFree(packed.data);
            return -1;
        }
    }
    if (pb_put_bytes(b, packed.data, packed.len) < 0) {
        PyMem_RawFree(packed.data);
        return -1;
    }
    PyMem_RawFree(packed.data);
    return 0;
}

/* OTel ProfileType: field 3 type_index, 4 unit_index, 10 stacktrace_indices, 13 values */
static int
otel_encode_profile_type(dynbuf_t *b, uint32_t type_index, uint32_t unit_index,
                         const uint32_t *stacktrace_indices, size_t n,
                         const int64_t *values)
{
    if (pb_put_tag(b, 3, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, type_index) < 0) return -1;
    if (pb_put_tag(b, 4, PB_WIRETYPE_VARINT) < 0) return -1;
    if (pb_put_varint(b, unit_index) < 0) return -1;
    if (n > 0) {
        if (pb_put_tag(b, 10, PB_WIRETYPE_LEN) < 0) return -1;
        dynbuf_t packed = {0};
        for (size_t i = 0; i < n; i++) {
            if (pb_put_varint(&packed, stacktrace_indices[i]) < 0) {
                PyMem_RawFree(packed.data);
                return -1;
            }
        }
        if (pb_put_bytes(b, packed.data, packed.len) < 0) {
            PyMem_RawFree(packed.data);
            return -1;
        }
        PyMem_RawFree(packed.data);
        if (pb_put_tag(b, 13, PB_WIRETYPE_LEN) < 0) return -1;
        packed = (dynbuf_t){0};
        for (size_t i = 0; i < n; i++) {
            if (pb_put_varint(&packed, (uint64_t)values[i]) < 0) {
                PyMem_RawFree(packed.data);
                return -1;
            }
        }
        if (pb_put_bytes(b, packed.data, packed.len) < 0) {
            PyMem_RawFree(packed.data);
            return -1;
        }
        PyMem_RawFree(packed.data);
    }
    return 0;
}

/* Context for add_frame callback - stores frame data for encoding in order */
typedef struct {
    uint32_t *name_idx;
    uint32_t *filename_idx;
    uint32_t *lineno;
    size_t count;
    size_t cap;
} otel_frame_ctx_t;

static int64_t
otel_add_frame_cb(void *ctx, int64_t filename_idx, int64_t name_idx, int lineno)
{
    otel_frame_ctx_t *fc = (otel_frame_ctx_t *)ctx;
    if (fc->count >= fc->cap) {
        size_t new_cap = fc->cap ? fc->cap * 2 : 64;
        uint32_t *n = PyMem_RawRealloc(fc->name_idx, new_cap * sizeof(uint32_t));
        uint32_t *f = PyMem_RawRealloc(fc->filename_idx, new_cap * sizeof(uint32_t));
        uint32_t *l = PyMem_RawRealloc(fc->lineno, new_cap * sizeof(uint32_t));
        if (!n || !f || !l) return -1;
        fc->name_idx = n;
        fc->filename_idx = f;
        fc->lineno = l;
        fc->cap = new_cap;
    }
    uint32_t idx = (uint32_t)fc->count;
    fc->name_idx[idx] = (uint32_t)name_idx;
    fc->filename_idx[idx] = (uint32_t)filename_idx;
    fc->lineno[idx] = (uint32_t)lineno;
    fc->count++;
    return ((int64_t)idx << 32) | idx;
}

static int
build_otel_profile(dynbuf_t *profile_buf, Py_traceback_interning_table_t *table)
{
    profile_buf->data = NULL;
    profile_buf->len = profile_buf->cap = 0;

    string_table_t st = {0};
    otel_stacktrace_table_t stacktraces = {0};
    otel_frame_ctx_t frame_ctx = {0};
    frame_ctx.name_idx = NULL;
    frame_ctx.filename_idx = NULL;
    frame_ctx.lineno = NULL;
    dynbuf_t stacktraces_buf = {0};
    dynbuf_t mappings_buf = {0};
    dynbuf_t locations_buf = {0};
    dynbuf_t functions_buf = {0};
    dynbuf_t profile_types_buf = {0};

    /* String table: fixed 0-6, "<python>" at 7, interned at 8+ */
    const char *fixed[] = {"", "allocated_space", "bytes", "allocated_objects", "count", "unknown", "???"};
    st.strings = PyMem_RawCalloc(64, sizeof(char *));
    if (st.strings == NULL) goto err_otel;
    st.cap = 64;
    for (size_t i = 0; i < sizeof(fixed)/sizeof(fixed[0]); i++) {
        st.strings[st.count] = _PyMem_RawStrdup(fixed[i]);
        if (st.strings[st.count] == NULL) goto err_otel;
        st.count++;
    }
    if (string_table_add_str(&st, "<python>") < 0) goto err_otel;

    if (_Py_traceback_build_export_string_table(table, &st, string_table_add_str_cb) < 0) {
        goto err_otel;
    }

    /* Reserve index 0 for "unknown" frame (no traceback) */
    frame_ctx.cap = 64;
    frame_ctx.name_idx = PyMem_RawCalloc(64, sizeof(uint32_t));
    frame_ctx.filename_idx = PyMem_RawCalloc(64, sizeof(uint32_t));
    frame_ctx.lineno = PyMem_RawCalloc(64, sizeof(uint32_t));
    if (!frame_ctx.name_idx || !frame_ctx.filename_idx || !frame_ctx.lineno) {
        PyMem_RawFree(frame_ctx.name_idx);
        PyMem_RawFree(frame_ctx.filename_idx);
        PyMem_RawFree(frame_ctx.lineno);
        goto err_otel;
    }
    frame_ctx.name_idx[0] = 5;   /* "unknown" */
    frame_ctx.filename_idx[0] = 5;
    frame_ctx.lineno[0] = 0;
    frame_ctx.count = 1;

    if (_Py_traceback_build_export_frame_table(table, &frame_ctx, otel_add_frame_cb, 6) < 0) {
        goto err_otel;
    }

    /* Aggregate samples by stacktrace */
    Py_traceback_frame_id_t frame_ids[HEAP_EXPORT_TRACEBACK_MAX];
    for (struct heap_profile_entry *ent = heap_profile_get_first(); ent != NULL;
         ent = heap_profile_get_next(ent)) {
        uint32_t locs[HEAP_EXPORT_TRACEBACK_MAX];
        size_t nloc = 0;

        if (ent->traceback_id != NULL) {
            int n = _Py_traceback_fill_frame_ids(ent->traceback_id, table,
                                                 frame_ids, HEAP_EXPORT_TRACEBACK_MAX);
            for (int i = n - 1; i >= 0 && nloc < HEAP_EXPORT_TRACEBACK_MAX; i--) {
                uint32_t loc_idx;
                _Py_traceback_frame_id_get_export_indices(frame_ids[i], NULL, &loc_idx);
                locs[nloc++] = loc_idx;
            }
        }
        if (nloc == 0) {
            locs[nloc++] = 0;  /* unknown location */
        }
        if (otel_stacktrace_find_or_add(&stacktraces, locs, nloc,
                                        (int64_t)ent->bytes_since_last_sample,
                                        (int64_t)ent->allocs_since_last_sample) < 0) {
            goto err_otel;
        }
    }

    /* Encode Profile fields: 7 stacktraces, 8 mappings, 9 locations, 10 functions,
       13 string_table, 14 profile_types */
    for (size_t i = 0; i < stacktraces.count; i++) {
        if (pb_put_tag(&stacktraces_buf, 7, PB_WIRETYPE_LEN) < 0) goto err_otel;
        dynbuf_t st_buf = {0};
        if (otel_encode_stacktrace(&st_buf, stacktraces.entries[i].locs,
                                   stacktraces.entries[i].count) < 0) {
            PyMem_RawFree(st_buf.data);
            goto err_otel;
        }
        if (pb_put_bytes(&stacktraces_buf, st_buf.data, st_buf.len) < 0) {
            PyMem_RawFree(st_buf.data);
            goto err_otel;
        }
        PyMem_RawFree(st_buf.data);
    }

    if (pb_put_tag(&mappings_buf, 8, PB_WIRETYPE_LEN) < 0) goto err_otel;
    dynbuf_t map_buf = {0};
    if (otel_encode_mapping(&map_buf, 7) < 0) goto err_otel;  /* "<python>" */
    if (pb_put_bytes(&mappings_buf, map_buf.data, map_buf.len) < 0) {
        PyMem_RawFree(map_buf.data);
        goto err_otel;
    }
    PyMem_RawFree(map_buf.data);

    for (size_t i = 0; i < frame_ctx.count; i++) {
        if (pb_put_tag(&functions_buf, 10, PB_WIRETYPE_LEN) < 0) goto err_otel;
        map_buf = (dynbuf_t){0};
        if (otel_encode_function(&map_buf, frame_ctx.name_idx[i],
                                 frame_ctx.filename_idx[i], frame_ctx.lineno[i]) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err_otel;
        }
        if (pb_put_bytes(&functions_buf, map_buf.data, map_buf.len) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err_otel;
        }
        PyMem_RawFree(map_buf.data);

        if (pb_put_tag(&locations_buf, 9, PB_WIRETYPE_LEN) < 0) goto err_otel;
        map_buf = (dynbuf_t){0};
        if (otel_encode_location(&map_buf, 0, (uint32_t)i, frame_ctx.lineno[i]) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err_otel;
        }
        if (pb_put_bytes(&locations_buf, map_buf.data, map_buf.len) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err_otel;
        }
        PyMem_RawFree(map_buf.data);
    }

    /* ProfileType for bytes (1,2) and allocs (3,4) */
    uint32_t *st_indices = PyMem_RawMalloc(stacktraces.count * sizeof(uint32_t));
    int64_t *vals = PyMem_RawMalloc(stacktraces.count * sizeof(int64_t));
    if (!st_indices || !vals) {
        PyMem_RawFree(st_indices);
        PyMem_RawFree(vals);
        goto err_otel;
    }
    for (size_t i = 0; i < stacktraces.count; i++) {
        st_indices[i] = (uint32_t)i;
    }
    static const struct { uint32_t type; uint32_t unit; } profile_type_spec[] = {
        {1, 2},   /* allocated_space / bytes */
        {3, 4},   /* allocated_objects / count */
    };
    for (size_t pt = 0; pt < sizeof(profile_type_spec)/sizeof(profile_type_spec[0]); pt++) {
        for (size_t i = 0; i < stacktraces.count; i++) {
            vals[i] = (pt == 0) ? stacktraces.entries[i].bytes : stacktraces.entries[i].allocs;
        }
        if (pb_put_tag(&profile_types_buf, 14, PB_WIRETYPE_LEN) < 0) goto err_pt;
        map_buf = (dynbuf_t){0};
        if (otel_encode_profile_type(&map_buf, profile_type_spec[pt].type,
                                     profile_type_spec[pt].unit,
                                     st_indices, stacktraces.count, vals) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err_pt;
        }
        if (pb_put_bytes(&profile_types_buf, map_buf.data, map_buf.len) < 0) {
            PyMem_RawFree(map_buf.data);
            goto err_pt;
        }
        PyMem_RawFree(map_buf.data);
    }
    PyMem_RawFree(st_indices);
    PyMem_RawFree(vals);

    /* Assemble Profile: 7, 8, 9, 10, 13, 14, 1, 2, 3 */
    uint64_t now_ns = (uint64_t)time(NULL) * 1000000000ULL;
    static const unsigned char profile_id[16] = {
        'P', 'y', 'h', 'e', 'a', 'p', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    if (pb_put_tag(profile_buf, 1, PB_WIRETYPE_LEN) < 0) goto err_otel;
    if (pb_put_bytes(profile_buf, profile_id, 16) < 0) goto err_otel;
    if (pb_put_tag(profile_buf, 2, PB_WIRETYPE_64BIT) < 0) goto err_otel;
    if (pb_put_fixed64(profile_buf, now_ns) < 0) goto err_otel;
    if (pb_put_tag(profile_buf, 3, PB_WIRETYPE_64BIT) < 0) goto err_otel;
    if (pb_put_fixed64(profile_buf, now_ns) < 0) goto err_otel;
    if (dynbuf_append(profile_buf, stacktraces_buf.data, stacktraces_buf.len) < 0) goto err_otel;
    if (dynbuf_append(profile_buf, mappings_buf.data, mappings_buf.len) < 0) goto err_otel;
    if (dynbuf_append(profile_buf, locations_buf.data, locations_buf.len) < 0) goto err_otel;
    if (dynbuf_append(profile_buf, functions_buf.data, functions_buf.len) < 0) goto err_otel;
    for (size_t i = 0; i < st.count; i++) {
        if (pb_put_tag(profile_buf, 13, PB_WIRETYPE_LEN) < 0) goto err_otel;
        if (pb_put_string(profile_buf, st.strings[i]) < 0) goto err_otel;
    }
    if (dynbuf_append(profile_buf, profile_types_buf.data, profile_types_buf.len) < 0) goto err_otel;

    string_table_clear(&st);
    otel_stacktrace_table_clear(&stacktraces);
    PyMem_RawFree(frame_ctx.name_idx);
    PyMem_RawFree(frame_ctx.filename_idx);
    PyMem_RawFree(frame_ctx.lineno);
    _Py_traceback_clear_export_indices(table);
    _Py_traceback_clear_export_frame_indices(table);
    PyMem_RawFree(stacktraces_buf.data);
    PyMem_RawFree(mappings_buf.data);
    PyMem_RawFree(locations_buf.data);
    PyMem_RawFree(functions_buf.data);
    PyMem_RawFree(profile_types_buf.data);
    return 0;

err_pt:
    PyMem_RawFree(st_indices);
    PyMem_RawFree(vals);
    goto err_otel;

err_otel:
    string_table_clear(&st);
    otel_stacktrace_table_clear(&stacktraces);
    PyMem_RawFree(frame_ctx.name_idx);
    PyMem_RawFree(frame_ctx.filename_idx);
    PyMem_RawFree(frame_ctx.lineno);
    _Py_traceback_clear_export_indices(table);
    _Py_traceback_clear_export_frame_indices(table);
    PyMem_RawFree(stacktraces_buf.data);
    PyMem_RawFree(mappings_buf.data);
    PyMem_RawFree(locations_buf.data);
    PyMem_RawFree(functions_buf.data);
    PyMem_RawFree(profile_types_buf.data);
    PyMem_RawFree(profile_buf->data);
    return -1;
}

int
heap_profile_export_pprof(FILE *fp)
{
    if (!heap_profile_is_enabled()) {
        return -1;
    }
    Py_traceback_interning_table_t *table = heap_profile_get_interning_table();
    if (table == NULL) {
        return -1;
    }
    dynbuf_t profile_buf = {0};
    if (build_pprof_profile(&profile_buf, table) < 0) {
        return -1;
    }
    int ret = 0;
    if (profile_buf.len > 0 && fwrite(profile_buf.data, 1, profile_buf.len, fp) != profile_buf.len) {
        ret = -1;
    }
    PyMem_RawFree(profile_buf.data);
    return ret;
}

int
heap_profile_export_otel(FILE *fp)
{
    if (!heap_profile_is_enabled()) {
        return -1;
    }
    Py_traceback_interning_table_t *table = heap_profile_get_interning_table();
    if (table == NULL) {
        return -1;
    }
    dynbuf_t profile_buf = {0};
    if (build_otel_profile(&profile_buf, table) < 0) {
        return -1;
    }
    /* Wrap: Profile -> ScopeProfiles(2) -> ResourceProfiles(2) -> Request(1) */
    dynbuf_t scope_buf = {0};
    if (pb_put_tag(&scope_buf, 2, PB_WIRETYPE_LEN) < 0) {
        PyMem_RawFree(profile_buf.data);
        return -1;
    }
    if (pb_put_bytes(&scope_buf, profile_buf.data, profile_buf.len) < 0) {
        PyMem_RawFree(profile_buf.data);
        PyMem_RawFree(scope_buf.data);
        return -1;
    }
    PyMem_RawFree(profile_buf.data);
    dynbuf_t resource_buf = {0};
    if (pb_put_tag(&resource_buf, 2, PB_WIRETYPE_LEN) < 0) {
        PyMem_RawFree(scope_buf.data);
        return -1;
    }
    if (pb_put_bytes(&resource_buf, scope_buf.data, scope_buf.len) < 0) {
        PyMem_RawFree(scope_buf.data);
        PyMem_RawFree(resource_buf.data);
        return -1;
    }
    PyMem_RawFree(scope_buf.data);
    dynbuf_t envelope = {0};
    if (pb_put_tag(&envelope, 1, PB_WIRETYPE_LEN) < 0) {
        PyMem_RawFree(resource_buf.data);
        return -1;
    }
    if (pb_put_bytes(&envelope, resource_buf.data, resource_buf.len) < 0) {
        PyMem_RawFree(resource_buf.data);
        PyMem_RawFree(envelope.data);
        return -1;
    }
    PyMem_RawFree(resource_buf.data);
    int ret = 0;
    if (envelope.len > 0 && fwrite(envelope.data, 1, envelope.len, fp) != envelope.len) {
        ret = -1;
    }
    PyMem_RawFree(envelope.data);
    return ret;
}
