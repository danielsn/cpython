#ifndef Py_INTERNAL_TRACEBACK_H
#define Py_INTERNAL_TRACEBACK_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Export for '_ctypes' shared extension
PyAPI_FUNC(int) _Py_DisplaySourceLine(PyObject *, PyObject *, int, int, int *, PyObject **);

// Export for 'pyexact' shared extension
PyAPI_FUNC(void) _PyTraceback_Add(const char *, const char *, int);

/* Write the Python traceback into the file 'fd'. For example:

       Traceback (most recent call first):
         File "xxx", line xxx in <xxx>
         File "xxx", line xxx in <xxx>
         ...
         File "xxx", line xxx in <xxx>

   This function is written for debug purpose only, to dump the traceback in
   the worst case: after a segmentation fault, at fatal error, etc. That's why,
   it is very limited. Strings are truncated to 100 characters and encoded to
   ASCII with backslashreplace. It doesn't write the source code, only the
   function name, filename and line number of each frame. Write only the first
   100 frames: if the traceback is truncated, write the line " ...".

   This function is signal safe. */

extern void _Py_DumpTraceback(
    int fd,
    PyThreadState *tstate);

/* Write the traceback of all threads into the file 'fd'. current_thread can be
   NULL.

   Return NULL on success, or an error message on error.

   This function is written for debug purpose only. It calls
   _Py_DumpTraceback() for each thread, and so has the same limitations. It
   only write the traceback of the first 100 threads: write "..." if there are
   more threads.

   If current_tstate is NULL, the function tries to get the Python thread state
   of the current thread. It is not an error if the function is unable to get
   the current Python thread state.

   If interp is NULL, the function tries to get the interpreter state from
   the current Python thread state, or from
   _PyGILState_GetInterpreterStateUnsafe() in last resort.

   It is better to pass NULL to interp and current_tstate, the function tries
   different options to retrieve this information.

   This function is signal safe. */

extern const char* _Py_DumpTracebackThreads(
    int fd,
    PyInterpreterState *interp,
    PyThreadState *current_tstate);

/* Write a Unicode object into the file descriptor fd. Encode the string to
   ASCII using the backslashreplace error handler.

   Do nothing if text is not a Unicode object.

   This function is signal safe. */
extern void _Py_DumpASCII(int fd, PyObject *text);

/* Format an integer as decimal into the file descriptor fd.

   This function is signal safe. */
extern void _Py_DumpDecimal(
    int fd,
    size_t value);

/* Format an integer as hexadecimal with width digits into fd file descriptor.
   The function is signal safe. */
extern void _Py_DumpHexadecimal(
    int fd,
    uintptr_t value,
    Py_ssize_t width);

extern PyObject* _PyTraceBack_FromFrame(
    PyObject *tb_next,
    PyFrameObject *frame);

#define EXCEPTION_TB_HEADER "Traceback (most recent call last):\n"
#define EXCEPTION_GROUP_TB_HEADER "Exception Group Traceback (most recent call last):\n"

/* Write the traceback tb to file f. Prefix each line with
   indent spaces followed by the margin (if it is not NULL). */
extern int _PyTraceBack_Print(
    PyObject *tb, const char *header, PyObject *f);
extern int _Py_WriteIndentedMargin(int, const char*, PyObject *);
extern int _Py_WriteIndent(int, PyObject *);

// Export for the faulthandler module
PyAPI_FUNC(void) _Py_InitDumpStack(void);
PyAPI_FUNC(void) _Py_DumpStack(int fd);

/* Capture C stack into buffer (up to size frames). Returns count, or 0 if unsupported. */
PyAPI_FUNC(int) _Py_GetBacktrace(void **buffer, int size);
/* Dump a previously captured backtrace array to fd. */
PyAPI_FUNC(void) _Py_DumpBacktraceFromArray(int fd, void *const *array, int size);

extern void _Py_DumpTraceback_Init(void);

/* Traceback frame info for signal-safe collection.
   This function is signal safe. */
#define Py_TRACEBACK_FRAME_FILENAME_MAX 256
#define Py_TRACEBACK_FRAME_NAME_MAX 256

typedef struct {
    char filename[Py_TRACEBACK_FRAME_FILENAME_MAX];
    int lineno;
    char name[Py_TRACEBACK_FRAME_NAME_MAX];
} PyTracebackFrameInfo;

/* Collect the traceback of a Python thread into an array of structs.
   Caller provides the array and max_frames. Returns the number of frames
   filled, or -1 if tstate is invalid/freed.

   This function is signal safe. No memory allocations or GIL releases.

   Export for _testinternalcapi. */
PyAPI_FUNC(int) _Py_GetTracebackFrames(
    PyThreadState *tstate,
    PyTracebackFrameInfo *frames,
    int max_frames);

/* Traceback interning table: deduplicates strings, frames, and tracebacks
   via reference-counted interning. Returns canonical IDs for each level. */

typedef struct _Py_traceback_interning_table Py_traceback_interning_table_t;

/* Opaque IDs (pointers to interned entries). Use for equality only. */
struct _Py_traceback_interned_string;
struct _Py_traceback_interned_frame;
struct _Py_traceback_interned_traceback;
typedef const struct _Py_traceback_interned_string *Py_traceback_string_id_t;
typedef const struct _Py_traceback_interned_frame *Py_traceback_frame_id_t;
typedef const struct _Py_traceback_interned_traceback *Py_traceback_id_t;

