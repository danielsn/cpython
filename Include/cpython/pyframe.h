#ifndef Py_CPYTHON_PYFRAME_H
#  error "this header file must not be included directly"
#endif

PyAPI_DATA(PyTypeObject) PyFrame_Type;
PyAPI_DATA(PyTypeObject) PyFrameLocalsProxy_Type;

#define PyFrame_Check(op) Py_IS_TYPE((op), &PyFrame_Type)
#define PyFrameLocalsProxy_Check(op) Py_IS_TYPE((op), &PyFrameLocalsProxy_Type)

PyAPI_FUNC(PyFrameObject *) PyFrame_GetBack(PyFrameObject *frame);
PyAPI_FUNC(PyObject *) PyFrame_GetLocals(PyFrameObject *frame);

PyAPI_FUNC(PyObject *) PyFrame_GetGlobals(PyFrameObject *frame);
PyAPI_FUNC(PyObject *) PyFrame_GetBuiltins(PyFrameObject *frame);

PyAPI_FUNC(PyObject *) PyFrame_GetGenerator(PyFrameObject *frame);
PyAPI_FUNC(int) PyFrame_GetLasti(PyFrameObject *frame);
PyAPI_FUNC(PyObject*) PyFrame_GetVar(PyFrameObject *frame, PyObject *name);
PyAPI_FUNC(PyObject*) PyFrame_GetVarString(PyFrameObject *frame, const char *name);

/* The following functions are for use by debuggers and other tools
 * implementing custom frame evaluators with PEP 523. */

struct _PyInterpreterFrame;

/* Returns the code object of the frame (strong reference).
 * Does not raise an exception.
 * If allocation and reference count changes are not permitted, use
 * PyUnstable_InterpreterFrame_BorrowCode instead. */
PyAPI_FUNC(PyObject *) PyUnstable_InterpreterFrame_GetCode(struct _PyInterpreterFrame *frame);

/* Returns the code object of the frame as a borrowed reference.
 * The reference is valid as long as the frame is alive.
 * Use instead of PyUnstable_InterpreterFrame_GetCode when allocation and
 * reference count changes are not permitted (e.g. from a signal handler or
 * a custom memory allocator).  Does not allocate, does not change any
 * reference counts, does not acquire or release the GIL, does not raise an
 * exception.  Uses heuristics to detect freed memory; not 100% reliable. */
PyAPI_FUNC(PyObject *) PyUnstable_InterpreterFrame_BorrowCode(struct _PyInterpreterFrame *frame);

/* Returns a byte offset into the last executed instruction.
 * Does not raise an exception. */
PyAPI_FUNC(int) PyUnstable_InterpreterFrame_GetLasti(struct _PyInterpreterFrame *frame);

/* Returns the currently executing line number, or -1 if there is no line number.
 * Does not raise an exception. */
PyAPI_FUNC(int) PyUnstable_InterpreterFrame_GetLine(struct _PyInterpreterFrame *frame);

/* Returns the line number for the given byte offset in a code object.
 * addr is a byte offset as returned by PyUnstable_InterpreterFrame_GetLasti.
 * Returns -1 if no line number can be determined.  Does not raise an exception.
 * Unlike PyCode_Addr2Line, validates addr before accessing the line table
 * rather than asserting it, making it safe to call when the frame state may
 * be partially torn down. */
PyAPI_FUNC(int) PyUnstable_Code_GetLineNumber(PyCodeObject *code, int addr);

/* Returns a borrowed reference to the filename (co_filename) of a code
 * object, or NULL if not set.  The reference is valid as long as the code
 * object is alive.  Does not allocate, does not change any reference counts,
 * does not acquire or release the GIL, does not raise an exception.
 * Safe to call from signal handlers. */
PyAPI_FUNC(PyObject *) PyUnstable_Code_BorrowFilename(PyCodeObject *code);

/* Returns a borrowed reference to the function name (co_name) of a code
 * object, or NULL if not set.  The reference is valid as long as the code
 * object is alive.  Does not allocate, does not change any reference counts,
 * does not acquire or release the GIL, does not raise an exception.
 * Safe to call from signal handlers. */
PyAPI_FUNC(PyObject *) PyUnstable_Code_BorrowName(PyCodeObject *code);

/* Returns the current interpreter frame of the thread state, or NULL if the
 * thread has no current frame or freed memory is detected.
 * The returned frame may be incomplete; use
 * PyUnstable_InterpreterFrame_IsIncomplete to skip such frames during stack
 * walking.
 * Does not allocate memory, does not acquire or release the GIL, does not
 * raise an exception.  Safe to call from signal handlers; racy reads from
 * other threads are intentional and suppressed (_Py_NO_SANITIZE_THREAD).
 * Uses heuristics to detect freed memory; not 100% reliable. */
PyAPI_FUNC(struct _PyInterpreterFrame *)
PyUnstable_ThreadState_GetInterpreterFrame(PyThreadState *tstate);

/* Returns the previous (calling) frame, or NULL if frame is the outermost
 * frame or freed memory is detected.
 * The returned frame may be incomplete; use
 * PyUnstable_InterpreterFrame_IsIncomplete to skip such frames during stack
 * walking.
 * Does not allocate memory, does not acquire or release the GIL, does not
 * raise an exception.  Safe to call from signal handlers; racy reads from
 * other threads are intentional and suppressed (_Py_NO_SANITIZE_THREAD).
 * Uses heuristics to detect freed memory; not 100% reliable. */
PyAPI_FUNC(struct _PyInterpreterFrame *)
PyUnstable_InterpreterFrame_GetBack(struct _PyInterpreterFrame *frame);

/* Returns non-zero if the frame is an interpreter entry frame — an internal
 * trampoline inserted when C code calls into Python.  Always implies
 * PyUnstable_InterpreterFrame_IsIncomplete; use IsEntry only to distinguish
 * entry frames from other incomplete frames.
 * Does not allocate memory, does not acquire or release the GIL, does not
 * raise an exception.  Safe to call from signal handlers. */
PyAPI_FUNC(int) PyUnstable_InterpreterFrame_IsEntry(struct _PyInterpreterFrame *frame);

/* Returns non-zero if the frame is incomplete and should be skipped during
 * stack walking.  Covers interpreter entry frames and frames that have not
 * yet begun executing.
 * Does not allocate memory, does not acquire or release the GIL, does not
 * raise an exception.  Safe to call from signal handlers. */
PyAPI_FUNC(int) PyUnstable_InterpreterFrame_IsIncomplete(struct _PyInterpreterFrame *frame);

#define PyUnstable_EXECUTABLE_KIND_SKIP 0
#define PyUnstable_EXECUTABLE_KIND_PY_FUNCTION 1
#define PyUnstable_EXECUTABLE_KIND_BUILTIN_FUNCTION 3
#define PyUnstable_EXECUTABLE_KIND_METHOD_DESCRIPTOR 4
#define PyUnstable_EXECUTABLE_KINDS 5

PyAPI_DATA(const PyTypeObject *) const PyUnstable_ExecutableKinds[PyUnstable_EXECUTABLE_KINDS+1];
