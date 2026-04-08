#ifndef Py_CPYTHON_TRACEBACK_H
#  error "this header file must not be included directly"
#endif

typedef struct _traceback PyTracebackObject;

struct _traceback {
    PyObject_HEAD
    PyTracebackObject *tb_next;
    PyFrameObject *tb_frame;
    int tb_lasti;
    int tb_lineno;
};

/* Buffer size for the filename and name fields in PyUnstable_FrameInfo:
   up to 500 content bytes plus "..." (3) plus '\0' (1). */
#define Py_UNSTABLE_FRAMEINFO_STRSIZE 504

/* Structured, plain-data representation of a single Python frame.
   PyUnstable_CollectTraceback() and PyUnstable_PrintTraceback() do not
   acquire or release the GIL or allocate heap memory, so they can be called
   from signal handlers and are suitable for low-overhead observability tools
   such as sampling profilers and tracers.

   Populated by PyUnstable_CollectTraceback().  filename and name are
   ASCII-encoded (non-ASCII characters are backslash-escaped) and
   null-terminated; they are empty strings if the corresponding code
   attribute is missing or not a unicode object.  lineno is -1 when it
   cannot be determined. */
typedef struct {
    char filename[Py_UNSTABLE_FRAMEINFO_STRSIZE];
    int lineno;
    char name[Py_UNSTABLE_FRAMEINFO_STRSIZE];
} PyUnstable_FrameInfo;

/* Collect up to max_frames frames from tstate into the caller-supplied
   frames array and return the number of frames written (0..max_frames).
   Returns -1 if tstate is freed or has no current Python frame.

   The filename and function names are encoded to ASCII with backslashreplace
   and truncated to 500 characters.

   Because it does not acquire or release the GIL or allocate heap memory, this
   function is suitable for low-overhead observability tools such as sampling
   profilers and tracers, and can be called from signal handlers.

   In crash scenarios such as signal handlers for SIGSEGV, where the
   interpreter may be in an inconsistent state, the function might produce
   incomplete output or it may even crash itself.

   The caller does not need to hold an attached thread state, nor does tstate
   need to be attached.

   This function does not acquire or release the GIL, modify reference counts,
   or allocate heap memory. */
PyAPI_FUNC(int) PyUnstable_CollectTraceback(
    PyThreadState *tstate,
    PyUnstable_FrameInfo *frames,
    int max_frames);

/* Write a traceback collected by PyUnstable_CollectTraceback() to fd.
   The format looks like:

      Stack (most recent call first):
        File "xxx", line xxx in <xxx>
        File "xxx", line xxx in <xxx>
        ...

   Pass write_header=1 to emit the "Stack (most recent call first):" header
   line, or write_header=0 to omit it.

   This function only reads the caller-supplied frames array and does not
   access interpreter state.  It is async-signal-safe: it does not acquire or
   release the GIL, modify reference counts, or allocate heap memory, and its
   only I/O is via write(2). */
PyAPI_FUNC(void) PyUnstable_PrintTraceback(
    int fd,
    const PyUnstable_FrameInfo *frames,
    int n_frames,
    int write_header);