/* Allocator for interning table (e.g. use RawMalloc when called from allocator). */
typedef struct {
    void *(*malloc)(size_t size);
    void (*free)(void *ptr);
} Py_traceback_interning_allocator_t;

/* Create/destroy the interning table. allocator NULL = use PyMem_Malloc/Free. */
PyAPI_FUNC(Py_traceback_interning_table_t *) _Py_traceback_interning_table_new(
    const Py_traceback_interning_allocator_t *allocator);
PyAPI_FUNC(void) _Py_traceback_interning_table_free(Py_traceback_interning_table_t *table);

/* Dump traceback to fd (for heap profiler etc). */
PyAPI_FUNC(void) _Py_traceback_dump_id(
    Py_traceback_id_t traceback_id,
    Py_traceback_interning_table_t *table,
    int fd);

/* Fill frames array from traceback_id. Caller provides array and max_frames.
   Returns number of frames filled, or 0 if traceback_id is NULL or invalid. */
PyAPI_FUNC(int) _Py_traceback_fill_frames(
    Py_traceback_id_t traceback_id,
    Py_traceback_interning_table_t *table,
    PyTracebackFrameInfo *frames,
    int max_frames);

/* Frame info with string IDs instead of string copies. Use for O(1) dedup
   when building string tables (e.g. pprof export). */
typedef struct {
    Py_traceback_string_id_t filename_id;
    Py_traceback_string_id_t name_id;
    int lineno;
} PyTracebackFrameInfoWithIds;

/* Fill frames with string IDs. Same as _Py_traceback_fill_frames but returns
   IDs for O(1) lookup instead of copying strings. */
PyAPI_FUNC(int) _Py_traceback_fill_frames_with_string_ids(
    Py_traceback_id_t traceback_id,
    Py_traceback_interning_table_t *table,
    PyTracebackFrameInfoWithIds *frames,
    int max_frames);

/* Fill frame_ids array from traceback_id. Caller provides array and max_frames.
   Returns number of frames filled. Use for OTel export to get location indices. */
PyAPI_FUNC(int) _Py_traceback_fill_frame_ids(
    Py_traceback_id_t traceback_id,
    Py_traceback_interning_table_t *table,
    Py_traceback_frame_id_t *frame_ids,
    int max_frames);

/* Get string from string_id. Asserts if id is NULL. */
PyAPI_FUNC(const char *) _Py_traceback_string_id_get_str(
    Py_traceback_string_id_t string_id);

/* Build export string table: iterate all interned strings, call add_fn(ctx, str)
   for each, store returned index in string's export_index scratch. Call once
   at export start. Returns 0 on success, -1 on error (add_fn returned -1). */
typedef int64_t (*_Py_traceback_export_add_string_fn)(void *ctx, const char *s);
PyAPI_FUNC(int) _Py_traceback_build_export_string_table(
    Py_traceback_interning_table_t *table,
    void *ctx,
    _Py_traceback_export_add_string_fn add_fn);

/* Get pprof/otel string table index for string_id. Returns null_index if
   string_id is NULL. Asserts export_index was set (build called first). */
PyAPI_FUNC(int64_t) _Py_traceback_string_id_get_export_index(
    Py_traceback_string_id_t string_id,
    int64_t null_index);

/* Clear export_index scratch on all interned strings. Call when done with export. */
PyAPI_FUNC(void) _Py_traceback_clear_export_indices(
    Py_traceback_interning_table_t *table);

/* Build export frame table: iterate all interned frames, call add_fn(ctx,
   filename_idx, name_idx, lineno) for each, store returned (function_idx,
   location_idx) in frame's export scratch. Call after build_export_string_table.
   null_idx: string table index for NULL filename/name (e.g. "???").
   add_fn returns (function_idx << 32) | location_idx, or -1 on error. */
typedef int64_t (*_Py_traceback_export_add_frame_fn)(void *ctx,
    int64_t filename_idx, int64_t name_idx, int lineno);
PyAPI_FUNC(int) _Py_traceback_build_export_frame_table(
    Py_traceback_interning_table_t *table,
    void *ctx,
    _Py_traceback_export_add_frame_fn add_fn,
    int64_t null_idx);

/* Get OTel function/location indices for frame_id. Returns 0 for both if NULL.
   Asserts export indices were set (build called first). */
PyAPI_FUNC(void) _Py_traceback_frame_id_get_export_indices(
    Py_traceback_frame_id_t frame_id,
    uint32_t *out_function_index,
    uint32_t *out_location_index);

/* Clear export frame indices on all interned frames. Call when done with export. */
PyAPI_FUNC(void) _Py_traceback_clear_export_frame_indices(
    Py_traceback_interning_table_t *table);

/* Intern a traceback. Returns traceback_id with refcount 1, or NULL on failure.
   Interns strings -> string_ids, frames -> frame_ids, then the frame list -> traceback_id. */
PyAPI_FUNC(Py_traceback_id_t) _Py_traceback_intern(
    const PyTracebackFrameInfo *frames,
    int count,
    Py_traceback_interning_table_t *table);

/* Retain a traceback_id. Call when creating another reference. */
PyAPI_FUNC(void) _Py_traceback_retain(
    Py_traceback_id_t traceback_id,
    Py_traceback_interning_table_t *table);

/* Release a traceback_id. Call when done with the id. */
PyAPI_FUNC(void) _Py_traceback_release(
    Py_traceback_id_t traceback_id,
    Py_traceback_interning_table_t *table);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_TRACEBACK_H */
